"""Plate-crop OCR stress guard for the frozen low-quality top-3 selector."""

from __future__ import annotations

import argparse
import json
import statistics
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .low_quality_candidate_selector import (
    SELECTOR_FEATURES,
    _candidate_icon_probability,
    _deserialize_side_model,
    predict_selector,
)
from .low_quality_presence_rectification import (
    LOW_QUALITY_ENVIRONMENTS,
    QUALITY_THRESHOLD,
    TARGET_SIZE,
    _build_template_cache,
    _corrected_icon_records,
    apply_environment,
)
from .ocr_rectification_guard import _ocr, _percentile, levenshtein
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .roi_annotation import read_csv
from .top3_localization import ranked_candidates, read_image
from .low_quality_vehicle_presence_calibration import _write_csv


EXPERIMENT_ID = "low-quality-frozen-selector-ocr-stress-015-rerun-001"
VARIANTS = ("quality_aware_whole_crop", "frozen_selector_top3")


def selector_model_from_summary(summary: dict) -> dict:
    source = summary["frozen_model"]
    if tuple(source.get("feature_names", ())) != SELECTOR_FEATURES:
        raise ValueError("frozen_selector_feature_schema_mismatch")
    return {
        "mean": np.asarray(source["mean"], dtype=np.float64),
        "scale": np.asarray(source["scale"], dtype=np.float64),
        "coefficients": np.asarray(source["coefficients"], dtype=np.float64),
    }


def build_candidate_features(
    candidate: dict,
    rank: int,
    image_shape: tuple[int, ...],
    rectification_method: str,
    geometry_confidence: float,
    output_quality: float,
    input_quality: float,
    icon_probabilities: tuple[float, float],
) -> dict[str, float]:
    height, width = image_shape[:2]
    x1, y1, x2, y2 = [int(value) for value in candidate["bbox"]]
    bbox_width = max(1, x2 - x1)
    bbox_height = max(1, y2 - y1)
    features = {
        "generator_score": float(candidate["score"]),
        "rank_inverse": 1.0 / rank,
        "bbox_area_ratio": bbox_width * bbox_height / float(max(1, width * height)),
        "bbox_aspect": bbox_width / float(bbox_height),
        "origin_geometry": float(candidate["origin"] == "geometry"),
        "origin_character_edge": float(candidate["origin"] == "character_edge"),
        "origin_character_group": float(candidate["origin"] == "character_group"),
        "method_perspective": float(rectification_method == "perspective"),
        "method_rotated_rect": float(rectification_method == "rotated_rect"),
        "geometry_confidence": float(geometry_confidence),
        "rectification_quality": float(output_quality),
        "input_quality": float(input_quality),
        "icon_probability_max": max(icon_probabilities),
        "icon_probability_min": min(icon_probabilities),
        "icon_probability_mean": statistics.mean(icon_probabilities),
        "icon_probability_gap": abs(icon_probabilities[0] - icon_probabilities[1]),
    }
    if tuple(features) != SELECTOR_FEATURES:
        raise ValueError("candidate_feature_order_mismatch")
    return features


def choose_frozen_candidate(candidates: list[dict]) -> dict | None:
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda item: (float(item["frozen_selector_probability"]), -int(item["candidate_rank"])),
    )


def _template_context(split_manifest: Path, icon_manifest: Path) -> tuple[dict, list[str]]:
    split_rows = {row["annotation_id"]: row for row in read_csv(split_manifest)}
    icon_rows = read_csv(icon_manifest)
    template_rows, _ = _corrected_icon_records(icon_rows, split_rows, include_test_labels=False)
    test_ids = {key for key, value in split_rows.items() if value.get("split") == "test"}
    overlap = sorted({row["annotation_id"] for row in template_rows} & test_ids)
    return _build_template_cache(template_rows), overlap


def _evaluate_whole_crop(
    degraded: np.ndarray,
    ground_truth: str,
    tessdata_dir: Path,
    executable: Path,
    timeout_seconds: float,
) -> dict:
    started = time.perf_counter()
    rectification_started = time.perf_counter()
    result = rectify_plate(degraded, TARGET_SIZE)
    rectification_ms = (time.perf_counter() - rectification_started) * 1000.0
    pre_ocr_ms = (time.perf_counter() - started) * 1000.0
    ocr_started = time.perf_counter()
    text, error = _ocr(result.plate_image, tessdata_dir, executable, timeout_seconds)
    ocr_ms = (time.perf_counter() - ocr_started) * 1000.0
    distance = levenshtein(ground_truth, text)
    return {
        "pipeline_status": "selected",
        "candidate_count": 1,
        "no_candidate": 0,
        "selected_candidate_rank": 0,
        "selected_candidate_origin": "whole_crop",
        "selected_candidate_bbox": [0, 0, degraded.shape[1], degraded.shape[0]],
        "selected_bbox_width_ratio": 1.0,
        "selected_bbox_height_ratio": 1.0,
        "selected_bbox_area_ratio": 1.0,
        "selected_selector_probability": "",
        "rectification_status": result.status,
        "rectification_method": result.method,
        "geometric_success": int(result.method in GEOMETRIC_METHODS),
        "geometry_confidence": float(result.rect_quality_input),
        "output_quality": rectification_quality_score(result.plate_image),
        "ocr_text": text,
        "exact_match": int(text == ground_truth and not error),
        "edit_distance": distance,
        "cer": distance / max(1, len(ground_truth)),
        "error": error,
        "candidate_generation_ms": 0.0,
        "candidate_evaluation_ms": 0.0,
        "selection_ms": 0.0,
        "rectification_ms": rectification_ms,
        "pre_ocr_ms": pre_ocr_ms,
        "ocr_ms": ocr_ms,
        "total_ms": (time.perf_counter() - started) * 1000.0,
    }


def _evaluate_selector(
    degraded: np.ndarray,
    source_id: str,
    ground_truth: str,
    thresholds: dict,
    template_cache: dict,
    side_models: dict,
    selector_model: dict,
    tessdata_dir: Path,
    executable: Path,
    timeout_seconds: float,
) -> tuple[dict, list[dict]]:
    started = time.perf_counter()
    generation_started = time.perf_counter()
    candidates = ranked_candidates(
        degraded,
        thresholds,
        origins=("geometry", "character_edge"),
        limit=3,
        adaptive=True,
        component_group=True,
        reserve_coarse=True,
    )
    generation_ms = (time.perf_counter() - generation_started) * 1000.0
    input_quality = rectification_quality_score(
        cv2.resize(degraded, TARGET_SIZE, interpolation=cv2.INTER_AREA)
    )
    evaluation_started = time.perf_counter()
    evaluated: list[dict] = []
    plate_images: list[np.ndarray] = []
    for rank, candidate in enumerate(candidates, 1):
        x1, y1, x2, y2 = [int(value) for value in candidate["bbox"]]
        crop = degraded[y1:y2, x1:x2]
        if crop.size == 0:
            continue
        rectified = rectify_plate(crop, TARGET_SIZE)
        output_quality = rectification_quality_score(rectified.plate_image)
        base = {
            "rectification_geometry_confidence": float(rectified.rect_quality_input),
            "rectification_quality": float(output_quality),
            "quality_aware_fallback_used": 1.0,
        }
        icon_probabilities = tuple(
            _candidate_icon_probability(
                rectified.plate_image,
                side,
                template_cache,
                "validation",
                source_id,
                "absent",
                side_models[side],
                base,
            )
            for side in ("left", "right")
        )
        features = build_candidate_features(
            candidate,
            rank,
            degraded.shape,
            rectified.method,
            float(rectified.rect_quality_input),
            float(output_quality),
            float(input_quality),
            icon_probabilities,
        )
        selector_input = dict(features)
        probability = predict_selector(selector_model, selector_input)
        width_ratio = max(0, x2 - x1) / float(max(1, degraded.shape[1]))
        height_ratio = max(0, y2 - y1) / float(max(1, degraded.shape[0]))
        evaluated.append(
            {
                "candidate_rank": rank,
                "candidate_origin": candidate["origin"],
                "candidate_bbox": candidate["bbox"],
                "frozen_selector_probability": probability,
                "rectification_status": rectified.status,
                "rectification_method": rectified.method,
                "geometric_success": int(rectified.method in GEOMETRIC_METHODS),
                "geometry_confidence": float(rectified.rect_quality_input),
                "output_quality": float(output_quality),
                "left_icon_probability": icon_probabilities[0],
                "right_icon_probability": icon_probabilities[1],
                "bbox_width_ratio": width_ratio,
                "bbox_height_ratio": height_ratio,
                **features,
            }
        )
        plate_images.append(rectified.plate_image)
    evaluation_ms = (time.perf_counter() - evaluation_started) * 1000.0
    selection_started = time.perf_counter()
    selected = choose_frozen_candidate(evaluated)
    selection_ms = (time.perf_counter() - selection_started) * 1000.0
    for item in evaluated:
        item["selected"] = int(item is selected)
    pre_ocr_ms = (time.perf_counter() - started) * 1000.0
    if selected is None:
        distance = levenshtein(ground_truth, "")
        return (
            {
                "pipeline_status": "no_candidate",
                "candidate_count": len(candidates),
                "no_candidate": 1,
                "selected_candidate_rank": "",
                "selected_candidate_origin": "",
                "selected_candidate_bbox": "",
                "selected_bbox_width_ratio": 0.0,
                "selected_bbox_height_ratio": 0.0,
                "selected_bbox_area_ratio": 0.0,
                "selected_selector_probability": "",
                "rectification_status": "no_candidate",
                "rectification_method": "none",
                "geometric_success": 0,
                "geometry_confidence": 0.0,
                "output_quality": 0.0,
                "ocr_text": "",
                "exact_match": 0,
                "edit_distance": distance,
                "cer": distance / max(1, len(ground_truth)),
                "error": "",
                "candidate_generation_ms": generation_ms,
                "candidate_evaluation_ms": evaluation_ms,
                "selection_ms": selection_ms,
                "rectification_ms": evaluation_ms,
                "pre_ocr_ms": pre_ocr_ms,
                "ocr_ms": 0.0,
                "total_ms": (time.perf_counter() - started) * 1000.0,
            },
            evaluated,
        )
    selected_index = evaluated.index(selected)
    ocr_started = time.perf_counter()
    text, error = _ocr(plate_images[selected_index], tessdata_dir, executable, timeout_seconds)
    ocr_ms = (time.perf_counter() - ocr_started) * 1000.0
    distance = levenshtein(ground_truth, text)
    bbox = [int(value) for value in selected["candidate_bbox"]]
    area_ratio = float(selected["bbox_width_ratio"]) * float(selected["bbox_height_ratio"])
    return (
        {
            "pipeline_status": "selected",
            "candidate_count": len(evaluated),
            "no_candidate": 0,
            "selected_candidate_rank": selected["candidate_rank"],
            "selected_candidate_origin": selected["candidate_origin"],
            "selected_candidate_bbox": bbox,
            "selected_bbox_width_ratio": selected["bbox_width_ratio"],
            "selected_bbox_height_ratio": selected["bbox_height_ratio"],
            "selected_bbox_area_ratio": area_ratio,
            "selected_selector_probability": selected["frozen_selector_probability"],
            "rectification_status": selected["rectification_status"],
            "rectification_method": selected["rectification_method"],
            "geometric_success": selected["geometric_success"],
            "geometry_confidence": selected["geometry_confidence"],
            "output_quality": selected["output_quality"],
            "ocr_text": text,
            "exact_match": int(text == ground_truth and not error),
            "edit_distance": distance,
            "cer": distance / max(1, len(ground_truth)),
            "error": error,
            "candidate_generation_ms": generation_ms,
            "candidate_evaluation_ms": evaluation_ms,
            "selection_ms": selection_ms,
            "rectification_ms": evaluation_ms,
            "pre_ocr_ms": pre_ocr_ms,
            "ocr_ms": ocr_ms,
            "total_ms": (time.perf_counter() - started) * 1000.0,
        },
        evaluated,
    )


def summarize_rows(rows: list[dict], environment: str, variant: str) -> dict:
    selected = [row for row in rows if row["variant"] == variant]
    if environment == "synthetic_low_quality":
        selected = [row for row in selected if row["environment"] in LOW_QUALITY_ENVIRONMENTS]
    elif environment == "quality_below_0_25":
        selected = [row for row in selected if int(row["quality_below_threshold"])]
    else:
        selected = [row for row in selected if row["environment"] == environment]
    valid = [row for row in selected if not row["error"]]
    return {
        "split": "validation",
        "scope": "plate_crop_stress_guard",
        "environment": environment,
        "variant": variant,
        "count": len(selected),
        "unique_source_count": len({row["source_id"] for row in selected}),
        "quality_below_count": sum(int(row["quality_below_threshold"]) for row in selected),
        "exact_match_rate": sum(int(row["exact_match"]) for row in valid) / len(valid) if valid else 0.0,
        "mean_cer": statistics.mean(float(row["cer"]) for row in valid) if valid else 0.0,
        "geometric_success": sum(int(row["geometric_success"]) for row in valid) / len(valid)
        if valid
        else 0.0,
        "no_candidate_rate": sum(int(row["no_candidate"]) for row in valid) / len(valid)
        if valid
        else 0.0,
        "mean_selected_bbox_width_ratio": statistics.mean(
            float(row["selected_bbox_width_ratio"]) for row in valid
        )
        if valid
        else 0.0,
        "mean_pre_ocr_ms": statistics.mean(float(row["pre_ocr_ms"]) for row in valid)
        if valid
        else 0.0,
        "p95_pre_ocr_ms": _percentile([float(row["pre_ocr_ms"]) for row in valid], 0.95),
        "mean_ocr_ms": statistics.mean(float(row["ocr_ms"]) for row in valid) if valid else 0.0,
        "p95_ocr_ms": _percentile([float(row["ocr_ms"]) for row in valid], 0.95),
        "mean_total_ms": statistics.mean(float(row["total_ms"]) for row in valid) if valid else 0.0,
        "p95_total_ms": _percentile([float(row["total_ms"]) for row in valid], 0.95),
        "error_count": len(selected) - len(valid),
    }


def _report(summary: dict, lookup: dict[tuple[str, str], dict]) -> str:
    baseline = lookup[("synthetic_low_quality", "quality_aware_whole_crop")]
    selector = lookup[("synthetic_low_quality", "frozen_selector_top3")]
    low_baseline = lookup[("quality_below_0_25", "quality_aware_whole_crop")]
    low_selector = lookup[("quality_below_0_25", "frozen_selector_top3")]
    gates = "\n".join(
        f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()
    )
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Scope: `plate_crop_stress_guard` (full-frame selector OCR proof가 아님)
- OCR Validation / Test accessed: {summary['source_count']} / `false`
- EV Formal Test accessed: `false`
- Step 92 published quality<0.25 / matched 4-environment rows: {summary['prior_published_quality_below_count']} / {summary['prior_matched_low_quality_environment_count']} (`normal` {summary['prior_published_quality_below_included_normal_rows']}행 제외)

## Synthetic low-quality

| Variant | Exact | CER | Geometric | No candidate | Mean/P95 pre-OCR ms | Mean total ms |
|---|---:|---:|---:|---:|---:|---:|
| whole crop baseline | {baseline['exact_match_rate']:.2%} | {baseline['mean_cer']:.4f} | {baseline['geometric_success']:.2%} | {baseline['no_candidate_rate']:.2%} | {baseline['mean_pre_ocr_ms']:.2f}/{baseline['p95_pre_ocr_ms']:.2f} | {baseline['mean_total_ms']:.2f} |
| frozen selector top-3 | {selector['exact_match_rate']:.2%} | {selector['mean_cer']:.4f} | {selector['geometric_success']:.2%} | {selector['no_candidate_rate']:.2%} | {selector['mean_pre_ocr_ms']:.2f}/{selector['p95_pre_ocr_ms']:.2f} | {selector['mean_total_ms']:.2f} |

## Quality below 0.25

| Variant | Rows | Exact | CER | Geometric |
|---|---:|---:|---:|---:|
| whole crop baseline | {low_baseline['count']} | {low_baseline['exact_match_rate']:.2%} | {low_baseline['mean_cer']:.4f} | {low_baseline['geometric_success']:.2%} |
| frozen selector top-3 | {low_selector['count']} | {low_selector['exact_match_rate']:.2%} | {low_selector['mean_cer']:.4f} | {low_selector['geometric_success']:.2%} |

## Gate

{gates}

이 평가는 이미 잘린 일반 번호판 crop에서 frozen selector를 한 번 더 적용하는 보수적 stress guard다. 실패 시 selector를 OCR 입력에 직접 연결하지 않으며, 실제 full-frame OCR GT·EV·RGB/IR·독립 holdout·Pi latency를 별도로 검증해야 한다.
"""


def run(args: argparse.Namespace) -> dict:
    selector_summary = json.loads(Path(args.selector_summary).resolve().read_text(encoding="utf-8-sig"))
    side_summary = json.loads(Path(args.side_model_summary).resolve().read_text(encoding="utf-8-sig"))
    prior_ocr_summary = json.loads(Path(args.prior_ocr_summary).resolve().read_text(encoding="utf-8-sig"))
    selector_model = selector_model_from_summary(selector_summary)
    side_models = {side: _deserialize_side_model(side_summary, side) for side in ("left", "right")}
    template_cache, template_test_overlap = _template_context(
        Path(args.split_manifest).resolve(), Path(args.icon_manifest).resolve()
    )
    thresholds = json.loads(Path(args.thresholds).resolve().read_text(encoding="utf-8-sig"))
    tessdata_dir = Path(args.tessdata).resolve()
    executable = Path(args.tesseract).resolve()
    if not executable.is_file() or not (tessdata_dir / "kor.traineddata").is_file():
        raise FileNotFoundError("tesseract_or_kor_traineddata_missing")
    sources = read_csv(Path(args.ocr_manifest).resolve())
    if len(sources) != args.sample_size:
        raise ValueError(f"ocr_manifest_size:{len(sources)}")
    if any(source.get("split") != "validation" for source in sources):
        raise ValueError("non_validation_source_in_ocr_stress")
    detail_rows: list[dict] = []
    candidate_rows: list[dict] = []
    errors: list[dict] = []
    manifest_rows: list[dict] = []
    for source in sources:
        source_id = source["source_id"]
        input_path = Path(source["input_path"])
        ground_truth = source["ground_truth"]
        manifest_rows.append(
            {
                "source_id": source_id,
                "vehicle_id": source["vehicle_id"],
                "split": "validation",
                "ground_truth": ground_truth,
                "input_path": str(input_path),
                "plate_crop_stress_source": 1,
                "formal_test_member": 0,
            }
        )
        image = read_image(input_path)
        if image is None:
            errors.append({"source_id": source_id, "reason": "unreadable_input"})
            continue
        for environment in sorted(LOW_QUALITY_ENVIRONMENTS):
            degradation_started = time.perf_counter()
            degraded = apply_environment(image, environment)
            degradation_ms = (time.perf_counter() - degradation_started) * 1000.0
            normalized = cv2.resize(degraded, TARGET_SIZE, interpolation=cv2.INTER_AREA)
            input_quality = rectification_quality_score(normalized)
            for variant in VARIANTS:
                try:
                    if variant == "quality_aware_whole_crop":
                        result = _evaluate_whole_crop(
                            degraded,
                            ground_truth,
                            tessdata_dir,
                            executable,
                            args.ocr_timeout_seconds,
                        )
                        evaluated_candidates: list[dict] = []
                    else:
                        result, evaluated_candidates = _evaluate_selector(
                            degraded,
                            source_id,
                            ground_truth,
                            thresholds,
                            template_cache,
                            side_models,
                            selector_model,
                            tessdata_dir,
                            executable,
                            args.ocr_timeout_seconds,
                        )
                    detail_rows.append(
                        {
                            "experiment_id": args.experiment_id,
                            "source_id": source_id,
                            "vehicle_id": source["vehicle_id"],
                            "split": "validation",
                            "scope": "plate_crop_stress_guard",
                            "environment": environment,
                            "variant": variant,
                            "ground_truth": ground_truth,
                            "input_path": str(input_path),
                            "input_quality": input_quality,
                            "quality_below_threshold": int(input_quality < QUALITY_THRESHOLD),
                            "degradation_ms": degradation_ms,
                            **result,
                        }
                    )
                    for candidate in evaluated_candidates:
                        candidate_rows.append(
                            {
                                "experiment_id": args.experiment_id,
                                "source_id": source_id,
                                "environment": environment,
                                "input_path": str(input_path),
                                **candidate,
                            }
                        )
                except (cv2.error, KeyError, TypeError, ValueError) as exc:
                    errors.append(
                        {
                            "source_id": source_id,
                            "environment": environment,
                            "variant": variant,
                            "reason": f"{type(exc).__name__}:{str(exc).splitlines()[0][:160]}",
                        }
                    )
    environments = [*sorted(LOW_QUALITY_ENVIRONMENTS), "synthetic_low_quality", "quality_below_0_25"]
    metrics = [
        summarize_rows(detail_rows, environment, variant)
        for environment in environments
        for variant in VARIANTS
    ]
    lookup = {(row["environment"], row["variant"]): row for row in metrics}
    baseline = lookup[("synthetic_low_quality", "quality_aware_whole_crop")]
    selector = lookup[("synthetic_low_quality", "frozen_selector_top3")]
    low_baseline = lookup[("quality_below_0_25", "quality_aware_whole_crop")]
    low_selector = lookup[("quality_below_0_25", "frozen_selector_top3")]
    prior_metrics = read_csv(Path(args.prior_ocr_environment_summary).resolve())
    prior_lookup = {(row["environment"], row["variant"]): row for row in prior_metrics}
    prior = prior_lookup[("synthetic_low_quality", "quality_aware")]
    prior_detail_rows = read_csv(Path(args.prior_ocr_details).resolve())
    prior_low = [
        row
        for row in prior_detail_rows
        if row.get("variant") == "quality_aware"
        and row.get("environment") in LOW_QUALITY_ENVIRONMENTS
        and int(row.get("quality_below_threshold", 0))
        and not row.get("error")
    ]
    prior_low_exact = sum(int(row["exact_match"]) for row in prior_low) / len(prior_low)
    prior_low_cer = statistics.mean(float(row["cer"]) for row in prior_low)
    gate = {
        "synthetic_exact_match_non_regression": selector["exact_match_rate"]
        >= baseline["exact_match_rate"],
        "synthetic_cer_non_regression": selector["mean_cer"] <= baseline["mean_cer"],
        "quality_below_exact_match_non_regression": low_selector["exact_match_rate"]
        >= low_baseline["exact_match_rate"],
        "quality_below_cer_non_regression": low_selector["mean_cer"] <= low_baseline["mean_cer"],
        "baseline_synthetic_exact_reproduces_step92": baseline["exact_match_rate"]
        == float(prior["exact_match_rate"]),
        "baseline_synthetic_cer_reproduces_step92": abs(
            baseline["mean_cer"] - float(prior["mean_cer"])
        )
        <= 1e-9,
        "baseline_quality_below_exact_reproduces_step92": low_baseline["exact_match_rate"]
        == prior_low_exact,
        "baseline_quality_below_cer_reproduces_step92": abs(
            low_baseline["mean_cer"] - prior_low_cer
        )
        <= 1e-9,
        "selector_no_candidate_rate_le_0_05": selector["no_candidate_rate"] <= 0.05,
        "selector_geometric_regression_le_0_05": selector["geometric_success"]
        >= baseline["geometric_success"] - 0.05,
        "selector_pre_ocr_mean_le_350ms": selector["mean_pre_ocr_ms"] <= 350.0,
        "selector_pre_ocr_p95_le_600ms": selector["p95_pre_ocr_ms"] <= 600.0,
        "selector_total_mean_le_2x_baseline": selector["mean_total_ms"]
        <= 2.0 * baseline["mean_total_ms"],
        "processing_errors_zero": not errors
        and baseline["error_count"] == 0
        and selector["error_count"] == 0,
        "source_split_validation_only": all(source["split"] == "validation" for source in manifest_rows),
        "template_test_overlap_zero": not template_test_overlap,
        "prior_selector_gates_passed": selector_summary.get("status") == "selector_development_candidate"
        and all(selector_summary.get("gate", {}).values()),
        "prior_ocr_guard_passed": prior_ocr_summary.get("status") == "ocr_guard_pass"
        and all(prior_ocr_summary.get("gate", {}).values()),
        "frozen_selector_feature_schema_match": tuple(selector_summary["frozen_model"]["feature_names"])
        == SELECTOR_FEATURES,
        "formal_test_rows_zero": not any(source.get("split") == "test" for source in manifest_rows),
    }
    status = "selector_ocr_stress_guard_pass" if all(gate.values()) else "selector_ocr_stress_guard_fail"
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "scope": "plate_crop_stress_guard",
        "source_count": len(manifest_rows),
        "detail_rows": len(detail_rows),
        "candidate_detail_rows": len(candidate_rows),
        "quality_threshold": QUALITY_THRESHOLD,
        "environments": sorted(LOW_QUALITY_ENVIRONMENTS),
        "variants": list(VARIANTS),
        "selector_experiment_id": selector_summary.get("experiment_id"),
        "side_model_experiment_id": side_summary.get("experiment_id"),
        "prior_ocr_experiment_id": prior_ocr_summary.get("experiment_id"),
        "prior_published_quality_below_count": int(
            prior_lookup[("quality_below_0_25", "quality_aware")]["count"]
        ),
        "prior_matched_low_quality_environment_count": len(prior_low),
        "prior_published_quality_below_included_normal_rows": int(
            prior_lookup[("quality_below_0_25", "quality_aware")]["count"]
        )
        - len(prior_low),
        "selector_model_retrained": False,
        "selector_threshold_tuned": False,
        "whole_crop_fallback_added": False,
        "template_test_overlap": template_test_overlap,
        "gate": gate,
        "error_count": len(errors),
        "errors": errors,
        "ocr_test_split_opened": False,
        "formal_ev_test_opened": False,
        "selector_ocr_non_regression_evaluated": True,
        "full_frame_selector_ocr_evaluated": False,
        "desktop_plate_crop_end_to_end_latency_evaluated": True,
        "pi_end_to_end_latency_evaluated": False,
        "real_ir_evaluated": False,
        "independent_holdout_evaluated": False,
        "operational_candidate": False,
        "performance_claim": False,
    }
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _write_csv(output_dir / "manifest.csv", manifest_rows)
    _write_csv(output_dir / "details.csv", detail_rows)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    _write_csv(output_dir / "candidate_details.csv", candidate_rows)
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_dir / "REPORT.md").write_text(_report(summary, lookup), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ocr-manifest", required=True)
    parser.add_argument("--prior-ocr-summary", required=True)
    parser.add_argument("--prior-ocr-environment-summary", required=True)
    parser.add_argument("--prior-ocr-details", required=True)
    parser.add_argument("--selector-summary", required=True)
    parser.add_argument("--side-model-summary", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--thresholds", required=True)
    parser.add_argument("--tesseract", required=True)
    parser.add_argument("--tessdata", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--sample-size", type=int, default=50)
    parser.add_argument("--ocr-timeout-seconds", type=float, default=30.0)
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    return 0 if summary["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Retrain low-quality side evidence with grouped OCR hard negatives."""

from __future__ import annotations

import argparse
import ast
import json
import time
from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .icon_detector_common import detect_icon_contrastive
from .low_quality_presence_rectification import (
    LOGISTIC_GLOBAL_FEATURES,
    LOGISTIC_SIDE_FEATURES,
    LOW_QUALITY_ENVIRONMENTS,
    QUALITY_THRESHOLD,
    TARGET_SIZE,
    _build_template_cache,
    _corrected_icon_records,
    _fit_weighted_logistic,
    _group_fold,
    _icon_patch_features,
    _predict_logistic,
    _templates_for,
    apply_environment,
)
from .low_quality_vehicle_presence_calibration import (
    COHORTS,
    _manifest_rows,
    _read_unicode,
    _write_csv,
    apply_policy,
    choose_vehicle_policy,
    summarize_rows,
)
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .roi_annotation import read_csv


EXPERIMENT_ID = "low-quality-hard-negative-side-model-011"


def parse_candidate_bbox(value: str) -> list[int]:
    parsed = ast.literal_eval(value)
    if not isinstance(parsed, (list, tuple)) or len(parsed) != 4:
        raise ValueError("candidate_bbox_invalid")
    result = [int(item) for item in parsed]
    if result[2] <= result[0] or result[3] <= result[1]:
        raise ValueError("candidate_bbox_empty")
    return result


def _extract_side_features(
    image: np.ndarray,
    side: str,
    positive_templates: list,
    negative_templates: list,
    base: dict,
) -> dict[str, float]:
    detection = detect_icon_contrastive(image, side, positive_templates, negative_templates)
    result = {
        f"{side}_feature_template_edge_margin": float(
            detection.get("positive_edge_similarity", 0.0)
            - detection.get("negative_edge_similarity", 0.0)
        ),
        f"{side}_feature_template_robust_margin": float(
            detection.get("positive_robust_similarity", 0.0)
            - detection.get("negative_robust_similarity", 0.0)
        ),
    }
    for name in (
        "positive_edge_similarity",
        "negative_edge_similarity",
        "positive_robust_similarity",
        "negative_robust_similarity",
    ):
        result[f"{side}_feature_{name}"] = float(detection.get(name, 0.0))
    for name, value in _icon_patch_features(image, detection.get("bbox", [])).items():
        result[f"{side}_feature_{name}"] = value
    result.update({name: float(base[name]) for name in LOGISTIC_GLOBAL_FEATURES})
    return result


def build_template_context(split_manifest: Path, icon_manifest: Path) -> tuple[dict, dict, list[str], int]:
    split_source = read_csv(split_manifest)
    split_rows = {row["annotation_id"]: row for row in split_source}
    icon_rows = read_csv(icon_manifest)
    template_rows, labels = _corrected_icon_records(icon_rows, split_rows, include_test_labels=False)
    test_ids = {key for key, value in split_rows.items() if value.get("split") == "test"}
    overlap = sorted({row["annotation_id"] for row in template_rows} & test_ids)
    return _build_template_cache(template_rows), labels, overlap, len(template_rows)


def build_pilot_rows(
    selector_details: Path,
    split_manifest: Path,
    icon_manifest: Path,
    extra_feature_extractor: Callable[[np.ndarray, str], dict[str, float]] | None = None,
) -> tuple[list[dict], list[dict], dict]:
    template_cache, labels, template_test_overlap, template_count = build_template_context(
        split_manifest, icon_manifest
    )
    source_rows = read_csv(selector_details)
    selected = [row for row in source_rows if row.get("oof_selected") == "1"]
    if any(row.get("split") == "test" for row in selected):
        raise ValueError("formal_test_row_in_selector_side_training")
    rows: list[dict] = []
    errors: list[dict] = []
    image_cache: dict[str, np.ndarray | None] = {}
    for source in selected:
        started = time.perf_counter()
        annotation_id = source["annotation_id"]
        source_path = source["source_path"]
        try:
            if source_path not in image_cache:
                image_cache[source_path] = _read_unicode(Path(source_path))
            image = image_cache[source_path]
            if image is None:
                raise ValueError("unreadable_pilot_input")
            degraded = apply_environment(image, source["environment"])
            x1, y1, x2, y2 = parse_candidate_bbox(source["candidate_bbox"])
            crop = degraded[y1:y2, x1:x2]
            if crop.size == 0:
                raise ValueError("selected_candidate_crop_empty")
            rectified = rectify_plate(crop, TARGET_SIZE)
            expected_method = source["rectification_method"]
            if rectified.method != expected_method:
                raise ValueError(f"selected_rectification_method_drift:{expected_method}:{rectified.method}")
            output_quality = rectification_quality_score(rectified.plate_image)
            base = {
                "rectification_geometry_confidence": float(rectified.rect_quality_input),
                "rectification_quality": float(output_quality),
                "quality_aware_fallback_used": 1.0,
            }
            item = {
                "source_id": annotation_id,
                "source_group": f"pilot:{annotation_id}",
                "source_set": "pilot_development",
                "source_path": source_path,
                "original_split": source["split"],
                "ground_truth": source["ground_truth"],
                "vehicle_presence_label": source["vehicle_presence_label"],
                "label_provenance": "corrected_vehicle_human_side_icon_and_oof_selector",
                "environment": source["environment"],
                "quality_score": float(source["input_quality"]),
                "quality_below_threshold": int(source["quality_below_threshold"]),
                "quality_score_source": "ground_truth_plate_crop",
                "geometric_success": int(source["valid_geometric_candidate"]),
                "alignment_ssim": float(source["alignment_ssim"]),
                "rectification_method": rectified.method,
                "rectification_quality": float(output_quality),
                "rectification_geometry_confidence": float(rectified.rect_quality_input),
                "selector_candidate_rank": int(source["candidate_rank"]),
                "selector_oof_probability": float(source["oof_selector_probability"]),
                "prior_left_icon_probability": float(source["left_icon_probability"]),
                "prior_right_icon_probability": float(source["right_icon_probability"]),
                "error_reason": "",
            }
            for side in ("left", "right"):
                label = labels.get((annotation_id, side), "excluded")
                positives, negatives = _templates_for(
                    template_cache,
                    source["split"],
                    annotation_id,
                    side,
                    label,
                )
                item[f"{side}_icon_label"] = label
                item.update(_extract_side_features(rectified.plate_image, side, positives, negatives, base))
                if extra_feature_extractor is not None:
                    item.update(extra_feature_extractor(rectified.plate_image, side))
            item["processing_ms"] = (time.perf_counter() - started) * 1000.0
            rows.append(item)
        except (OSError, SyntaxError, ValueError, cv2.error) as exc:
            errors.append(
                {
                    "source_id": annotation_id,
                    "environment": source.get("environment", ""),
                    "reason": f"{type(exc).__name__}:{str(exc).splitlines()[0][:180]}",
                }
            )
    context = {
        "template_cache": template_cache,
        "template_test_overlap": template_test_overlap,
        "template_count": template_count,
    }
    return rows, errors, context


def build_hard_negative_rows(
    manifest_path: Path,
    template_cache: dict,
    extra_feature_extractor: Callable[[np.ndarray, str], dict[str, float]] | None = None,
) -> tuple[list[dict], list[dict]]:
    manifest = read_csv(manifest_path)
    if not manifest:
        raise ValueError("hard_negative_manifest_empty")
    rows: list[dict] = []
    errors: list[dict] = []
    for source in manifest:
        if source.get("split") == "test":
            raise ValueError("formal_test_row_in_hard_negative_manifest")
        source_path = Path(source["input_path"])
        image = _read_unicode(source_path)
        source_id = str(source.get("vehicle_id") or source.get("source_id"))
        if image is None:
            errors.append({"source_id": source_id, "reason": "unreadable_hard_negative_input"})
            continue
        for environment in sorted(LOW_QUALITY_ENVIRONMENTS):
            started = time.perf_counter()
            try:
                degraded = apply_environment(image, environment)
                normalized = cv2.resize(degraded, TARGET_SIZE, interpolation=cv2.INTER_AREA)
                input_quality = rectification_quality_score(normalized)
                rectified = rectify_plate(degraded, TARGET_SIZE)
                output_quality = rectification_quality_score(rectified.plate_image)
                geometric_success = int(rectified.method in GEOMETRIC_METHODS)
                base = {
                    "rectification_geometry_confidence": float(rectified.rect_quality_input),
                    "rectification_quality": float(output_quality),
                    "quality_aware_fallback_used": 1.0,
                }
                item = {
                    "source_id": source_id,
                    "source_group": f"ocr-hard-negative:{source_id}",
                    "source_set": "ocr_hard_negative",
                    "source_path": str(source_path),
                    "original_split": "development_external",
                    "ground_truth": "NON_EV",
                    "vehicle_presence_label": "absent",
                    "label_provenance": "non_ev_ocr_dataset_class_inferred_icon_absent",
                    "environment": environment,
                    "quality_score": float(input_quality),
                    "quality_below_threshold": int(input_quality < QUALITY_THRESHOLD),
                    "quality_score_source": "normalized_full_plate_input",
                    "geometric_success": geometric_success,
                    "alignment_ssim": "",
                    "rectification_method": rectified.method,
                    "rectification_quality": float(output_quality),
                    "rectification_geometry_confidence": float(rectified.rect_quality_input),
                    "left_icon_label": "absent",
                    "right_icon_label": "absent",
                    "error_reason": "",
                }
                for side in ("left", "right"):
                    item.update(
                        _extract_side_features(
                            rectified.plate_image,
                            side,
                            template_cache[(side, "positive", "")],
                            template_cache[(side, "negative", "")],
                            base,
                        )
                    )
                    if extra_feature_extractor is not None:
                        item.update(extra_feature_extractor(rectified.plate_image, side))
                item["processing_ms"] = (time.perf_counter() - started) * 1000.0
                rows.append(item)
            except (OSError, ValueError, cv2.error) as exc:
                errors.append(
                    {
                        "source_id": source_id,
                        "environment": environment,
                        "reason": f"{type(exc).__name__}:{str(exc).splitlines()[0][:180]}",
                    }
                )
    return rows, errors


def score_side(model: dict, row: dict, side: str) -> tuple[float, float]:
    probability = _predict_logistic(model, row, side)
    effective = probability if int(row["geometric_success"]) else -1.0
    return probability, effective


def _serialize_model(model: dict) -> dict:
    return {
        "feature_names": [*LOGISTIC_SIDE_FEATURES, *LOGISTIC_GLOBAL_FEATURES],
        "mean": model["mean"].tolist(),
        "scale": model["scale"].tolist(),
        "coefficients": model["coefficients"].tolist(),
        "positive_count": model["positive_count"],
        "negative_count": model["negative_count"],
        "learning_rate": model["learning_rate"],
        "regularization": model["regularization"],
        "iterations": model["iterations"],
    }


def _fit_side_models(rows: list[dict]) -> dict[str, dict]:
    models: dict[str, dict] = {}
    for side in ("left", "right"):
        eligible = [row for row in rows if row[f"{side}_icon_label"] in {"present", "absent"}]
        models[side] = _fit_weighted_logistic(eligible, side)
    return models


def _set_scores(rows: list[dict], models: dict[str, dict], prefix: str, set_effective: bool) -> None:
    for row in rows:
        for side in ("left", "right"):
            probability, effective = score_side(models[side], row, side)
            row[f"{prefix}_{side}_icon_probability"] = probability
            row[f"{prefix}_{side}_icon_score"] = effective
            if set_effective:
                row[f"{side}_icon_logistic_score"] = probability
                row[f"{side}_icon_score"] = effective


def apply_nested_group_oof(
    rows: list[dict], fold_count: int, fpr_cap: float
) -> tuple[list[dict], dict[str, dict], dict[str, dict]]:
    for row in rows:
        row["side_model_fold"] = _group_fold(row["source_group"], fold_count)
    audit: list[dict] = []
    for fold in range(fold_count):
        fit_rows = [row for row in rows if int(row["side_model_fold"]) != fold]
        holdout_rows = [row for row in rows if int(row["side_model_fold"]) == fold]
        fit_groups = {row["source_group"] for row in fit_rows}
        holdout_groups = {row["source_group"] for row in holdout_rows}
        overlap = sorted(fit_groups & holdout_groups)
        models = _fit_side_models(fit_rows)
        _set_scores(fit_rows, models, "fold_fit", set_effective=True)
        policies = {
            cohort: choose_vehicle_policy(fit_rows, cohort, fpr_cap)
            for cohort in ("quality_below_0_25", "quality_at_or_above_0_25")
        }
        _set_scores(holdout_rows, models, "oof", set_effective=True)
        for row in holdout_rows:
            cohort = (
                "quality_below_0_25"
                if int(row["quality_below_threshold"])
                else "quality_at_or_above_0_25"
            )
            apply_policy(row, policies[cohort], "oof")
            row["policy_fold"] = fold
        audit.append(
            {
                "fold": fold,
                "fit_group_count": len(fit_groups),
                "holdout_group_count": len(holdout_groups),
                "group_overlap": len(overlap),
                "models": {side: _serialize_model(model) for side, model in models.items()},
                "policies": policies,
            }
        )
    final_models = _fit_side_models(rows)
    _set_scores(rows, final_models, "frozen", set_effective=True)
    final_policies = {
        cohort: choose_vehicle_policy(rows, cohort, fpr_cap)
        for cohort in ("quality_below_0_25", "quality_at_or_above_0_25")
    }
    for row in rows:
        cohort = "quality_below_0_25" if int(row["quality_below_threshold"]) else "quality_at_or_above_0_25"
        apply_policy(row, final_policies[cohort], "frozen")
        for side in ("left", "right"):
            row[f"{side}_icon_logistic_score"] = row[f"oof_{side}_icon_probability"]
            row[f"{side}_icon_score"] = row[f"oof_{side}_icon_score"]
    return audit, {side: _serialize_model(model) for side, model in final_models.items()}, final_policies


def _positive_geometric(rows: list[dict], below_only: bool = False) -> float:
    selected = [
        row
        for row in rows
        if row["vehicle_presence_label"] == "present"
        and (not below_only or int(row["quality_below_threshold"]))
    ]
    return sum(int(row["geometric_success"]) for row in selected) / len(selected) if selected else 0.0


def _report(summary: dict, lookup: dict[tuple[str, str], dict]) -> str:
    combined = lookup[("all", "synthetic_low_quality")]
    low = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    gates = "\n".join(
        f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()
    )
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Formal Test accessed in this run: `false`
- Sources/rows/errors: {summary['source_count']} / {summary['detail_rows']} / {summary['error_count']}

## Nested source-group OOF

| Cohort | Recall | FPR | Prescreen Recall | REVIEW | Geometric success |
|---|---:|---:|---:|---:|---:|
| Combined | {combined['oof_vehicle_recall']:.2%} | {combined['oof_vehicle_fpr']:.2%} | {combined['oof_prescreen_recall']:.2%} | {combined['oof_review_rate']:.2%} | {combined['geometric_rectification_success']:.2%} |
| Quality < 0.25 | {low['oof_vehicle_recall']:.2%} | {low['oof_vehicle_fpr']:.2%} | {low['oof_prescreen_recall']:.2%} | {low['oof_review_rate']:.2%} | {low['geometric_rectification_success']:.2%} |
| Corrected pilot | {pilot['oof_vehicle_recall']:.2%} | {pilot['oof_vehicle_fpr']:.2%} | {pilot['oof_prescreen_recall']:.2%} | {pilot['oof_review_rate']:.2%} | {pilot['geometric_rectification_success']:.2%} |
| OCR hard negative | N/A | {hard['oof_vehicle_fpr']:.2%} | N/A | {hard['oof_review_rate']:.2%} | {hard['geometric_rectification_success']:.2%} |

## Gate

{gates}

OCR hard-negative의 side absent는 dataset class 기반 개발 추정 라벨이다. 통과하더라도 사람 icon GT, selector 적용 후 OCR 비회귀, 전체 latency, 실제 RGB/IR 독립 holdout과 Pi runtime 검증이 필요하다.
"""


def run(args: argparse.Namespace) -> dict:
    selector_summary = json.loads(Path(args.selector_summary).resolve().read_text(encoding="utf-8-sig"))
    ocr_guard = json.loads(Path(args.ocr_guard_summary).resolve().read_text(encoding="utf-8-sig"))
    if selector_summary.get("formal_test_accessed_in_this_run"):
        raise ValueError("formal_test_accessed_in_selector_summary")
    pilot_rows, pilot_errors, context = build_pilot_rows(
        Path(args.selector_details).resolve(),
        Path(args.split_manifest).resolve(),
        Path(args.icon_manifest).resolve(),
    )
    hard_rows, hard_errors = build_hard_negative_rows(
        Path(args.ocr_hard_negative_manifest).resolve(),
        context["template_cache"],
    )
    rows = [*pilot_rows, *hard_rows]
    errors = [*pilot_errors, *hard_errors]
    pilot_paths = {str(Path(row["source_path"]).resolve()).casefold() for row in pilot_rows}
    hard_paths = {str(Path(row["source_path"]).resolve()).casefold() for row in hard_rows}
    source_path_overlap = sorted(pilot_paths & hard_paths)
    audit, final_models, final_policies = apply_nested_group_oof(
        rows, args.fold_count, args.fit_fpr_cap
    )
    metrics = [
        summarize_rows(rows, source_set, cohort)
        for source_set in ("all", "pilot_development", "ocr_hard_negative")
        for cohort in COHORTS
    ]
    lookup = {(row["source_set"], row["cohort"]): row for row in metrics}
    combined = lookup[("all", "synthetic_low_quality")]
    low = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    positive_geometric = _positive_geometric(pilot_rows)
    low_positive_geometric = _positive_geometric(pilot_rows, below_only=True)
    gate = {
        "combined_oof_vehicle_recall_at_least_0_40": combined["oof_vehicle_recall"] >= 0.40,
        "combined_oof_vehicle_fpr_le_0_10": combined["oof_vehicle_fpr"] <= args.gate_fpr,
        "quality_below_0_25_oof_recall_at_least_step96": low["oof_vehicle_recall"] >= 0.42857142857142855,
        "quality_below_0_25_oof_fpr_le_0_10": low["oof_vehicle_fpr"] <= args.gate_fpr,
        "quality_below_0_25_prescreen_recall_one": low["oof_prescreen_recall"] == 1.0,
        "pilot_negative_fpr_le_0_15": pilot["oof_vehicle_fpr"] <= 0.15,
        "ocr_hard_negative_fpr_le_0_10": hard["oof_vehicle_fpr"] <= args.gate_fpr,
        "pilot_geometric_success_at_least_step98": pilot["geometric_rectification_success"] >= 0.7588235294117647,
        "presence_positive_geometric_success_at_least_step98": positive_geometric >= 0.7386363636363636,
        "quality_below_positive_geometric_success_at_least_step98": low_positive_geometric >= 0.7142857142857143,
        "prior_selector_all_gates_passed": selector_summary.get("status") == "selector_development_candidate" and all(selector_summary.get("gate", {}).values()),
        "prior_ocr_rectifier_guard_passed": ocr_guard.get("status") == "ocr_guard_pass",
        "source_path_overlap_zero": not source_path_overlap,
        "group_overlap_zero": all(not item["group_overlap"] for item in audit),
        "formal_test_rows_zero": not any(row["original_split"] == "test" for row in rows),
        "template_test_overlap_zero": not context["template_test_overlap"],
        "processing_errors_zero": not errors,
    }
    status = "hard_negative_side_model_development_candidate" if all(gate.values()) else "rejected"
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "source_count": len({row["source_group"] for row in rows}),
        "pilot_source_count": len({row["source_group"] for row in pilot_rows}),
        "hard_negative_source_count": len({row["source_group"] for row in hard_rows}),
        "detail_rows": len(rows),
        "fit_fpr_cap": args.fit_fpr_cap,
        "gate_fpr": args.gate_fpr,
        "template_count": context["template_count"],
        "fold_audit": audit,
        "frozen_side_models": final_models,
        "frozen_vehicle_policies": final_policies,
        "positive_geometric_success": positive_geometric,
        "quality_below_positive_geometric_success": low_positive_geometric,
        "source_path_overlap": source_path_overlap,
        "template_test_overlap": context["template_test_overlap"],
        "gate": gate,
        "error_count": len(errors),
        "errors": errors,
        "formal_test_accessed_in_this_run": False,
        "formal_test_previously_opened": bool(args.formal_test_previously_opened),
        "formal_test_reuse_for_tuning_prohibited": True,
        "hard_negative_label_provenance": "non_ev_ocr_dataset_class_inferred_icon_absent",
        "hard_negative_human_icon_gt": False,
        "selector_ocr_non_regression_evaluated": False,
        "end_to_end_selector_latency_evaluated": False,
        "operational_candidate": False,
        "real_ir_evaluated": False,
        "independent_holdout_evaluated": False,
        "performance_claim": False,
    }
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _write_csv(output_dir / "manifest.csv", _manifest_rows(rows))
    _write_csv(output_dir / "details.csv", rows)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "REPORT.md").write_text(_report(summary, lookup), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selector-details", required=True)
    parser.add_argument("--selector-summary", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--ocr-hard-negative-manifest", required=True)
    parser.add_argument("--ocr-guard-summary", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--fold-count", type=int, default=5)
    parser.add_argument("--fit-fpr-cap", type=float, default=0.08)
    parser.add_argument("--gate-fpr", type=float, default=0.10)
    parser.add_argument("--formal-test-previously-opened", action="store_true")
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    return 0 if summary["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

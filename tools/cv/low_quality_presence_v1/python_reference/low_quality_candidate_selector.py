"""Development-only group-OOF selector for low-quality top-3 plate candidates."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .icon_detector_common import detect_icon_contrastive
from .low_quality_presence_rectification import (
    IOU_THRESHOLD,
    LOGISTIC_GLOBAL_FEATURES,
    LOW_QUALITY_ENVIRONMENTS,
    QUALITY_THRESHOLD,
    TARGET_SIZE,
    _build_template_cache,
    _corrected_icon_records,
    _group_fold,
    _icon_patch_features,
    _predict_logistic,
    _templates_for,
    _warp_ground_truth,
    alignment_ssim,
    apply_environment,
)
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .roi_annotation import parse_box, read_csv
from .top3_localization import intersection_over_union, ranked_candidates, read_image


EXPERIMENT_ID = "low-quality-candidate-selector-007"
SELECTOR_FEATURES = (
    "generator_score",
    "rank_inverse",
    "bbox_area_ratio",
    "bbox_aspect",
    "origin_geometry",
    "origin_character_edge",
    "origin_character_group",
    "method_perspective",
    "method_rotated_rect",
    "geometry_confidence",
    "rectification_quality",
    "input_quality",
    "icon_probability_max",
    "icon_probability_min",
    "icon_probability_mean",
    "icon_probability_gap",
)


def _deserialize_side_model(summary: dict, side: str) -> dict:
    source = summary["logistic_models"][side]["final_model"]
    return {
        "mean": np.asarray(source["mean"], dtype=np.float64),
        "scale": np.asarray(source["scale"], dtype=np.float64),
        "coefficients": np.asarray(source["coefficients"], dtype=np.float64),
    }


def _vehicle_presence_label(labels: dict[tuple[str, str], str], annotation_id: str) -> str:
    left = labels.get((annotation_id, "left"), "excluded")
    right = labels.get((annotation_id, "right"), "excluded")
    if "present" in {left, right}:
        return "present"
    if left == "absent" and right == "absent":
        return "absent"
    return "excluded"


def _candidate_icon_probability(
    image: np.ndarray,
    side: str,
    template_cache: dict,
    split: str,
    annotation_id: str,
    label: str,
    side_model: dict,
    base: dict,
) -> float:
    positives, negatives = _templates_for(template_cache, split, annotation_id, side, label)
    detection = detect_icon_contrastive(image, side, positives, negatives)
    feature_row = {
        f"{side}_feature_template_edge_margin": float(
            detection.get("positive_edge_similarity", 0.0) - detection.get("negative_edge_similarity", 0.0)
        ),
        f"{side}_feature_template_robust_margin": float(
            detection.get("positive_robust_similarity", 0.0) - detection.get("negative_robust_similarity", 0.0)
        ),
    }
    for name in (
        "positive_edge_similarity",
        "negative_edge_similarity",
        "positive_robust_similarity",
        "negative_robust_similarity",
    ):
        feature_row[f"{side}_feature_{name}"] = float(detection.get(name, 0.0))
    for name, value in _icon_patch_features(image, detection.get("bbox", [])).items():
        feature_row[f"{side}_feature_{name}"] = value
    feature_row.update({name: base[name] for name in LOGISTIC_GLOBAL_FEATURES})
    return _predict_logistic(side_model, feature_row, side)


def build_candidate_rows(
    annotation_manifest: Path,
    split_manifest: Path,
    icon_manifest: Path,
    thresholds_path: Path,
    side_model_summary: dict,
) -> tuple[list[dict], list[dict], list[dict], list[dict]]:
    split_source = read_csv(split_manifest)
    split_rows = {row["annotation_id"]: row for row in split_source}
    allowed = {key: value for key, value in split_rows.items() if value.get("split") in {"train", "validation"}}
    if any(value.get("split") == "test" for value in allowed.values()):
        raise ValueError("formal_test_in_candidate_selector_allowed_rows")
    icon_rows = read_csv(icon_manifest)
    template_rows, icon_labels = _corrected_icon_records(icon_rows, split_rows, include_test_labels=False)
    test_ids = {key for key, value in split_rows.items() if value.get("split") == "test"}
    template_test_overlap = sorted({row["annotation_id"] for row in template_rows} & test_ids)
    if template_test_overlap:
        raise ValueError(f"formal_test_template_overlap:{template_test_overlap[:3]}")
    template_cache = _build_template_cache(template_rows)
    side_models = {side: _deserialize_side_model(side_model_summary, side) for side in ("left", "right")}
    thresholds = json.loads(thresholds_path.read_text(encoding="utf-8-sig"))
    rows: list[dict] = []
    source_manifest: list[dict] = []
    source_environment_rows: list[dict] = []
    errors: list[dict] = []
    for source in read_csv(annotation_manifest):
        split_row = allowed.get(source["annotation_id"])
        if split_row is None:
            continue
        annotation_id = source["annotation_id"]
        source_manifest.append(
            {
                "annotation_id": annotation_id,
                "source_path": source["source_path"],
                "split": split_row["split"],
                "ground_truth": split_row["ground_truth"],
                "vehicle_presence_label": _vehicle_presence_label(icon_labels, annotation_id),
                "formal_test_member": 0,
            }
        )
        image = read_image(Path(source["source_path"]))
        gt_bbox = parse_box(source.get("plate_bbox", ""))
        gt_quad = parse_box(source.get("plate_quad", ""))
        if len(gt_bbox) != 4 and len(gt_quad) == 8:
            xs, ys = gt_quad[0::2], gt_quad[1::2]
            gt_bbox = [min(xs), min(ys), max(xs), max(ys)]
        if len(gt_quad) != 8 and len(gt_bbox) == 4:
            x1, y1, x2, y2 = gt_bbox
            gt_quad = [x1, y1, x2, y1, x2, y2, x1, y2]
        if image is None or len(gt_bbox) != 4 or len(gt_quad) != 8:
            errors.append({"annotation_id": annotation_id, "reason": "input_or_ground_truth_missing"})
            continue
        for environment in sorted(LOW_QUALITY_ENVIRONMENTS):
            started = time.perf_counter()
            try:
                degraded = apply_environment(image, environment)
                x1, y1, x2, y2 = gt_bbox
                gt_crop = degraded[y1:y2, x1:x2]
                input_quality = rectification_quality_score(gt_crop)
                gt_rectified = _warp_ground_truth(degraded, gt_quad)
                candidates = ranked_candidates(
                    degraded,
                    thresholds,
                    origins=("geometry", "character_edge"),
                    limit=3,
                    adaptive=True,
                    component_group=True,
                    reserve_coarse=True,
                )
                source_key = f"{annotation_id}|{environment}"
                source_environment_rows.append(
                    {
                        "source_key": source_key,
                        "annotation_id": annotation_id,
                        "source_group": annotation_id,
                        "source_path": source["source_path"],
                        "split": split_row["split"],
                        "ground_truth": split_row["ground_truth"],
                        "vehicle_presence_label": _vehicle_presence_label(icon_labels, annotation_id),
                        "environment": environment,
                        "input_quality": input_quality,
                        "quality_below_threshold": int(input_quality < QUALITY_THRESHOLD),
                        "candidate_count": len(candidates),
                        "top3_hit": int(any(intersection_over_union(item["bbox"], gt_bbox) >= IOU_THRESHOLD for item in candidates)),
                        "processing_ms_before_selector": (time.perf_counter() - started) * 1000.0,
                    }
                )
                height, width = degraded.shape[:2]
                for rank, candidate in enumerate(candidates, 1):
                    bx1, by1, bx2, by2 = candidate["bbox"]
                    crop = degraded[by1:by2, bx1:bx2]
                    if crop.size == 0:
                        continue
                    rectified = rectify_plate(crop, TARGET_SIZE)
                    output_quality = rectification_quality_score(rectified.plate_image)
                    candidate_iou = intersection_over_union(candidate["bbox"], gt_bbox)
                    geometric_method = int(rectified.method in GEOMETRIC_METHODS)
                    base = {
                        "rectification_geometry_confidence": float(rectified.rect_quality_input),
                        "rectification_quality": float(output_quality),
                        "quality_aware_fallback_used": 1.0,
                    }
                    icon_probabilities = []
                    for side in ("left", "right"):
                        icon_probabilities.append(
                            _candidate_icon_probability(
                                rectified.plate_image,
                                side,
                                template_cache,
                                split_row["split"],
                                annotation_id,
                                icon_labels.get((annotation_id, side), "excluded"),
                                side_models[side],
                                base,
                            )
                        )
                    bbox_width = max(1, bx2 - bx1)
                    bbox_height = max(1, by2 - by1)
                    candidate_features = {
                        "generator_score": float(candidate["score"]),
                        "rank_inverse": 1.0 / rank,
                        "bbox_area_ratio": bbox_width * bbox_height / float(max(1, width * height)),
                        "bbox_aspect": bbox_width / float(bbox_height),
                        "origin_geometry": int(candidate["origin"] == "geometry"),
                        "origin_character_edge": int(candidate["origin"] == "character_edge"),
                        "origin_character_group": int(candidate["origin"] == "character_group"),
                        "method_perspective": int(rectified.method == "perspective"),
                        "method_rotated_rect": int(rectified.method == "rotated_rect"),
                        "geometry_confidence": float(rectified.rect_quality_input),
                        "rectification_quality": float(output_quality),
                        "input_quality": float(input_quality),
                        "icon_probability_max": max(icon_probabilities),
                        "icon_probability_min": min(icon_probabilities),
                        "icon_probability_mean": statistics.mean(icon_probabilities),
                        "icon_probability_gap": abs(icon_probabilities[0] - icon_probabilities[1]),
                    }
                    rows.append(
                        {
                            "source_key": source_key,
                            "annotation_id": annotation_id,
                            "source_group": annotation_id,
                            "source_path": source["source_path"],
                            "split": split_row["split"],
                            "ground_truth": split_row["ground_truth"],
                            "vehicle_presence_label": _vehicle_presence_label(icon_labels, annotation_id),
                            "environment": environment,
                            "quality_below_threshold": int(input_quality < QUALITY_THRESHOLD),
                            "candidate_rank": rank,
                            "candidate_origin": candidate["origin"],
                            "candidate_bbox": candidate["bbox"],
                            "candidate_iou": candidate_iou,
                            "rectification_method": rectified.method,
                            "valid_geometric_candidate": int(geometric_method and candidate_iou >= IOU_THRESHOLD),
                            "alignment_ssim": (
                                alignment_ssim(rectified.plate_image, gt_rectified)
                                if candidate_iou >= IOU_THRESHOLD
                                else 0.0
                            ),
                            "left_icon_probability": icon_probabilities[0],
                            "right_icon_probability": icon_probabilities[1],
                            **candidate_features,
                            "error_reason": "",
                        }
                    )
            except (cv2.error, ValueError) as exc:
                errors.append(
                    {
                        "annotation_id": annotation_id,
                        "environment": environment,
                        "reason": f"{type(exc).__name__}:{str(exc).splitlines()[0][:160]}",
                    }
                )
    return rows, source_manifest, source_environment_rows, errors


def _feature_vector(row: dict) -> np.ndarray:
    return np.asarray([float(row[name]) for name in SELECTOR_FEATURES], dtype=np.float64)


def _sigmoid(values: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(values, -30.0, 30.0)))


def fit_selector(rows: list[dict]) -> dict:
    x = np.vstack([_feature_vector(row) for row in rows])
    y = np.asarray([float(row["valid_geometric_candidate"]) for row in rows])
    positive_count = int(np.sum(y == 1.0))
    negative_count = int(np.sum(y == 0.0))
    if not positive_count or not negative_count:
        raise ValueError(f"selector_class_missing:{positive_count}:{negative_count}")
    mean = np.mean(x, axis=0)
    scale = np.std(x, axis=0)
    scale[scale < 1e-6] = 1.0
    design = np.column_stack([(x - mean) / scale, np.ones(len(x), dtype=np.float64)])
    weights = np.where(y == 1.0, len(y) / (2.0 * positive_count), len(y) / (2.0 * negative_count))
    coefficients = np.zeros(design.shape[1], dtype=np.float64)
    learning_rate, regularization, iterations = 0.06, 0.03, 900
    for _ in range(iterations):
        probability = _sigmoid(design @ coefficients)
        gradient = design.T @ (weights * (probability - y)) / float(np.sum(weights))
        gradient[:-1] += regularization * coefficients[:-1]
        coefficients -= learning_rate * gradient
    return {
        "mean": mean,
        "scale": scale,
        "coefficients": coefficients,
        "positive_count": positive_count,
        "negative_count": negative_count,
        "learning_rate": learning_rate,
        "regularization": regularization,
        "iterations": iterations,
    }


def predict_selector(model: dict, row: dict) -> float:
    design = np.append((_feature_vector(row) - model["mean"]) / model["scale"], 1.0)
    return float(_sigmoid(np.asarray([design @ model["coefficients"]]))[0])


def _serialize_model(model: dict) -> dict:
    return {
        "feature_names": list(SELECTOR_FEATURES),
        "mean": model["mean"].tolist(),
        "scale": model["scale"].tolist(),
        "coefficients": model["coefficients"].tolist(),
        "positive_count": model["positive_count"],
        "negative_count": model["negative_count"],
        "learning_rate": model["learning_rate"],
        "regularization": model["regularization"],
        "iterations": model["iterations"],
    }


def apply_group_oof_selector(rows: list[dict], fold_count: int) -> tuple[list[dict], dict]:
    for row in rows:
        row["selector_fold"] = _group_fold(str(row["source_group"]), fold_count)
        row["oof_selected"] = 0
    audit: list[dict] = []
    for fold in range(fold_count):
        fit_rows = [row for row in rows if int(row["selector_fold"]) != fold]
        holdout_rows = [row for row in rows if int(row["selector_fold"]) == fold]
        fit_groups = {row["source_group"] for row in fit_rows}
        holdout_groups = {row["source_group"] for row in holdout_rows}
        overlap = fit_groups & holdout_groups
        if overlap:
            raise ValueError(f"selector_group_leak:{fold}:{sorted(overlap)[:3]}")
        model = fit_selector(fit_rows)
        by_source: dict[str, list[dict]] = {}
        for row in holdout_rows:
            row["oof_selector_probability"] = predict_selector(model, row)
            by_source.setdefault(row["source_key"], []).append(row)
        for candidates in by_source.values():
            selected = max(candidates, key=lambda item: (item["oof_selector_probability"], -int(item["candidate_rank"])))
            selected["oof_selected"] = 1
        audit.append(
            {
                "fold": fold,
                "fit_group_count": len(fit_groups),
                "holdout_group_count": len(holdout_groups),
                "group_overlap": 0,
                "model": _serialize_model(model),
            }
        )
    final_model = fit_selector(rows)
    by_source: dict[str, list[dict]] = {}
    for row in rows:
        row["frozen_selector_probability"] = predict_selector(final_model, row)
        row["frozen_selected"] = 0
        by_source.setdefault(row["source_key"], []).append(row)
    for candidates in by_source.values():
        selected = max(candidates, key=lambda item: (item["frozen_selector_probability"], -int(item["candidate_rank"])))
        selected["frozen_selected"] = 1
    return audit, _serialize_model(final_model)


def summarize_selection(
    candidate_rows: list[dict],
    source_rows: list[dict],
    baseline_lookup: dict[tuple[str, str], dict],
    cohort: str,
    label_scope: str,
) -> dict:
    sources = [
        row
        for row in source_rows
        if (cohort == "synthetic_low_quality" or int(row["quality_below_threshold"]) == 1)
        and (label_scope == "all" or row["vehicle_presence_label"] == label_scope)
    ]
    keys = {row["source_key"] for row in sources}
    selected = [row for row in candidate_rows if row["source_key"] in keys and int(row["oof_selected"])]
    baseline = [baseline_lookup[(row["annotation_id"], row["environment"])] for row in sources]
    selected_alignment = [float(row["alignment_ssim"]) for row in selected if int(row["valid_geometric_candidate"])]
    baseline_alignment = [float(row["alignment_ssim"]) for row in baseline if int(row["geometric_success"])]
    return {
        "cohort": cohort,
        "label_scope": label_scope,
        "row_count": len(sources),
        "unique_source_count": len({row["annotation_id"] for row in sources}),
        "top3_recall": sum(int(row["top3_hit"]) for row in sources) / len(sources) if sources else 0.0,
        "baseline_geometric_success": sum(int(row["geometric_success"]) for row in baseline) / len(baseline) if baseline else 0.0,
        "oof_geometric_success": sum(int(row["valid_geometric_candidate"]) for row in selected) / len(sources) if sources else 0.0,
        "baseline_mean_alignment_ssim": statistics.mean(baseline_alignment) if baseline_alignment else 0.0,
        "oof_mean_alignment_ssim": statistics.mean(selected_alignment) if selected_alignment else 0.0,
        "selection_count": len(selected),
    }


def _write_csv(path: Path, rows: list[dict]) -> None:
    fields: list[str] = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields or ["annotation_id"])
        writer.writeheader()
        writer.writerows(rows)


def _report(summary: dict, lookup: dict[tuple[str, str], dict]) -> str:
    all_rows = lookup[("synthetic_low_quality", "all")]
    positives = lookup[("synthetic_low_quality", "present")]
    low_positives = lookup[("quality_below_0_25", "present")]
    gates = "\n".join(f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items())
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Formal Test accessed in this run: `false`
- Development sources/candidate rows: {summary['source_count']} / {summary['candidate_detail_rows']}
- Errors: {summary['error_count']}

## Group-OOF candidate selection

| Scope | Top-3 Recall | Baseline geometric | OOF selector geometric | Baseline alignment | OOF alignment |
|---|---:|---:|---:|---:|---:|
| All low-quality | {all_rows['top3_recall']:.2%} | {all_rows['baseline_geometric_success']:.2%} | {all_rows['oof_geometric_success']:.2%} | {all_rows['baseline_mean_alignment_ssim']:.4f} | {all_rows['oof_mean_alignment_ssim']:.4f} |
| Presence-positive | {positives['top3_recall']:.2%} | {positives['baseline_geometric_success']:.2%} | {positives['oof_geometric_success']:.2%} | {positives['baseline_mean_alignment_ssim']:.4f} | {positives['oof_mean_alignment_ssim']:.4f} |
| Quality < 0.25 positive | {low_positives['top3_recall']:.2%} | {low_positives['baseline_geometric_success']:.2%} | {low_positives['oof_geometric_success']:.2%} | {low_positives['baseline_mean_alignment_ssim']:.4f} | {low_positives['oof_mean_alignment_ssim']:.4f} |

## Gate

{gates}

이 selector는 GT-IoU가 있는 기존 개발 데이터에서만 학습·OOF 평가했다. Test 22장은 접근하지 않았으며, 통과해도 후속 존재 FPR/OCR guard와 신규 실제 RGB/IR holdout이 필요하다.
"""


def run(args: argparse.Namespace) -> dict:
    side_model_summary = json.loads(Path(args.side_model_summary).resolve().read_text(encoding="utf-8-sig"))
    if int(side_model_summary.get("test_rows_included", 0)):
        raise ValueError("formal_test_in_side_model_summary")
    candidate_rows, source_manifest, source_rows, errors = build_candidate_rows(
        Path(args.annotation_manifest).resolve(),
        Path(args.split_manifest).resolve(),
        Path(args.icon_manifest).resolve(),
        Path(args.thresholds).resolve(),
        side_model_summary,
    )
    baseline_rows = [
        row
        for row in read_csv(Path(args.baseline_details).resolve())
        if row.get("variant") == "quality_aware" and row.get("environment") in LOW_QUALITY_ENVIRONMENTS
    ]
    if any(row.get("split") == "test" for row in baseline_rows):
        raise ValueError("formal_test_in_selector_baseline")
    baseline_lookup = {(row["annotation_id"], row["environment"]): row for row in baseline_rows}
    expected_keys = {(row["annotation_id"], row["environment"]) for row in source_rows}
    if expected_keys - set(baseline_lookup):
        raise ValueError(f"selector_baseline_join_missing:{len(expected_keys - set(baseline_lookup))}")
    audit, final_model = apply_group_oof_selector(candidate_rows, args.fold_count)
    metrics = [
        summarize_selection(candidate_rows, source_rows, baseline_lookup, cohort, label_scope)
        for cohort in ("synthetic_low_quality", "quality_below_0_25")
        for label_scope in ("all", "present", "absent")
    ]
    lookup = {(row["cohort"], row["label_scope"]): row for row in metrics}
    all_rows = lookup[("synthetic_low_quality", "all")]
    positives = lookup[("synthetic_low_quality", "present")]
    low_positives = lookup[("quality_below_0_25", "present")]
    gate = {
        "top3_recall_non_regression": all_rows["top3_recall"] >= all_rows["baseline_geometric_success"],
        "overall_geometric_success_improves_0_05": all_rows["oof_geometric_success"] >= all_rows["baseline_geometric_success"] + 0.05,
        "presence_positive_geometric_success_improves_0_05": positives["oof_geometric_success"] >= positives["baseline_geometric_success"] + 0.05,
        "quality_below_0_25_positive_geometric_success_improves_one_row": low_positives["oof_geometric_success"] >= low_positives["baseline_geometric_success"] + (1.0 / max(1, low_positives["row_count"])),
        "overall_alignment_regression_le_0_02": all_rows["oof_mean_alignment_ssim"] >= all_rows["baseline_mean_alignment_ssim"] - 0.02,
        "presence_alignment_regression_le_0_02": positives["oof_mean_alignment_ssim"] >= positives["baseline_mean_alignment_ssim"] - 0.02,
        "group_overlap_zero": all(not item["group_overlap"] for item in audit),
        "formal_test_rows_zero": not any(row["split"] == "test" for row in source_manifest),
        "processing_errors_zero": not errors,
    }
    status = "selector_development_candidate" if all(gate.values()) else "rejected"
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "source_count": len(source_manifest),
        "source_environment_rows": len(source_rows),
        "candidate_detail_rows": len(candidate_rows),
        "quality_threshold": QUALITY_THRESHOLD,
        "iou_threshold": IOU_THRESHOLD,
        "selector_features": list(SELECTOR_FEATURES),
        "fold_audit": audit,
        "frozen_model": final_model,
        "gate": gate,
        "error_count": len(errors),
        "errors": errors,
        "formal_test_accessed_in_this_run": False,
        "formal_test_previously_opened": bool(args.formal_test_previously_opened),
        "formal_test_reuse_for_tuning_prohibited": True,
        "operational_candidate": False,
        "real_ir_evaluated": False,
        "independent_holdout_evaluated": False,
        "performance_claim": False,
    }
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _write_csv(output_dir / "manifest.csv", source_manifest)
    _write_csv(output_dir / "details.csv", candidate_rows)
    _write_csv(output_dir / "source_environment_details.csv", source_rows)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "REPORT.md").write_text(_report(summary, lookup), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation-manifest", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--thresholds", required=True)
    parser.add_argument("--side-model-summary", required=True)
    parser.add_argument("--baseline-details", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--fold-count", type=int, default=5)
    parser.add_argument("--formal-test-previously-opened", action="store_true")
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    return 0 if summary["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

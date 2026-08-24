"""Development-only vehicle presence calibration with grouped hard negatives."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
import time
from collections import Counter
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
    _group_fold,
    _icon_patch_features,
    _predict_logistic,
    apply_environment,
)
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .roi_annotation import read_csv


EXPERIMENT_ID = "low-quality-vehicle-presence-hard-negative-005"
POLICY_ALPHAS = (0.50, 0.75, 1.00)
COHORTS = ("synthetic_low_quality", "quality_below_0_25", "quality_at_or_above_0_25")


def _write_csv(path: Path, rows: list[dict]) -> None:
    fields: list[str] = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    if not fields:
        fields = ["source_id"]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def _read_unicode(path: Path) -> np.ndarray | None:
    try:
        return cv2.imdecode(np.fromfile(str(path), dtype=np.uint8), cv2.IMREAD_COLOR)
    except OSError:
        return None


def _model_from_summary(summary: dict, side: str) -> dict:
    source = summary["logistic_models"][side]["final_model"]
    return {
        "mean": np.asarray(source["mean"], dtype=np.float64),
        "scale": np.asarray(source["scale"], dtype=np.float64),
        "coefficients": np.asarray(source["coefficients"], dtype=np.float64),
    }


def _vehicle_label(row: dict) -> str:
    left = str(row.get("left_icon_label", ""))
    right = str(row.get("right_icon_label", ""))
    if "present" in {left, right}:
        return "present"
    if left == "absent" and right == "absent":
        return "absent"
    return "excluded"


def load_development_rows(path: Path) -> list[dict]:
    result: list[dict] = []
    for source in read_csv(path):
        if source.get("split") == "test":
            raise ValueError("formal_test_row_in_development_details")
        if source.get("variant") != "quality_aware" or source.get("environment") not in LOW_QUALITY_ENVIRONMENTS:
            continue
        result.append(
            {
                "source_id": source["annotation_id"],
                "source_group": f"pilot:{source['annotation_id']}",
                "source_set": "pilot_development",
                "source_path": source["source_path"],
                "original_split": source["split"],
                "ground_truth": source["ground_truth"],
                "vehicle_presence_label": _vehicle_label(source),
                "label_provenance": "corrected_vehicle_and_human_icon_labels",
                "environment": source["environment"],
                "quality_score": float(source["gt_crop_quality"]),
                "quality_below_threshold": int(source["quality_below_threshold"]),
                "quality_score_source": "ground_truth_plate_crop",
                "geometric_success": int(source["geometric_success"]),
                "alignment_ssim": float(source["alignment_ssim"]),
                "rectification_method": source["rectification_method"],
                "rectification_quality": float(source["rectification_quality"]),
                "rectification_geometry_confidence": float(source["rectification_geometry_confidence"]),
                "left_icon_score": float(source["left_icon_score"]),
                "right_icon_score": float(source["right_icon_score"]),
                "left_icon_logistic_score": float(source.get("left_icon_logistic_score") or -1.0),
                "right_icon_logistic_score": float(source.get("right_icon_logistic_score") or -1.0),
                "processing_ms": float(source["processing_ms"]),
                "error_reason": source.get("error_reason", ""),
            }
        )
    if not result:
        raise ValueError("development_details_empty")
    return result


def _extract_icon_features(
    image: np.ndarray,
    side: str,
    template_cache: dict,
    base: dict,
) -> dict:
    positive_templates = template_cache[(side, "positive", "")]
    negative_templates = template_cache[(side, "negative", "")]
    detection = detect_icon_contrastive(image, side, positive_templates, negative_templates)
    result = {
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
        result[f"{side}_feature_{name}"] = float(detection.get(name, 0.0))
    for name, value in _icon_patch_features(image, detection.get("bbox", [])).items():
        result[f"{side}_feature_{name}"] = value
    result.update({name: base[name] for name in LOGISTIC_GLOBAL_FEATURES})
    return result


def evaluate_ocr_hard_negatives(
    manifest_path: Path,
    template_cache: dict,
    models: dict[str, dict],
) -> tuple[list[dict], list[dict]]:
    rows: list[dict] = []
    errors: list[dict] = []
    manifest = read_csv(manifest_path)
    if not manifest:
        raise ValueError("ocr_hard_negative_manifest_empty")
    for source in manifest:
        input_path = Path(source["input_path"])
        image = _read_unicode(input_path)
        source_id = str(source.get("vehicle_id") or source.get("source_id"))
        if image is None:
            errors.append({"source_id": source_id, "reason": "unreadable_input"})
            continue
        for environment in sorted(LOW_QUALITY_ENVIRONMENTS):
            started = time.perf_counter()
            try:
                degraded = apply_environment(image, environment)
                normalized = cv2.resize(degraded, TARGET_SIZE, interpolation=cv2.INTER_AREA)
                input_quality = rectification_quality_score(normalized)
                rectified = rectify_plate(degraded, TARGET_SIZE)
                geometric_success = int(rectified.method in GEOMETRIC_METHODS)
                output_quality = rectification_quality_score(rectified.plate_image)
                base = {
                    "rectification_geometry_confidence": float(rectified.rect_quality_input),
                    "rectification_quality": float(output_quality),
                    "quality_aware_fallback_used": 1.0,
                }
                item = {
                    "source_id": source_id,
                    "source_group": f"ocr-hard-negative:{source_id}",
                    "source_set": "ocr_hard_negative",
                    "source_path": str(input_path),
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
                    "error_reason": "",
                }
                for side in ("left", "right"):
                    feature_row = _extract_icon_features(rectified.plate_image, side, template_cache, base)
                    raw_score = _predict_logistic(models[side], feature_row, side)
                    item[f"{side}_icon_logistic_score"] = raw_score
                    item[f"{side}_icon_score"] = raw_score if geometric_success else -1.0
                item["processing_ms"] = (time.perf_counter() - started) * 1000.0
                rows.append(item)
            except (cv2.error, ValueError) as exc:
                errors.append(
                    {
                        "source_id": source_id,
                        "environment": environment,
                        "reason": f"{type(exc).__name__}:{str(exc).splitlines()[0][:160]}",
                    }
                )
    return rows, errors


def vehicle_score(row: dict, alpha: float) -> float:
    left = float(row["left_icon_score"])
    right = float(row["right_icon_score"])
    high, low = max(left, right), min(left, right)
    return float(alpha * high + (1.0 - alpha) * low)


def _cohort_match(row: dict, cohort: str) -> bool:
    below = bool(int(row["quality_below_threshold"]))
    if cohort == "quality_below_0_25":
        return below
    if cohort == "quality_at_or_above_0_25":
        return not below
    return True


def choose_vehicle_policy(rows: list[dict], cohort: str, fpr_cap: float) -> dict:
    eligible = [
        row
        for row in rows
        if row["vehicle_presence_label"] in {"present", "absent"} and _cohort_match(row, cohort)
    ]
    positives = [row for row in eligible if row["vehicle_presence_label"] == "present"]
    negatives = [row for row in eligible if row["vehicle_presence_label"] == "absent"]
    if not positives or not negatives:
        raise ValueError(f"vehicle_policy_class_missing:{cohort}:{len(positives)}:{len(negatives)}")
    best: dict | None = None
    best_key = (-1.0, -1.0, -1.0, -1.0)
    for alpha in POLICY_ALPHAS:
        scores = sorted({vehicle_score(row, alpha) for row in eligible})
        thresholds = [max(scores) + 1e-6, *scores]
        for threshold in thresholds:
            recall = sum(
                bool(int(row["geometric_success"])) and vehicle_score(row, alpha) >= threshold
                for row in positives
            ) / len(positives)
            fpr = sum(
                bool(int(row["geometric_success"])) and vehicle_score(row, alpha) >= threshold
                for row in negatives
            ) / len(negatives)
            if fpr > fpr_cap:
                continue
            key = (recall, -fpr, threshold, alpha)
            if key > best_key:
                best_key = key
                best = {
                    "alpha": float(alpha),
                    "threshold": float(threshold),
                    "recall": float(recall),
                    "fpr": float(fpr),
                    "positive_count": len(positives),
                    "negative_count": len(negatives),
                }
    if best is None:
        raise ValueError(f"vehicle_policy_not_found:{cohort}")
    return best


def apply_policy(row: dict, policy: dict, prefix: str) -> None:
    score = vehicle_score(row, float(policy["alpha"]))
    detected = int(bool(int(row["geometric_success"])) and score >= float(policy["threshold"]))
    if detected:
        decision = "PRESENT"
    elif not int(row["geometric_success"]) or int(row["quality_below_threshold"]):
        decision = "REVIEW"
    else:
        decision = "ABSENT"
    row[f"{prefix}_vehicle_score"] = score
    row[f"{prefix}_vehicle_present"] = detected
    row[f"{prefix}_presence_decision"] = decision
    row[f"{prefix}_alpha"] = float(policy["alpha"])
    row[f"{prefix}_threshold"] = float(policy["threshold"])


def apply_group_oof_vehicle_policy(
    rows: list[dict],
    fold_count: int,
    fpr_cap: float,
) -> tuple[list[dict], dict[str, dict]]:
    audit: list[dict] = []
    for row in rows:
        row["policy_fold"] = _group_fold(str(row["source_group"]), fold_count)
    for fold in range(fold_count):
        fit_rows = [row for row in rows if int(row["policy_fold"]) != fold]
        holdout_rows = [row for row in rows if int(row["policy_fold"]) == fold]
        fit_groups = {str(row["source_group"]) for row in fit_rows}
        holdout_groups = {str(row["source_group"]) for row in holdout_rows}
        overlap = fit_groups & holdout_groups
        if overlap:
            raise ValueError(f"vehicle_policy_group_leak:{fold}:{sorted(overlap)[:3]}")
        policies = {
            cohort: choose_vehicle_policy(fit_rows, cohort, fpr_cap)
            for cohort in ("quality_below_0_25", "quality_at_or_above_0_25")
        }
        for row in holdout_rows:
            cohort = "quality_below_0_25" if int(row["quality_below_threshold"]) else "quality_at_or_above_0_25"
            apply_policy(row, policies[cohort], "oof")
        audit.append(
            {
                "fold": fold,
                "fit_group_count": len(fit_groups),
                "holdout_group_count": len(holdout_groups),
                "group_overlap": 0,
                "policies": policies,
            }
        )
    final_policies = {
        cohort: choose_vehicle_policy(rows, cohort, fpr_cap)
        for cohort in ("quality_below_0_25", "quality_at_or_above_0_25")
    }
    for row in rows:
        cohort = "quality_below_0_25" if int(row["quality_below_threshold"]) else "quality_at_or_above_0_25"
        apply_policy(row, final_policies[cohort], "frozen")
    return audit, final_policies


def summarize_rows(rows: list[dict], source_set: str, cohort: str) -> dict:
    selected = [row for row in rows if (source_set == "all" or row["source_set"] == source_set) and _cohort_match(row, cohort)]
    labeled = [row for row in selected if row["vehicle_presence_label"] in {"present", "absent"}]
    positives = [row for row in labeled if row["vehicle_presence_label"] == "present"]
    negatives = [row for row in labeled if row["vehicle_presence_label"] == "absent"]
    alignment = [
        float(row["alignment_ssim"])
        for row in selected
        if row["alignment_ssim"] != "" and int(row["geometric_success"])
    ]
    times = [float(row["processing_ms"]) for row in selected]
    return {
        "source_set": source_set,
        "cohort": cohort,
        "row_count": len(selected),
        "unique_source_count": len({row["source_group"] for row in selected}),
        "positive_count": len(positives),
        "negative_count": len(negatives),
        "oof_vehicle_recall": (
            sum(int(row["oof_vehicle_present"]) for row in positives) / len(positives) if positives else 0.0
        ),
        "oof_vehicle_fpr": (
            sum(int(row["oof_vehicle_present"]) for row in negatives) / len(negatives) if negatives else 0.0
        ),
        "oof_prescreen_recall": (
            sum(row["oof_presence_decision"] in {"PRESENT", "REVIEW"} for row in positives) / len(positives)
            if positives
            else 0.0
        ),
        "oof_review_rate": (
            sum(row["oof_presence_decision"] == "REVIEW" for row in labeled) / len(labeled) if labeled else 0.0
        ),
        "geometric_rectification_success": (
            sum(int(row["geometric_success"]) for row in selected) / len(selected) if selected else 0.0
        ),
        "mean_alignment_ssim": statistics.mean(alignment) if alignment else 0.0,
        "mean_processing_ms": statistics.mean(times) if times else 0.0,
        "p95_processing_ms": _percentile(times, 0.95),
        "error_count": sum(bool(row["error_reason"]) for row in selected),
    }


def _percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(np.ceil(len(ordered) * quantile)) - 1))
    return float(ordered[index])


def _manifest_rows(rows: list[dict]) -> list[dict]:
    grouped: dict[str, list[dict]] = {}
    for row in rows:
        grouped.setdefault(str(row["source_group"]), []).append(row)
    result = []
    for group, items in sorted(grouped.items()):
        first = items[0]
        result.append(
            {
                "source_group": group,
                "source_id": first["source_id"],
                "source_set": first["source_set"],
                "source_path": first["source_path"],
                "original_split": first["original_split"],
                "ground_truth": first["ground_truth"],
                "vehicle_presence_label": first["vehicle_presence_label"],
                "label_provenance": first["label_provenance"],
                "environment_rows": len(items),
                "policy_fold": first["policy_fold"],
                "formal_test_member": 0,
            }
        )
    return result


def _report(summary: dict, lookup: dict[tuple[str, str], dict]) -> str:
    combined = lookup[("all", "synthetic_low_quality")]
    low = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    gate_lines = "\n".join(f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items())
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Formal Test accessed in this run: `false`
- Development sources: {summary['development_source_count']}; OCR hard-negative sources: {summary['hard_negative_source_count']}
- Detail rows: {summary['detail_rows']}; errors: {summary['error_count']}

## Group-OOF vehicle presence

| Cohort | Recall | FPR | Prescreen Recall | REVIEW | Geometric success |
|---|---:|---:|---:|---:|---:|
| Combined synthetic low-quality | {combined['oof_vehicle_recall']:.2%} | {combined['oof_vehicle_fpr']:.2%} | {combined['oof_prescreen_recall']:.2%} | {combined['oof_review_rate']:.2%} | {combined['geometric_rectification_success']:.2%} |
| Combined quality < 0.25 | {low['oof_vehicle_recall']:.2%} | {low['oof_vehicle_fpr']:.2%} | {low['oof_prescreen_recall']:.2%} | {low['oof_review_rate']:.2%} | {low['geometric_rectification_success']:.2%} |
| Pilot development | {pilot['oof_vehicle_recall']:.2%} | {pilot['oof_vehicle_fpr']:.2%} | {pilot['oof_prescreen_recall']:.2%} | {pilot['oof_review_rate']:.2%} | {pilot['geometric_rectification_success']:.2%} |
| OCR NON_EV hard negatives | N/A | {hard['oof_vehicle_fpr']:.2%} | N/A | {hard['oof_review_rate']:.2%} | {hard['geometric_rectification_success']:.2%} |

## Frozen policy for the next independent holdout

```json
{json.dumps(summary['frozen_policy'], ensure_ascii=False, indent=2)}
```

## Gate

{gate_lines}

이 후보는 기존 개발 데이터에서 동결한 post-processing 정책이다. 이미 개봉된 Test를 재사용하지 않았고, 신규 실제 RGB/IR 홀드아웃 통과 전에는 운영·Pi/API 성능으로 주장하지 않는다. OCR NON_EV의 아이콘 음성은 dataset class 기반 개발 라벨이며 독립 사람 아이콘 검수 GT가 아니다.
"""


def run(args: argparse.Namespace) -> dict:
    development_summary = json.loads(Path(args.development_summary).resolve().read_text(encoding="utf-8-sig"))
    if int(development_summary.get("test_rows_included", 0)):
        raise ValueError("formal_test_in_development_summary")
    development_rows = load_development_rows(Path(args.development_details).resolve())
    split_rows = {
        row["annotation_id"]: row for row in read_csv(Path(args.split_manifest).resolve())
    }
    icon_rows = read_csv(Path(args.icon_manifest).resolve())
    template_rows, _ = _corrected_icon_records(icon_rows, split_rows, include_test_labels=False)
    test_ids = {key for key, value in split_rows.items() if value.get("split") == "test"}
    template_ids = {row["annotation_id"] for row in template_rows}
    template_test_overlap = sorted(template_ids & test_ids)
    if template_test_overlap:
        raise ValueError(f"formal_test_template_overlap:{template_test_overlap[:3]}")
    template_cache = _build_template_cache(template_rows)
    models = {side: _model_from_summary(development_summary, side) for side in ("left", "right")}
    hard_negative_rows, errors = evaluate_ocr_hard_negatives(
        Path(args.ocr_hard_negative_manifest).resolve(), template_cache, models
    )
    rows = [*development_rows, *hard_negative_rows]
    audit, final_policy = apply_group_oof_vehicle_policy(rows, args.fold_count, args.calibration_fpr_cap)
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
    ocr_guard = json.loads(Path(args.ocr_guard_summary).resolve().read_text(encoding="utf-8-sig"))
    gate = {
        "combined_oof_vehicle_recall_at_least_min": combined["oof_vehicle_recall"] >= args.min_vehicle_recall,
        "combined_oof_vehicle_fpr_le_gate": combined["oof_vehicle_fpr"] <= args.gate_fpr,
        "quality_below_0_25_oof_vehicle_recall_at_least_min": low["oof_vehicle_recall"] >= args.min_very_low_recall,
        "quality_below_0_25_oof_vehicle_fpr_le_gate": low["oof_vehicle_fpr"] <= args.gate_fpr,
        "quality_below_0_25_prescreen_recall_one": low["oof_prescreen_recall"] == 1.0,
        "pilot_corrected_negative_fpr_le_0_15": pilot["oof_vehicle_fpr"] <= 0.15,
        "ocr_hard_negative_fpr_le_gate": hard["oof_vehicle_fpr"] <= args.gate_fpr,
        "pilot_geometric_rectification_at_least_0_54": pilot["geometric_rectification_success"] >= 0.54,
        "ocr_hard_negative_geometric_rectification_at_least_0_80": hard["geometric_rectification_success"] >= 0.80,
        "prior_ocr_non_regression_guard_passed": ocr_guard.get("status") == "ocr_guard_pass",
        "group_overlap_zero": all(not item["group_overlap"] for item in audit),
        "formal_test_rows_zero": not any(row.get("original_split") == "test" for row in rows),
        "formal_test_templates_zero": not template_test_overlap,
        "processing_errors_zero": not errors,
    }
    status = "frozen_development_candidate" if all(gate.values()) else "rejected"
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "development_source_count": len({row["source_group"] for row in development_rows}),
        "hard_negative_source_count": len({row["source_group"] for row in hard_negative_rows}),
        "detail_rows": len(rows),
        "quality_threshold": QUALITY_THRESHOLD,
        "calibration_fpr_cap": args.calibration_fpr_cap,
        "gate_fpr": args.gate_fpr,
        "policy_alphas": list(POLICY_ALPHAS),
        "frozen_policy": final_policy,
        "fold_audit": audit,
        "template_source_split": "train_only",
        "template_count": len(template_rows),
        "template_test_overlap": template_test_overlap,
        "development_split_counts": dict(Counter(row["original_split"] for row in development_rows)),
        "hard_negative_label_provenance": "non_ev_ocr_dataset_class_inferred_icon_absent",
        "error_count": len(errors),
        "errors": errors,
        "gate": gate,
        "formal_test_accessed_in_this_run": False,
        "formal_test_previously_opened": bool(args.formal_test_previously_opened),
        "formal_test_reuse_for_tuning_prohibited": True,
        "ocr_guard_reference": str(Path(args.ocr_guard_summary).resolve()),
        "ocr_guard_status": ocr_guard.get("status"),
        "operational_candidate": False,
        "real_ir_evaluated": False,
        "independent_holdout_evaluated": False,
        "performance_claim": False,
    }
    _write_csv(output_dir / "manifest.csv", _manifest_rows(rows))
    _write_csv(output_dir / "details.csv", rows)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "REPORT.md").write_text(_report(summary, lookup), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--development-details", required=True)
    parser.add_argument("--development-summary", required=True)
    parser.add_argument("--ocr-hard-negative-manifest", required=True)
    parser.add_argument("--ocr-guard-summary", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--fold-count", type=int, default=5)
    parser.add_argument("--calibration-fpr-cap", type=float, default=0.08)
    parser.add_argument("--gate-fpr", type=float, default=0.10)
    parser.add_argument("--min-vehicle-recall", type=float, default=0.40)
    parser.add_argument("--min-very-low-recall", type=float, default=0.35)
    parser.add_argument("--formal-test-previously-opened", action="store_true")
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    return 0 if summary["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

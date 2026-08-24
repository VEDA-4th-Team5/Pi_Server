"""Evaluate a runtime-only REVIEW guard for icon-side observability failures."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from .low_quality_spatial_icon import (
    BLUE_SLICE,
    EDGE_SLICE,
    HOG_BINS,
    HOG_SLICE,
    INTENSITY_SLICE,
    SATURATION_SLICE,
    SPATIAL_FEATURE_NAMES,
    VISIBILITY_PROXY_THRESHOLD,
)
from .low_quality_vehicle_presence_calibration import COHORTS, _write_csv
from .roi_annotation import read_csv


EXPERIMENT_ID = "low-quality-runtime-icon-observability-guard-014-rerun-001"
FOLD_COUNT = 3
MODEL_FEATURE_NAMES = (
    "rectification_quality",
    "rectification_geometry_confidence",
    "intensity_grid_std",
    "edge_mean",
    "edge_std",
    "blue_mean",
    "saturation_mean",
    "hog_active_cell_ratio",
    "hog_dominant_bin_mean",
    "hog_entropy",
    "patch_gray_contrast",
    "patch_edge_density",
    "patch_laplacian_log",
)


def _as_float(row: dict, field: str) -> float:
    return float(row[field])


def runtime_feature_vector(row: dict, side: str) -> np.ndarray:
    spatial = np.asarray(
        [_as_float(row, f"{side}_{name}") for name in SPATIAL_FEATURE_NAMES],
        dtype=np.float64,
    )
    hog = spatial[HOG_SLICE].reshape(-1, HOG_BINS)
    hog_norm = np.linalg.norm(hog, axis=1)
    hog_sum = np.sum(hog, axis=1, keepdims=True)
    distribution = np.divide(hog, hog_sum, out=np.zeros_like(hog), where=hog_sum > 1e-12)
    entropy = -np.sum(distribution * np.log(np.clip(distribution, 1e-12, 1.0)), axis=1)
    entropy /= np.log(float(HOG_BINS))
    values = np.asarray(
        [
            _as_float(row, "rectification_quality"),
            _as_float(row, "rectification_geometry_confidence"),
            float(np.std(spatial[INTENSITY_SLICE])),
            float(np.mean(spatial[EDGE_SLICE])),
            float(np.std(spatial[EDGE_SLICE])),
            float(np.mean(spatial[BLUE_SLICE])),
            float(np.mean(spatial[SATURATION_SLICE])),
            float(np.mean(hog_norm > 1e-8)),
            float(np.mean(np.max(hog, axis=1))),
            float(np.mean(entropy)),
            _as_float(row, f"{side}_feature_gray_contrast"),
            _as_float(row, f"{side}_feature_edge_density"),
            _as_float(row, f"{side}_feature_laplacian_log"),
        ],
        dtype=np.float64,
    )
    if values.shape != (len(MODEL_FEATURE_NAMES),) or not np.all(np.isfinite(values)):
        raise ValueError(f"runtime_observability_feature_invalid:{side}:{values.shape}")
    return values


def attach_runtime_features(rows: list[dict]) -> None:
    for row in rows:
        for side in ("left", "right"):
            values = runtime_feature_vector(row, side)
            # Python 3.9 기반 Raspberry Pi에서도 동작하도록 strict 인자를 쓰지 않는다.
            # runtime_feature_vector()가 바로 위에서 동일 길이를 이미 검증한다.
            for name, value in zip(MODEL_FEATURE_NAMES, values):
                row[f"{side}_guard_feature_{name}"] = float(value)


def _attached_feature_vector(row: dict, side: str) -> np.ndarray:
    return np.asarray(
        [float(row[f"{side}_guard_feature_{name}"]) for name in MODEL_FEATURE_NAMES],
        dtype=np.float64,
    )


def _eligible_present_side_records(rows: list[dict]) -> list[tuple[dict, str, int]]:
    result: list[tuple[dict, str, int]] = []
    for row in rows:
        if row.get("source_set") != "pilot_development" or not int(row["geometric_success"]):
            continue
        for side in ("left", "right"):
            if row.get(f"{side}_icon_label") != "present":
                continue
            status = row.get(f"{side}_visibility_status")
            if status not in {"low_visibility_proxy", "retained_visibility_proxy"}:
                continue
            result.append((row, side, int(status == "low_visibility_proxy")))
    return result


def assign_source_folds(rows: list[dict], fold_count: int = FOLD_COUNT) -> dict[str, int]:
    if fold_count != FOLD_COUNT:
        raise ValueError(f"runtime_guard_fold_count_must_be_{FOLD_COUNT}")
    low_groups = sorted(
        {
            str(row["source_group"])
            for row, _, target in _eligible_present_side_records(rows)
            if target
        }
    )
    if len(low_groups) != fold_count:
        raise ValueError(f"low_visibility_source_count:{len(low_groups)}")
    mapping = {group: fold for fold, group in enumerate(low_groups)}
    for group in sorted({str(row["source_group"]) for row in rows}):
        if group in mapping:
            continue
        digest = hashlib.sha256(group.encode("utf-8")).digest()
        mapping[group] = int.from_bytes(digest[:8], "big") % fold_count
    for row in rows:
        row["guard_fold"] = mapping[str(row["source_group"])]
    return mapping


def _sigmoid(values: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(values, -30.0, 30.0)))


def fit_guard_model(records: list[tuple[dict, str, int]]) -> dict:
    if not records:
        raise ValueError("runtime_guard_training_empty")
    x = np.vstack([_attached_feature_vector(row, side) for row, side, _ in records])
    y = np.asarray([float(target) for _, _, target in records], dtype=np.float64)
    positive_count = int(np.sum(y == 1.0))
    negative_count = int(np.sum(y == 0.0))
    if not positive_count or not negative_count:
        raise ValueError(f"runtime_guard_class_missing:{positive_count}:{negative_count}")
    mean = np.mean(x, axis=0)
    scale = np.std(x, axis=0)
    scale[scale < 1e-6] = 1.0
    normalized = (x - mean) / scale
    design = np.column_stack([normalized, np.ones(len(normalized), dtype=np.float64)])
    sample_weight = np.where(
        y == 1.0,
        len(y) / (2.0 * positive_count),
        len(y) / (2.0 * negative_count),
    )
    coefficients = np.zeros(design.shape[1], dtype=np.float64)
    learning_rate = 0.04
    regularization = 0.08
    iterations = 1200
    for _ in range(iterations):
        probability = _sigmoid(design @ coefficients)
        gradient = design.T @ (sample_weight * (probability - y)) / float(np.sum(sample_weight))
        gradient[:-1] += regularization * coefficients[:-1]
        coefficients -= learning_rate * gradient
    model = {
        "mean": mean,
        "scale": scale,
        "coefficients": coefficients,
        "positive_count": positive_count,
        "negative_count": negative_count,
        "learning_rate": learning_rate,
        "regularization": regularization,
        "iterations": iterations,
    }
    probabilities = [predict_guard_model(model, row, side) for row, side, target in records if target]
    model["threshold"] = float(min(probabilities))
    return model


def predict_guard_model(model: dict, row: dict, side: str) -> float:
    normalized = (_attached_feature_vector(row, side) - model["mean"]) / model["scale"]
    design = np.append(normalized, 1.0)
    return float(_sigmoid(np.asarray([design @ model["coefficients"]]))[0])


def _serialize_model(model: dict) -> dict:
    return {
        "feature_names": list(MODEL_FEATURE_NAMES),
        "mean": model["mean"].tolist(),
        "scale": model["scale"].tolist(),
        "coefficients": model["coefficients"].tolist(),
        "threshold": model["threshold"],
        "positive_count": model["positive_count"],
        "negative_count": model["negative_count"],
        "learning_rate": model["learning_rate"],
        "regularization": model["regularization"],
        "iterations": model["iterations"],
    }


def apply_review_guard(row: dict, prefix: str) -> None:
    baseline = str(row["oof_presence_decision"])
    flagged = int(
        bool(int(row[f"{prefix}_left_observability_flag"]))
        or bool(int(row[f"{prefix}_right_observability_flag"]))
    )
    if baseline == "ABSENT" and flagged:
        decision = "REVIEW"
        reason = "review_runtime_icon_observability_low_confidence"
    else:
        decision = baseline
        reason = str(row.get("oof_decision_reason", "unchanged"))
    row[f"{prefix}_guard_observability_flag"] = flagged
    row[f"{prefix}_guard_presence_decision"] = decision
    row[f"{prefix}_guard_decision_reason"] = reason
    row[f"{prefix}_guard_vehicle_present"] = int(row["oof_vehicle_present"])


def apply_group_oof_guard(
    rows: list[dict], fold_count: int = FOLD_COUNT
) -> tuple[list[dict], dict]:
    assign_source_folds(rows, fold_count)
    audit: list[dict] = []
    for fold in range(fold_count):
        fit_rows = [row for row in rows if int(row["guard_fold"]) != fold]
        holdout_rows = [row for row in rows if int(row["guard_fold"]) == fold]
        fit_groups = {str(row["source_group"]) for row in fit_rows}
        holdout_groups = {str(row["source_group"]) for row in holdout_rows}
        overlap = sorted(fit_groups & holdout_groups)
        if overlap:
            raise ValueError(f"runtime_guard_group_overlap:{fold}:{overlap[:3]}")
        records = _eligible_present_side_records(fit_rows)
        model = fit_guard_model(records)
        threshold = float(model["threshold"])
        for row in holdout_rows:
            for side in ("left", "right"):
                probability = predict_guard_model(model, row, side)
                row[f"oof_{side}_observability_risk"] = probability
                row[f"oof_{side}_observability_flag"] = int(probability >= threshold)
            apply_review_guard(row, "oof")
        fit_low = [(row, side) for row, side, target in records if target]
        fit_retained = [(row, side) for row, side, target in records if not target]
        audit.append(
            {
                "fold": fold,
                "fit_group_count": len(fit_groups),
                "holdout_group_count": len(holdout_groups),
                "group_overlap": len(overlap),
                "fit_low_visibility_recall": sum(
                    predict_guard_model(model, row, side) >= threshold for row, side in fit_low
                )
                / len(fit_low),
                "fit_retained_false_review_rate": sum(
                    predict_guard_model(model, row, side) >= threshold for row, side in fit_retained
                )
                / len(fit_retained),
                "model": _serialize_model(model),
            }
        )
    final_model = fit_guard_model(_eligible_present_side_records(rows))
    threshold = float(final_model["threshold"])
    for row in rows:
        for side in ("left", "right"):
            probability = predict_guard_model(final_model, row, side)
            row[f"frozen_{side}_observability_risk"] = probability
            row[f"frozen_{side}_observability_flag"] = int(probability >= threshold)
        apply_review_guard(row, "frozen")
    return audit, _serialize_model(final_model)


def _cohort_match(row: dict, cohort: str) -> bool:
    if cohort == "synthetic_low_quality":
        return True
    if cohort == "quality_below_0_25":
        return bool(int(row["quality_below_threshold"]))
    if cohort == "quality_at_or_above_0_25":
        return not bool(int(row["quality_below_threshold"]))
    raise ValueError(f"unknown_cohort:{cohort}")


def summarize_decisions(rows: list[dict], source_set: str, cohort: str) -> dict:
    selected = [
        row
        for row in rows
        if (source_set == "all" or row["source_set"] == source_set) and _cohort_match(row, cohort)
    ]
    labeled = [row for row in selected if row["vehicle_presence_label"] in {"present", "absent"}]
    positives = [row for row in labeled if row["vehicle_presence_label"] == "present"]
    negatives = [row for row in labeled if row["vehicle_presence_label"] == "absent"]

    def rate(items: list[dict], predicate) -> float:
        return sum(predicate(row) for row in items) / len(items) if items else 0.0

    return {
        "source_set": source_set,
        "cohort": cohort,
        "row_count": len(selected),
        "positive_count": len(positives),
        "negative_count": len(negatives),
        "baseline_vehicle_recall": rate(positives, lambda row: int(row["oof_vehicle_present"])),
        "guard_vehicle_recall": rate(positives, lambda row: int(row["oof_guard_vehicle_present"])),
        "baseline_vehicle_fpr": rate(negatives, lambda row: int(row["oof_vehicle_present"])),
        "guard_vehicle_fpr": rate(negatives, lambda row: int(row["oof_guard_vehicle_present"])),
        "baseline_prescreen_recall": rate(
            positives, lambda row: row["oof_presence_decision"] in {"PRESENT", "REVIEW"}
        ),
        "guard_prescreen_recall": rate(
            positives, lambda row: row["oof_guard_presence_decision"] in {"PRESENT", "REVIEW"}
        ),
        "baseline_review_rate": rate(labeled, lambda row: row["oof_presence_decision"] == "REVIEW"),
        "guard_review_rate": rate(
            labeled, lambda row: row["oof_guard_presence_decision"] == "REVIEW"
        ),
        "added_review_rate": rate(
            labeled,
            lambda row: row["oof_presence_decision"] != "REVIEW"
            and row["oof_guard_presence_decision"] == "REVIEW",
        ),
    }


def summarize_observability(rows: list[dict]) -> list[dict]:
    records = _eligible_present_side_records(rows)
    result: list[dict] = []
    scopes = [
        ("all", records),
        ("low_visibility_proxy", [record for record in records if record[2]]),
        ("retained_visibility_proxy", [record for record in records if not record[2]]),
    ]
    scopes.extend(
        (
            f"environment:{environment}",
            [record for record in records if record[0]["environment"] == environment],
        )
        for environment in sorted({record[0]["environment"] for record in records})
    )
    for scope, selected in scopes:
        flags = [int(row[f"oof_{side}_observability_flag"]) for row, side, _ in selected]
        risks = [float(row[f"oof_{side}_observability_risk"]) for row, side, _ in selected]
        low_count = sum(target for _, _, target in selected)
        retained_count = len(selected) - low_count
        result.append(
            {
                "scope": scope,
                "side_count": len(selected),
                "source_count": len({row["source_group"] for row, _, _ in selected}),
                "low_visibility_count": low_count,
                "retained_visibility_count": retained_count,
                "flag_count": sum(flags),
                "flag_rate": sum(flags) / len(flags) if flags else 0.0,
                "low_visibility_recall": (
                    sum(
                        int(row[f"oof_{side}_observability_flag"])
                        for row, side, target in selected
                        if target
                    )
                    / low_count
                    if low_count
                    else 0.0
                ),
                "retained_false_review_rate": (
                    sum(
                        int(row[f"oof_{side}_observability_flag"])
                        for row, side, target in selected
                        if not target
                    )
                    / retained_count
                    if retained_count
                    else 0.0
                ),
                "mean_risk": float(np.mean(risks)) if risks else 0.0,
                "human_degraded_visibility_gt": False,
                "target_kind": "paired_visibility_proxy",
            }
        )
    return result


def _manifest_rows(rows: list[dict]) -> list[dict]:
    grouped: dict[str, list[dict]] = {}
    for row in rows:
        grouped.setdefault(str(row["source_group"]), []).append(row)
    result: list[dict] = []
    for group, items in sorted(grouped.items()):
        first = items[0]
        result.append(
            {
                "source_group": group,
                "source_id": first["source_id"],
                "source_set": first["source_set"],
                "source_path": first["source_path"],
                "original_split": first["original_split"],
                "vehicle_presence_label": first["vehicle_presence_label"],
                "environment_rows": len(items),
                "guard_fold": first["guard_fold"],
                "formal_test_member": 0,
            }
        )
    return result


def _report(summary: dict, lookup: dict[tuple[str, str], dict], observability: dict[str, dict]) -> str:
    combined = lookup[("all", "synthetic_low_quality")]
    low_quality = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    low = observability["low_visibility_proxy"]
    retained = observability["retained_visibility_proxy"]
    gates = "\n".join(
        f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()
    )
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Formal Test accessed in this run: `false`
- Sources/rows/errors: {summary['source_count']} / {summary['detail_rows']} / {summary['error_count']}
- Runtime model features: {summary['runtime_feature_count']}

## OOF observability proxy guard

| Proxy side scope | Count | Guard flag rate |
|---|---:|---:|
| Low visibility | {low['side_count']} | {low['low_visibility_recall']:.2%} |
| Retained visibility | {retained['side_count']} | {retained['retained_false_review_rate']:.2%} |

학습 target은 canonical 비교 visibility proxy이며 사람의 degraded-image visibility GT가 아니다. 모델 입력에는 canonical/proxy/사람 label/environment/alignment SSIM을 넣지 않고 selected rectification에서 런타임 계산 가능한 구조 feature만 사용했다.

## Decision effect

| Cohort | PRESENT Recall before/after | FPR before/after | Prescreen Recall before/after | REVIEW before/after |
|---|---:|---:|---:|---:|
| Combined | {combined['baseline_vehicle_recall']:.2%} / {combined['guard_vehicle_recall']:.2%} | {combined['baseline_vehicle_fpr']:.2%} / {combined['guard_vehicle_fpr']:.2%} | {combined['baseline_prescreen_recall']:.2%} / {combined['guard_prescreen_recall']:.2%} | {combined['baseline_review_rate']:.2%} / {combined['guard_review_rate']:.2%} |
| Quality < 0.25 | {low_quality['baseline_vehicle_recall']:.2%} / {low_quality['guard_vehicle_recall']:.2%} | {low_quality['baseline_vehicle_fpr']:.2%} / {low_quality['guard_vehicle_fpr']:.2%} | {low_quality['baseline_prescreen_recall']:.2%} / {low_quality['guard_prescreen_recall']:.2%} | {low_quality['baseline_review_rate']:.2%} / {low_quality['guard_review_rate']:.2%} |
| Corrected pilot | {pilot['baseline_vehicle_recall']:.2%} / {pilot['guard_vehicle_recall']:.2%} | {pilot['baseline_vehicle_fpr']:.2%} / {pilot['guard_vehicle_fpr']:.2%} | {pilot['baseline_prescreen_recall']:.2%} / {pilot['guard_prescreen_recall']:.2%} | {pilot['baseline_review_rate']:.2%} / {pilot['guard_review_rate']:.2%} |
| OCR hard negative | N/A | {hard['baseline_vehicle_fpr']:.2%} / {hard['guard_vehicle_fpr']:.2%} | N/A | {hard['baseline_review_rate']:.2%} / {hard['guard_review_rate']:.2%} |

- Entire-vehicle low-visibility rows: {summary['low_visibility_vehicle_row_count']}
- Auto-ABSENT misses before/after guard: {summary['low_visibility_vehicle_miss_auto_absent_before']} / {summary['low_visibility_vehicle_miss_auto_absent_after']}
- PRESENT decisions changed: {summary['present_decisions_changed_count']}

## Gate

{gates}

통과하더라도 proxy 7 side/3 source 기반 개발 후보일 뿐이다. 실제 사람 가시성 GT RGB/IR, 신규 독립 holdout, selector 적용 후 OCR 비회귀 및 end-to-end/Pi latency 없이는 운영 후보가 아니다.
"""


def run(args: argparse.Namespace) -> dict:
    source_summary = json.loads(Path(args.spatial_summary).resolve().read_text(encoding="utf-8-sig"))
    rows = read_csv(Path(args.spatial_details).resolve())
    if not rows:
        raise ValueError("runtime_guard_details_empty")
    if any(row.get("original_split") == "test" for row in rows):
        raise ValueError("formal_test_row_in_runtime_guard")
    errors: list[dict] = []
    try:
        attach_runtime_features(rows)
        audit, final_model = apply_group_oof_guard(rows, args.fold_count)
    except (KeyError, TypeError, ValueError) as exc:
        errors.append({"reason": f"{type(exc).__name__}:{str(exc)}"})
        raise
    metrics = [
        summarize_decisions(rows, source_set, cohort)
        for source_set in ("all", "pilot_development", "ocr_hard_negative")
        for cohort in COHORTS
    ]
    lookup = {(row["source_set"], row["cohort"]): row for row in metrics}
    observability_rows = summarize_observability(rows)
    observability = {row["scope"]: row for row in observability_rows}
    combined = lookup[("all", "synthetic_low_quality")]
    low_quality = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    low = observability["low_visibility_proxy"]
    retained = observability["retained_visibility_proxy"]
    low_visibility_vehicle_rows = [
        row
        for row in rows
        if row.get("source_set") == "pilot_development"
        and row.get("vehicle_visibility_status") == "low_visibility_proxy"
    ]
    rectification_failures = [row for row in rows if not int(row["geometric_success"])]
    present_decisions_changed = [
        row
        for row in rows
        if int(row["oof_vehicle_present"]) != int(row["oof_guard_vehicle_present"])
    ]
    invalid_transitions = [
        row
        for row in rows
        if row["oof_guard_presence_decision"] != row["oof_presence_decision"]
        and not (
            row["oof_presence_decision"] == "ABSENT"
            and row["oof_guard_presence_decision"] == "REVIEW"
        )
    ]
    gate = {
        "low_visibility_proxy_side_oof_recall_at_least_0_80": low[
            "low_visibility_recall"
        ]
        >= 0.80,
        "retained_visibility_proxy_side_false_review_le_0_20": retained[
            "retained_false_review_rate"
        ]
        <= 0.20,
        "low_visibility_vehicle_miss_auto_absent_after_zero": not any(
            not int(row["oof_guard_vehicle_present"])
            and row["oof_guard_presence_decision"] == "ABSENT"
            for row in low_visibility_vehicle_rows
        ),
        "pilot_guard_review_rate_le_0_35": pilot["guard_review_rate"] <= 0.35,
        "pilot_added_review_rate_le_0_15": pilot["added_review_rate"] <= 0.15,
        "hard_negative_guard_review_rate_le_0_45": hard["guard_review_rate"] <= 0.45,
        "hard_negative_added_review_rate_le_0_15": hard["added_review_rate"] <= 0.15,
        "combined_present_recall_unchanged": combined["guard_vehicle_recall"]
        == combined["baseline_vehicle_recall"],
        "combined_present_fpr_unchanged": combined["guard_vehicle_fpr"]
        == combined["baseline_vehicle_fpr"],
        "pilot_present_recall_fpr_unchanged": pilot["guard_vehicle_recall"]
        == pilot["baseline_vehicle_recall"]
        and pilot["guard_vehicle_fpr"] == pilot["baseline_vehicle_fpr"],
        "hard_negative_present_fpr_unchanged": hard["guard_vehicle_fpr"]
        == hard["baseline_vehicle_fpr"],
        "quality_below_prescreen_recall_one": low_quality["guard_prescreen_recall"] == 1.0,
        "rectification_failures_all_review": bool(rectification_failures)
        and all(row["oof_guard_presence_decision"] == "REVIEW" for row in rectification_failures),
        "present_decisions_changed_zero": not present_decisions_changed,
        "only_absent_to_review_transitions": not invalid_transitions,
        "source_group_overlap_zero": all(not item["group_overlap"] for item in audit),
        "formal_test_rows_zero": not any(row.get("original_split") == "test" for row in rows),
        "step104_preregistered_gates_passed": source_summary.get("status")
        == "spatial_descriptor_development_candidate"
        and all(source_summary.get("gate", {}).values()),
        "step104_performance_claim_false": source_summary.get("performance_claim") is False,
        "processing_errors_zero": not errors,
    }
    status = "runtime_observability_guard_development_candidate" if all(gate.values()) else "rejected"
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "source_experiment_id": source_summary.get("experiment_id"),
        "source_count": len({row["source_group"] for row in rows}),
        "detail_rows": len(rows),
        "runtime_feature_count": len(MODEL_FEATURE_NAMES),
        "runtime_feature_names": list(MODEL_FEATURE_NAMES),
        "forbidden_predictors": [
            "canonical_image",
            "visibility_proxy",
            "visibility_status",
            "human_side_label",
            "environment",
            "alignment_ssim",
            "input_plate_quality_score",
            "formal_test",
        ],
        "fold_count": args.fold_count,
        "fold_audit": audit,
        "frozen_model": final_model,
        "low_visibility_proxy_threshold": VISIBILITY_PROXY_THRESHOLD,
        "low_visibility_proxy_side_count": low["side_count"],
        "retained_visibility_proxy_side_count": retained["side_count"],
        "low_visibility_proxy_source_count": low["source_count"],
        "low_visibility_vehicle_row_count": len(low_visibility_vehicle_rows),
        "low_visibility_vehicle_miss_auto_absent_before": sum(
            not int(row["oof_vehicle_present"]) and row["oof_presence_decision"] == "ABSENT"
            for row in low_visibility_vehicle_rows
        ),
        "low_visibility_vehicle_miss_auto_absent_after": sum(
            not int(row["oof_guard_vehicle_present"])
            and row["oof_guard_presence_decision"] == "ABSENT"
            for row in low_visibility_vehicle_rows
        ),
        "present_decisions_changed_count": len(present_decisions_changed),
        "decision_transition_counts": dict(
            sorted(
                Counter(
                    f"{row['oof_presence_decision']}->{row['oof_guard_presence_decision']}"
                    for row in rows
                ).items()
            )
        ),
        "gate": gate,
        "error_count": len(errors),
        "errors": errors,
        "formal_test_accessed_in_this_run": False,
        "formal_test_previously_opened": bool(source_summary.get("formal_test_previously_opened")),
        "human_degraded_visibility_gt_evaluated": False,
        "visibility_proxy_supervision_only": True,
        "ocr_image_or_processing_changed": False,
        "selector_ocr_non_regression_evaluated": False,
        "end_to_end_selector_latency_evaluated": False,
        "real_ir_evaluated": False,
        "independent_holdout_evaluated": False,
        "operational_candidate": False,
        "performance_claim": False,
    }
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _write_csv(output_dir / "manifest.csv", _manifest_rows(rows))
    _write_csv(output_dir / "details.csv", rows)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    _write_csv(output_dir / "observability_summary.csv", observability_rows)
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_dir / "REPORT.md").write_text(
        _report(summary, lookup, observability), encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spatial-details", required=True)
    parser.add_argument("--spatial-summary", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--fold-count", type=int, default=FOLD_COUNT)
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    return 0 if summary["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Evaluate a spatial icon descriptor on low-quality rectified plate sides."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .icon_template_bank import side_region
from .low_quality_hard_negative_side_model import (
    _positive_geometric,
    build_hard_negative_rows,
    build_pilot_rows,
)
from .low_quality_presence_rectification import (
    LOGISTIC_GLOBAL_FEATURES,
    LOGISTIC_SIDE_FEATURES,
    _group_fold,
)
from .low_quality_vehicle_presence_calibration import (
    COHORTS,
    _read_unicode,
    _manifest_rows,
    _write_csv,
    apply_policy,
    choose_vehicle_policy,
    summarize_rows,
)
from .roi_annotation import read_csv


EXPERIMENT_ID = "low-quality-spatial-icon-descriptor-013"
SPATIAL_CELL_COUNT = 16
HOG_BINS = 9
SPATIAL_FEATURE_COUNT = SPATIAL_CELL_COUNT * (HOG_BINS + 4)
SPATIAL_FEATURE_NAMES = tuple(f"spatial_{index:03d}" for index in range(SPATIAL_FEATURE_COUNT))
MODEL_FEATURE_NAMES = (*SPATIAL_FEATURE_NAMES, *LOGISTIC_SIDE_FEATURES, *LOGISTIC_GLOBAL_FEATURES)
HOG_SLICE = slice(0, SPATIAL_CELL_COUNT * HOG_BINS)
INTENSITY_SLICE = slice(HOG_SLICE.stop, HOG_SLICE.stop + SPATIAL_CELL_COUNT)
BLUE_SLICE = slice(INTENSITY_SLICE.stop, INTENSITY_SLICE.stop + SPATIAL_CELL_COUNT)
SATURATION_SLICE = slice(BLUE_SLICE.stop, BLUE_SLICE.stop + SPATIAL_CELL_COUNT)
EDGE_SLICE = slice(SATURATION_SLICE.stop, SATURATION_SLICE.stop + SPATIAL_CELL_COUNT)
VISIBILITY_PROXY_THRESHOLD = 0.50


def spatial_descriptor(image: np.ndarray, side: str) -> np.ndarray:
    """Coarse fixed-position HOG, intensity, blue, saturation, and edge descriptor."""
    region = side_region(image, side)
    if region.size == 0:
        return np.zeros(SPATIAL_FEATURE_COUNT, dtype=np.float64)
    resized = cv2.resize(region, (64, 64), interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(resized, cv2.COLOR_BGR2HSV)
    hue, saturation, value = cv2.split(hsv)
    gray = cv2.cvtColor(resized, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=1.6, tileGridSize=(4, 4)).apply(gray)
    gx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)
    magnitude, angle = cv2.cartToPolar(gx, gy, angleInDegrees=True)
    unsigned_angle = np.mod(angle, 180.0)
    orientation_bin = np.minimum((unsigned_angle / (180.0 / HOG_BINS)).astype(np.int32), HOG_BINS - 1)
    edges = cv2.Canny(gray, 24, 96)
    blue = (hue >= 85) & (hue <= 125) & (saturation >= 25) & (value >= 20)
    hog_values: list[float] = []
    intensity_values: list[float] = []
    blue_values: list[float] = []
    saturation_values: list[float] = []
    edge_values: list[float] = []
    for cell_y in range(4):
        for cell_x in range(4):
            y1, y2 = cell_y * 16, (cell_y + 1) * 16
            x1, x2 = cell_x * 16, (cell_x + 1) * 16
            bins = orientation_bin[y1:y2, x1:x2].reshape(-1)
            weights = magnitude[y1:y2, x1:x2].reshape(-1)
            histogram = np.bincount(bins, weights=weights, minlength=HOG_BINS).astype(np.float64)
            histogram /= max(1e-8, float(np.linalg.norm(histogram)))
            hog_values.extend(histogram.tolist())
            intensity_values.append(float(np.mean(gray[y1:y2, x1:x2])) / 255.0)
            blue_values.append(float(np.mean(blue[y1:y2, x1:x2])))
            saturation_values.append(float(np.mean(saturation[y1:y2, x1:x2])) / 255.0)
            edge_values.append(float(np.mean(edges[y1:y2, x1:x2] > 0)))
    descriptor = np.asarray(
        [*hog_values, *intensity_values, *blue_values, *saturation_values, *edge_values],
        dtype=np.float64,
    )
    if descriptor.shape != (SPATIAL_FEATURE_COUNT,):
        raise ValueError(f"spatial_descriptor_shape:{descriptor.shape}")
    return descriptor


def extract_spatial_features(image: np.ndarray, side: str) -> dict[str, float]:
    descriptor = spatial_descriptor(image, side)
    return {
        f"{side}_{name}": float(value)
        # spatial_descriptor()가 길이를 검증하므로 Python 3.9 호환 zip을 사용한다.
        for name, value in zip(SPATIAL_FEATURE_NAMES, descriptor)
    }


def _cosine_similarity(left: np.ndarray, right: np.ndarray) -> float:
    denominator = float(np.linalg.norm(left) * np.linalg.norm(right))
    if denominator <= 1e-12:
        return 1.0 if float(np.linalg.norm(left - right)) <= 1e-12 else 0.0
    return float(np.clip(np.dot(left, right) / denominator, 0.0, 1.0))


def _retention(current: float, reference: float) -> float:
    if reference <= 1e-12:
        return 1.0 if current <= 1e-12 else float(current / 1e-12)
    return float(max(0.0, current / reference))


def visibility_metrics(current: np.ndarray, reference: np.ndarray) -> dict[str, float]:
    """Paired canonical/degraded visibility proxy; this is not human visibility GT."""
    if current.shape != (SPATIAL_FEATURE_COUNT,) or reference.shape != (SPATIAL_FEATURE_COUNT,):
        raise ValueError(f"visibility_descriptor_shape:{current.shape}:{reference.shape}")
    hog_cosine = _cosine_similarity(current[HOG_SLICE], reference[HOG_SLICE])
    contrast_retention = _retention(
        float(np.std(current[INTENSITY_SLICE])),
        float(np.std(reference[INTENSITY_SLICE])),
    )
    edge_retention = _retention(
        float(np.mean(current[EDGE_SLICE])),
        float(np.mean(reference[EDGE_SLICE])),
    )
    blue_retention = _retention(
        float(np.mean(current[BLUE_SLICE])),
        float(np.mean(reference[BLUE_SLICE])),
    )
    saturation_retention = _retention(
        float(np.mean(current[SATURATION_SLICE])),
        float(np.mean(reference[SATURATION_SLICE])),
    )
    proxy = (
        0.50 * hog_cosine
        + 0.25 * float(np.clip(edge_retention, 0.0, 1.0))
        + 0.25 * float(np.clip(contrast_retention, 0.0, 1.0))
    )
    return {
        "hog_cosine": hog_cosine,
        "edge_retention": edge_retention,
        "contrast_retention": contrast_retention,
        "blue_retention": blue_retention,
        "saturation_retention": saturation_retention,
        "visibility_proxy": float(np.clip(proxy, 0.0, 1.0)),
    }


def _row_spatial_descriptor(row: dict, side: str) -> np.ndarray:
    return np.asarray(
        [float(row[f"{side}_{name}"]) for name in SPATIAL_FEATURE_NAMES],
        dtype=np.float64,
    )


def add_visibility_metrics(rows: list[dict], icon_manifest_path: Path) -> list[dict]:
    """Attach paired visibility proxies only where human physical-presence GT exists."""
    canonical_paths = {
        source["annotation_id"]: source["canonical_path"]
        for source in read_csv(icon_manifest_path)
        if source.get("annotation_id") and source.get("canonical_path")
    }
    canonical_cache: dict[str, np.ndarray | None] = {}
    descriptor_cache: dict[tuple[str, str], np.ndarray] = {}
    errors: list[dict] = []
    metric_names = (
        "hog_cosine",
        "edge_retention",
        "contrast_retention",
        "blue_retention",
        "saturation_retention",
        "visibility_proxy",
    )
    for row in rows:
        row["vehicle_visibility_proxy"] = ""
        row["vehicle_visibility_status"] = "not_applicable"
        present_side_proxies: list[float] = []
        for side in ("left", "right"):
            for name in metric_names:
                row[f"{side}_{name}"] = ""
            label = str(row.get(f"{side}_icon_label", ""))
            if row.get("source_set") != "pilot_development" or label != "present":
                row[f"{side}_visibility_status"] = "not_applicable_absent_or_unlabeled"
                continue
            if not int(row["geometric_success"]):
                row[f"{side}_visibility_status"] = "unmeasurable_rectification_failed"
                row["vehicle_visibility_status"] = "unmeasurable_rectification_failed"
                continue
            annotation_id = str(row["source_id"])
            canonical_path = canonical_paths.get(annotation_id, "")
            if not canonical_path:
                row[f"{side}_visibility_status"] = "unmeasurable_canonical_missing"
                errors.append(
                    {
                        "source_id": annotation_id,
                        "environment": row.get("environment", ""),
                        "reason": "visibility_canonical_path_missing",
                    }
                )
                continue
            if canonical_path not in canonical_cache:
                canonical_cache[canonical_path] = _read_unicode(Path(canonical_path))
            canonical = canonical_cache[canonical_path]
            if canonical is None:
                row[f"{side}_visibility_status"] = "unmeasurable_canonical_unreadable"
                errors.append(
                    {
                        "source_id": annotation_id,
                        "environment": row.get("environment", ""),
                        "reason": "visibility_canonical_unreadable",
                    }
                )
                continue
            cache_key = (annotation_id, side)
            if cache_key not in descriptor_cache:
                descriptor_cache[cache_key] = spatial_descriptor(canonical, side)
            measured = visibility_metrics(_row_spatial_descriptor(row, side), descriptor_cache[cache_key])
            for name, value in measured.items():
                row[f"{side}_{name}"] = value
            proxy = measured["visibility_proxy"]
            present_side_proxies.append(proxy)
            row[f"{side}_visibility_status"] = (
                "low_visibility_proxy"
                if proxy < VISIBILITY_PROXY_THRESHOLD
                else "retained_visibility_proxy"
            )
        if present_side_proxies:
            vehicle_proxy = max(present_side_proxies)
            row["vehicle_visibility_proxy"] = vehicle_proxy
            row["vehicle_visibility_status"] = (
                "low_visibility_proxy"
                if vehicle_proxy < VISIBILITY_PROXY_THRESHOLD
                else "retained_visibility_proxy"
            )
    return errors


def _feature_vector(row: dict, side: str) -> np.ndarray:
    values = [float(row[f"{side}_{name}"]) for name in SPATIAL_FEATURE_NAMES]
    values.extend(float(row[f"{side}_feature_{name}"]) for name in LOGISTIC_SIDE_FEATURES)
    values.extend(float(row[name]) for name in LOGISTIC_GLOBAL_FEATURES)
    return np.asarray(values, dtype=np.float64)


def _sigmoid(values: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(values, -30.0, 30.0)))


def fit_spatial_model(rows: list[dict], side: str) -> dict:
    eligible = [row for row in rows if row[f"{side}_icon_label"] in {"present", "absent"}]
    x = np.vstack([_feature_vector(row, side) for row in eligible])
    y = np.asarray(
        [1.0 if row[f"{side}_icon_label"] == "present" else 0.0 for row in eligible],
        dtype=np.float64,
    )
    positive_count = int(np.sum(y == 1.0))
    negative_count = int(np.sum(y == 0.0))
    if not positive_count or not negative_count:
        raise ValueError(f"spatial_model_class_missing:{side}:{positive_count}:{negative_count}")
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


def predict_spatial_model(model: dict, row: dict, side: str) -> float:
    normalized = (_feature_vector(row, side) - model["mean"]) / model["scale"]
    design = np.append(normalized, 1.0)
    return float(_sigmoid(np.asarray([design @ model["coefficients"]]))[0])


def _serialize_model(model: dict) -> dict:
    return {
        "feature_names": list(MODEL_FEATURE_NAMES),
        "mean": model["mean"].tolist(),
        "scale": model["scale"].tolist(),
        "coefficients": model["coefficients"].tolist(),
        "positive_count": model["positive_count"],
        "negative_count": model["negative_count"],
        "learning_rate": model["learning_rate"],
        "regularization": model["regularization"],
        "iterations": model["iterations"],
    }


def _fit_models(rows: list[dict]) -> dict[str, dict]:
    return {side: fit_spatial_model(rows, side) for side in ("left", "right")}


def _set_scores(rows: list[dict], models: dict[str, dict], prefix: str, set_effective: bool) -> None:
    for row in rows:
        for side in ("left", "right"):
            probability = predict_spatial_model(models[side], row, side)
            effective = probability if int(row["geometric_success"]) else -1.0
            row[f"{prefix}_{side}_icon_probability"] = probability
            row[f"{prefix}_{side}_icon_score"] = effective
            if set_effective:
                row[f"{side}_icon_logistic_score"] = probability
                row[f"{side}_icon_score"] = effective


def apply_nested_group_oof(
    rows: list[dict], fold_count: int, fpr_cap: float
) -> tuple[list[dict], dict, dict]:
    for row in rows:
        row["spatial_model_fold"] = _group_fold(row["source_group"], fold_count)
    audit: list[dict] = []
    for fold in range(fold_count):
        fit_rows = [row for row in rows if int(row["spatial_model_fold"]) != fold]
        holdout_rows = [row for row in rows if int(row["spatial_model_fold"]) == fold]
        fit_groups = {row["source_group"] for row in fit_rows}
        holdout_groups = {row["source_group"] for row in holdout_rows}
        overlap = sorted(fit_groups & holdout_groups)
        models = _fit_models(fit_rows)
        _set_scores(fit_rows, models, "fold_fit", set_effective=True)
        policies = {
            cohort: choose_vehicle_policy(fit_rows, cohort, fpr_cap)
            for cohort in ("quality_below_0_25", "quality_at_or_above_0_25")
        }
        _set_scores(holdout_rows, models, "oof", set_effective=True)
        for row in holdout_rows:
            cohort = "quality_below_0_25" if int(row["quality_below_threshold"]) else "quality_at_or_above_0_25"
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
    final_models = _fit_models(rows)
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


def assign_review_reasons(rows: list[dict]) -> None:
    for row in rows:
        detected = bool(int(row["oof_vehicle_present"]))
        decision = str(row["oof_presence_decision"])
        label = str(row.get("vehicle_presence_label", "excluded"))
        if detected:
            reason = {
                "present": "detected_present",
                "absent": "false_positive_present",
            }.get(label, "detected_unlabeled")
        elif not int(row["geometric_success"]):
            reason = "review_rectification_failed"
        elif int(row["quality_below_threshold"]) and label == "present":
            reason = "review_low_quality_icon_not_confirmed"
        elif int(row["quality_below_threshold"]) and decision == "REVIEW":
            reason = "review_low_quality_absence_uncertain"
        elif decision == "REVIEW":
            reason = "review_other_uncertainty"
        elif label == "present":
            reason = "miss_score_below_threshold"
        elif label == "absent":
            reason = "confirmed_absent"
        else:
            reason = "excluded_unlabeled"
        row["oof_decision_reason"] = reason


def _mean_or_blank(records: list[dict], field: str) -> float | str:
    values = [float(record[field]) for record in records if record.get(field, "") != ""]
    return float(np.mean(values)) if values else ""


def summarize_visibility(rows: list[dict]) -> list[dict]:
    side_records: list[dict] = []
    for row in rows:
        if row.get("source_set") != "pilot_development":
            continue
        for side in ("left", "right"):
            if row.get(f"{side}_icon_label") != "present":
                continue
            side_records.append(
                {
                    "source_group": row["source_group"],
                    "environment": row["environment"],
                    "quality_below_threshold": int(row["quality_below_threshold"]),
                    "geometric_success": int(row["geometric_success"]),
                    "visibility_status": row[f"{side}_visibility_status"],
                    "hog_cosine": row[f"{side}_hog_cosine"],
                    "edge_retention": row[f"{side}_edge_retention"],
                    "contrast_retention": row[f"{side}_contrast_retention"],
                    "blue_retention": row[f"{side}_blue_retention"],
                    "saturation_retention": row[f"{side}_saturation_retention"],
                    "visibility_proxy": row[f"{side}_visibility_proxy"],
                    "oof_side_probability": row[f"oof_{side}_icon_probability"],
                    "oof_vehicle_present": int(row["oof_vehicle_present"]),
                    "oof_review": int(row["oof_presence_decision"] == "REVIEW"),
                }
            )
    scopes: list[tuple[str, str, list[dict]]] = [
        ("cohort", "all_present_sides", side_records),
        (
            "cohort",
            "quality_below_0_25",
            [record for record in side_records if record["quality_below_threshold"]],
        ),
        (
            "cohort",
            "quality_at_or_above_0_25",
            [record for record in side_records if not record["quality_below_threshold"]],
        ),
    ]
    scopes.extend(
        (
            "environment",
            environment,
            [record for record in side_records if record["environment"] == environment],
        )
        for environment in sorted({record["environment"] for record in side_records})
    )
    scopes.extend(
        (
            "visibility_status",
            status,
            [record for record in side_records if record["visibility_status"] == status],
        )
        for status in (
            "low_visibility_proxy",
            "retained_visibility_proxy",
            "unmeasurable_rectification_failed",
        )
    )
    result: list[dict] = []
    for scope_type, scope_value, selected in scopes:
        measurable = [record for record in selected if record["visibility_proxy"] != ""]
        low_visibility = [
            record for record in measurable if record["visibility_status"] == "low_visibility_proxy"
        ]
        result.append(
            {
                "scope_type": scope_type,
                "scope_value": scope_value,
                "side_count": len(selected),
                "unique_source_count": len({record["source_group"] for record in selected}),
                "measurable_side_count": len(measurable),
                "visibility_proxy_coverage": len(measurable) / len(selected) if selected else 0.0,
                "low_visibility_proxy_count": len(low_visibility),
                "low_visibility_proxy_rate": len(low_visibility) / len(measurable) if measurable else 0.0,
                "mean_hog_cosine": _mean_or_blank(measurable, "hog_cosine"),
                "mean_edge_retention": _mean_or_blank(measurable, "edge_retention"),
                "mean_contrast_retention": _mean_or_blank(measurable, "contrast_retention"),
                "mean_blue_retention": _mean_or_blank(measurable, "blue_retention"),
                "mean_saturation_retention": _mean_or_blank(measurable, "saturation_retention"),
                "mean_visibility_proxy": _mean_or_blank(measurable, "visibility_proxy"),
                "mean_oof_side_probability": _mean_or_blank(selected, "oof_side_probability"),
                "oof_vehicle_present_rate": (
                    sum(record["oof_vehicle_present"] for record in selected) / len(selected)
                    if selected
                    else 0.0
                ),
                "oof_review_rate": (
                    sum(record["oof_review"] for record in selected) / len(selected) if selected else 0.0
                ),
                "unmeasurable_rectification_failure_count": sum(
                    record["visibility_status"] == "unmeasurable_rectification_failed"
                    for record in selected
                ),
                "human_degraded_visibility_gt": False,
                "measurement_kind": "paired_canonical_degraded_proxy",
            }
        )
    return result


def _format_optional_percent(value: float | str) -> str:
    return "N/A" if value == "" else f"{float(value):.2%}"


def _report(
    summary: dict,
    lookup: dict[tuple[str, str], dict],
    visibility_lookup: dict[tuple[str, str], dict],
) -> str:
    combined = lookup[("all", "synthetic_low_quality")]
    low = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    visibility_all = visibility_lookup[("cohort", "all_present_sides")]
    visibility_low_quality = visibility_lookup[("cohort", "quality_below_0_25")]
    visibility_low = visibility_lookup[("visibility_status", "low_visibility_proxy")]
    visibility_retained = visibility_lookup[("visibility_status", "retained_visibility_proxy")]
    gates = "\n".join(
        f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()
    )
    reasons = "\n".join(
        f"- `{name}`: {count}" for name, count in summary["oof_decision_reason_counts"].items()
    )
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- Formal Test accessed in this run: `false`
- Sources/rows/errors: {summary['source_count']} / {summary['detail_rows']} / {summary['error_count']}
- Side feature count: {summary['side_feature_count']}

## Nested source-group OOF

| Cohort | Recall | FPR | Prescreen Recall | REVIEW | Geometric success |
|---|---:|---:|---:|---:|---:|
| Combined | {combined['oof_vehicle_recall']:.2%} | {combined['oof_vehicle_fpr']:.2%} | {combined['oof_prescreen_recall']:.2%} | {combined['oof_review_rate']:.2%} | {combined['geometric_rectification_success']:.2%} |
| Quality < 0.25 | {low['oof_vehicle_recall']:.2%} | {low['oof_vehicle_fpr']:.2%} | {low['oof_prescreen_recall']:.2%} | {low['oof_review_rate']:.2%} | {low['geometric_rectification_success']:.2%} |
| Corrected pilot | {pilot['oof_vehicle_recall']:.2%} | {pilot['oof_vehicle_fpr']:.2%} | {pilot['oof_prescreen_recall']:.2%} | {pilot['oof_review_rate']:.2%} | {pilot['geometric_rectification_success']:.2%} |
| OCR hard negative | N/A | {hard['oof_vehicle_fpr']:.2%} | N/A | {hard['oof_review_rate']:.2%} | {hard['geometric_rectification_success']:.2%} |

## Paired icon-visibility proxy

이 표의 정답은 원본에서 사람이 표시한 **아이콘 물리적 존재 여부**다. 저화질 결과에서 사람이 실제로 볼 수 있는지에 대한 정답은 없으므로, 정상 canonical 대비 HOG cosine 50% + edge 보존율 25% + 명암 대비 보존율 25%의 프록시만 측정한다. 프록시 0.50 미만을 `low_visibility_proxy`로 분리하며, 평면화 실패는 계산하지 않고 `unmeasurable_rectification_failed`로 센다.

| Scope | Present sides | Measured | Low visibility | Mean proxy | Detection | REVIEW | Rectification-unmeasurable |
|---|---:|---:|---:|---:|---:|---:|---:|
| All physically present sides | {visibility_all['side_count']} | {visibility_all['measurable_side_count']} | {visibility_all['low_visibility_proxy_rate']:.2%} | {_format_optional_percent(visibility_all['mean_visibility_proxy'])} | {visibility_all['oof_vehicle_present_rate']:.2%} | {visibility_all['oof_review_rate']:.2%} | {visibility_all['unmeasurable_rectification_failure_count']} |
| Quality < 0.25 | {visibility_low_quality['side_count']} | {visibility_low_quality['measurable_side_count']} | {visibility_low_quality['low_visibility_proxy_rate']:.2%} | {_format_optional_percent(visibility_low_quality['mean_visibility_proxy'])} | {visibility_low_quality['oof_vehicle_present_rate']:.2%} | {visibility_low_quality['oof_review_rate']:.2%} | {visibility_low_quality['unmeasurable_rectification_failure_count']} |
| Low-visibility proxy | {visibility_low['side_count']} | {visibility_low['measurable_side_count']} | {visibility_low['low_visibility_proxy_rate']:.2%} | {_format_optional_percent(visibility_low['mean_visibility_proxy'])} | {visibility_low['oof_vehicle_present_rate']:.2%} | {visibility_low['oof_review_rate']:.2%} | {visibility_low['unmeasurable_rectification_failure_count']} |
| Retained-visibility proxy | {visibility_retained['side_count']} | {visibility_retained['measurable_side_count']} | {visibility_retained['low_visibility_proxy_rate']:.2%} | {_format_optional_percent(visibility_retained['mean_visibility_proxy'])} | {visibility_retained['oof_vehicle_present_rate']:.2%} | {visibility_retained['oof_review_rate']:.2%} | {visibility_retained['unmeasurable_rectification_failure_count']} |

### Post-run visibility safety observation

- Low-visibility proxy sides below the existing plate-quality threshold: {summary['visibility_safety_observation']['low_visibility_proxy_quality_below_side_count']} / {summary['visibility_safety_observation']['low_visibility_proxy_side_count']}
- Rows containing those sides: {summary['visibility_safety_observation']['row_with_low_visibility_side_count']}
- Such rows routed `PRESENT` / `REVIEW` / `ABSENT`: {summary['visibility_safety_observation']['row_with_low_visibility_side_present_count']} / {summary['visibility_safety_observation']['row_with_low_visibility_side_review_count']} / {summary['visibility_safety_observation']['row_with_low_visibility_side_absent_count']}
- Entire-vehicle low-visibility proxy rows missed and auto-`ABSENT`: {summary['visibility_safety_observation']['low_visibility_vehicle_miss_auto_absent_count']} / {summary['visibility_safety_observation']['low_visibility_vehicle_row_count']}

이는 사전 등록 게이트가 끝난 뒤 확인한 **안전 진단**이며 모델 선택에 소급 사용하지 않았다. 현재 plate 전체 품질 임계값은 관찰된 icon-specific 가시성 저하를 포괄하지 못하므로, 이 결과의 development-candidate 상태는 운영 승인으로 해석하면 안 된다.

### OOF decision reasons

{reasons}

## Gate

{gates}

공간 descriptor는 고정 좌·우 ROI를 사용하지만 OCR hard-negative absent는 dataset class 기반 추정 라벨이다. `REVIEW`는 미검출을 검출 성공으로 바꾸지 않으며, 평면화 실패와 저화질 아이콘 미확인을 자동 확정하지 않기 위한 안전 라우팅이다. 통과하더라도 신규 사람 가시성 GT RGB/IR, selector 적용 후 OCR 비회귀, end-to-end/Pi latency와 독립 holdout이 필요하다.
"""


def run(args: argparse.Namespace) -> dict:
    selector_summary = json.loads(Path(args.selector_summary).resolve().read_text(encoding="utf-8-sig"))
    ocr_guard = json.loads(Path(args.ocr_guard_summary).resolve().read_text(encoding="utf-8-sig"))
    if selector_summary.get("formal_test_accessed_in_this_run"):
        raise ValueError("formal_test_accessed_in_spatial_selector_summary")
    pilot_rows, pilot_errors, context = build_pilot_rows(
        Path(args.selector_details).resolve(),
        Path(args.split_manifest).resolve(),
        Path(args.icon_manifest).resolve(),
        extra_feature_extractor=extract_spatial_features,
    )
    hard_rows, hard_errors = build_hard_negative_rows(
        Path(args.ocr_hard_negative_manifest).resolve(),
        context["template_cache"],
        extra_feature_extractor=extract_spatial_features,
    )
    rows = [*pilot_rows, *hard_rows]
    errors = [*pilot_errors, *hard_errors]
    errors.extend(add_visibility_metrics(rows, Path(args.icon_manifest).resolve()))
    pilot_paths = {str(Path(row["source_path"]).resolve()).casefold() for row in pilot_rows}
    hard_paths = {str(Path(row["source_path"]).resolve()).casefold() for row in hard_rows}
    source_path_overlap = sorted(pilot_paths & hard_paths)
    audit, final_models, final_policies = apply_nested_group_oof(rows, args.fold_count, args.fit_fpr_cap)
    assign_review_reasons(rows)
    metrics = [
        summarize_rows(rows, source_set, cohort)
        for source_set in ("all", "pilot_development", "ocr_hard_negative")
        for cohort in COHORTS
    ]
    visibility_summaries = summarize_visibility(rows)
    visibility_lookup = {
        (row["scope_type"], row["scope_value"]): row for row in visibility_summaries
    }
    lookup = {(row["source_set"], row["cohort"]): row for row in metrics}
    combined = lookup[("all", "synthetic_low_quality")]
    low = lookup[("all", "quality_below_0_25")]
    pilot = lookup[("pilot_development", "synthetic_low_quality")]
    hard = lookup[("ocr_hard_negative", "synthetic_low_quality")]
    positive_geometric = _positive_geometric(pilot_rows)
    low_positive_geometric = _positive_geometric(pilot_rows, below_only=True)
    geometry_success_present_sides = [
        (row, side)
        for row in pilot_rows
        if int(row["geometric_success"])
        for side in ("left", "right")
        if row[f"{side}_icon_label"] == "present"
    ]
    quality_below_geometry_success_present_sides = [
        (row, side)
        for row, side in geometry_success_present_sides
        if int(row["quality_below_threshold"])
    ]
    rectification_failures = [row for row in rows if not int(row["geometric_success"])]
    low_quality_present_misses = [
        row
        for row in pilot_rows
        if row["vehicle_presence_label"] == "present"
        and int(row["quality_below_threshold"])
        and not int(row["oof_vehicle_present"])
    ]
    rows_with_low_visibility_side = [
        row
        for row in pilot_rows
        if any(row[f"{side}_visibility_status"] == "low_visibility_proxy" for side in ("left", "right"))
    ]
    low_visibility_sides = [
        (row, side)
        for row in pilot_rows
        for side in ("left", "right")
        if row[f"{side}_visibility_status"] == "low_visibility_proxy"
    ]
    low_visibility_vehicle_rows = [
        row for row in pilot_rows if row["vehicle_visibility_status"] == "low_visibility_proxy"
    ]
    visibility_safety_observation = {
        "posthoc_after_preregistered_gate_evaluation": True,
        "low_visibility_proxy_side_count": len(low_visibility_sides),
        "low_visibility_proxy_quality_below_side_count": sum(
            int(row["quality_below_threshold"]) for row, _ in low_visibility_sides
        ),
        "row_with_low_visibility_side_count": len(rows_with_low_visibility_side),
        "row_with_low_visibility_side_present_count": sum(
            row["oof_presence_decision"] == "PRESENT" for row in rows_with_low_visibility_side
        ),
        "row_with_low_visibility_side_review_count": sum(
            row["oof_presence_decision"] == "REVIEW" for row in rows_with_low_visibility_side
        ),
        "row_with_low_visibility_side_absent_count": sum(
            row["oof_presence_decision"] == "ABSENT" for row in rows_with_low_visibility_side
        ),
        "low_visibility_vehicle_row_count": len(low_visibility_vehicle_rows),
        "low_visibility_vehicle_miss_auto_absent_count": sum(
            not int(row["oof_vehicle_present"]) and row["oof_presence_decision"] == "ABSENT"
            for row in low_visibility_vehicle_rows
        ),
        "existing_plate_quality_threshold_covers_observed_low_visibility_proxy": all(
            int(row["quality_below_threshold"]) for row, _ in low_visibility_sides
        )
        if low_visibility_sides
        else False,
        "operational_visibility_safety_pass": False,
    }
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
        "feature_extraction_mean_le_200ms": combined["mean_processing_ms"] <= 200.0,
        "feature_extraction_p95_le_350ms": combined["p95_processing_ms"] <= 350.0,
        "source_path_overlap_zero": not source_path_overlap,
        "group_overlap_zero": all(not item["group_overlap"] for item in audit),
        "formal_test_rows_zero": not any(row["original_split"] == "test" for row in rows),
        "template_test_overlap_zero": not context["template_test_overlap"],
        "processing_errors_zero": not errors,
        "rectification_failures_all_review": bool(rectification_failures)
        and all(row["oof_presence_decision"] == "REVIEW" for row in rectification_failures),
        "quality_below_present_misses_all_review": bool(low_quality_present_misses)
        and all(row["oof_presence_decision"] == "REVIEW" for row in low_quality_present_misses),
        "geometry_success_present_side_visibility_proxy_coverage_one": bool(
            geometry_success_present_sides
        )
        and all(row[f"{side}_visibility_proxy"] != "" for row, side in geometry_success_present_sides),
        "quality_below_present_side_visibility_proxy_coverage_one": bool(
            quality_below_geometry_success_present_sides
        )
        and all(
            row[f"{side}_visibility_proxy"] != ""
            for row, side in quality_below_geometry_success_present_sides
        ),
    }
    status = "spatial_descriptor_development_candidate" if all(gate.values()) else "rejected"
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "source_count": len({row["source_group"] for row in rows}),
        "pilot_source_count": len({row["source_group"] for row in pilot_rows}),
        "hard_negative_source_count": len({row["source_group"] for row in hard_rows}),
        "detail_rows": len(rows),
        "spatial_feature_count": SPATIAL_FEATURE_COUNT,
        "side_feature_count": len(MODEL_FEATURE_NAMES),
        "spatial_grid": "4x4",
        "hog_bins": HOG_BINS,
        "fit_fpr_cap": args.fit_fpr_cap,
        "gate_fpr": args.gate_fpr,
        "fold_audit": audit,
        "frozen_side_models": final_models,
        "frozen_vehicle_policies": final_policies,
        "positive_geometric_success": positive_geometric,
        "quality_below_positive_geometric_success": low_positive_geometric,
        "source_path_overlap": source_path_overlap,
        "template_test_overlap": context["template_test_overlap"],
        "physical_presence_gt": "human_side_icon_label_on_canonical",
        "human_degraded_visibility_gt_evaluated": False,
        "visibility_proxy_only": True,
        "visibility_proxy_threshold": VISIBILITY_PROXY_THRESHOLD,
        "visibility_proxy_definition": (
            "0.50*canonical_degraded_hog_cosine+0.25*clipped_edge_retention+"
            "0.25*clipped_intensity_grid_contrast_retention"
        ),
        "visibility_present_side_count": visibility_lookup[("cohort", "all_present_sides")][
            "side_count"
        ],
        "visibility_measurable_side_count": visibility_lookup[("cohort", "all_present_sides")][
            "measurable_side_count"
        ],
        "low_visibility_proxy_side_count": visibility_lookup[
            ("cohort", "all_present_sides")
        ]["low_visibility_proxy_count"],
        "rectification_unmeasurable_present_side_count": visibility_lookup[
            ("cohort", "all_present_sides")
        ]["unmeasurable_rectification_failure_count"],
        "oof_decision_reason_counts": dict(
            sorted(Counter(str(row["oof_decision_reason"]) for row in rows).items())
        ),
        "visibility_safety_observation": visibility_safety_observation,
        "gate": gate,
        "error_count": len(errors),
        "errors": errors,
        "formal_test_accessed_in_this_run": False,
        "formal_test_previously_opened": bool(args.formal_test_previously_opened),
        "formal_test_reuse_for_tuning_prohibited": True,
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
    _write_csv(output_dir / "visibility_summary.csv", visibility_summaries)
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "REPORT.md").write_text(
        _report(summary, lookup, visibility_lookup), encoding="utf-8"
    )
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

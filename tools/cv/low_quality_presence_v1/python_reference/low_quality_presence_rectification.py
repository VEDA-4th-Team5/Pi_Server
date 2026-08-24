"""Train/Validation-only evaluation for low-quality plate rectification and icon presence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .canonical_robustness_pilot import degrade
from .icon_detector_common import detect_icon, detect_icon_contrastive, detect_icon_robust
from .icon_template_bank import build_absent_templates, build_templates, median_bbox
from .plate_rectifier import GEOMETRIC_METHODS, order_quad, rectify_plate, rectify_plate_legacy
from .rectification_quality import rectification_quality_score
from .roi_annotation import parse_box, read_csv
from .roi_first_classifier import select_perspective_preferred_candidate
from .top3_localization import intersection_over_union, ranked_candidates, read_image


EXPERIMENT_ID = "low-quality-presence-rectification-001"
TARGET_SIZE = (440, 100)
QUALITY_THRESHOLD = 0.25
IOU_THRESHOLD = 0.50
ICON_TRAIN_FPR_CAP = 0.35
ICON_FALLBACK_RECT_QUALITY_MIN = 0.60
ENVIRONMENTS = ("normal", "low_light", "gaussian_blur_07", "motion_blur_09", "downscale_jpeg")
LOW_QUALITY_ENVIRONMENTS = frozenset(ENVIRONMENTS[1:])
VARIANTS = ("baseline", "quality_aware")
LOGISTIC_SIDE_FEATURES = (
    "template_edge_margin",
    "template_robust_margin",
    "positive_edge_similarity",
    "negative_edge_similarity",
    "positive_robust_similarity",
    "negative_robust_similarity",
    "blue_ratio",
    "mean_saturation",
    "mean_value",
    "gray_contrast",
    "edge_density",
    "laplacian_log",
)
LOGISTIC_GLOBAL_FEATURES = (
    "rectification_geometry_confidence",
    "rectification_quality",
    "quality_aware_fallback_used",
)


def apply_environment(image: np.ndarray, environment: str) -> np.ndarray:
    if environment == "downscale_jpeg":
        return degrade(degrade(image, "downscale_050"), "jpeg_q40")
    return degrade(image, environment)


def _warp_ground_truth(image: np.ndarray, quad: list[int]) -> np.ndarray:
    source = order_quad(np.asarray(quad, dtype=np.float32).reshape(4, 2))
    destination = np.asarray(
        [[0, 0], [TARGET_SIZE[0] - 1, 0], [TARGET_SIZE[0] - 1, TARGET_SIZE[1] - 1], [0, TARGET_SIZE[1] - 1]],
        dtype=np.float32,
    )
    transform = cv2.getPerspectiveTransform(source, destination)
    return cv2.warpPerspective(
        image,
        transform,
        TARGET_SIZE,
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_REPLICATE,
    )


def alignment_ssim(left: np.ndarray, right: np.ndarray) -> float:
    """Full-reference structural alignment score for two same-size rectified images."""
    left_gray = cv2.cvtColor(left, cv2.COLOR_BGR2GRAY).astype(np.float64)
    right_gray = cv2.cvtColor(right, cv2.COLOR_BGR2GRAY).astype(np.float64)
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    mu_left = cv2.GaussianBlur(left_gray, (11, 11), 1.5)
    mu_right = cv2.GaussianBlur(right_gray, (11, 11), 1.5)
    mu_left_sq = mu_left * mu_left
    mu_right_sq = mu_right * mu_right
    mu_cross = mu_left * mu_right
    sigma_left = cv2.GaussianBlur(left_gray * left_gray, (11, 11), 1.5) - mu_left_sq
    sigma_right = cv2.GaussianBlur(right_gray * right_gray, (11, 11), 1.5) - mu_right_sq
    sigma_cross = cv2.GaussianBlur(left_gray * right_gray, (11, 11), 1.5) - mu_cross
    numerator = (2 * mu_cross + c1) * (2 * sigma_cross + c2)
    denominator = (mu_left_sq + mu_right_sq + c1) * (sigma_left + sigma_right + c2)
    return float(np.mean(numerator / np.maximum(denominator, 1e-12)))


def _icon_patch_features(image: np.ndarray, bbox: list[int]) -> dict[str, float]:
    if len(bbox) != 4:
        return {name: 0.0 for name in ("blue_ratio", "mean_saturation", "mean_value", "gray_contrast", "edge_density", "laplacian_log")}
    x1, y1, x2, y2 = bbox
    patch = image[y1:y2, x1:x2]
    if patch.size == 0:
        return {name: 0.0 for name in ("blue_ratio", "mean_saturation", "mean_value", "gray_contrast", "edge_density", "laplacian_log")}
    hsv = cv2.cvtColor(patch, cv2.COLOR_BGR2HSV)
    hue, saturation, value = cv2.split(hsv)
    blue = (hue >= 85) & (hue <= 125) & (saturation >= 25) & (value >= 20)
    gray = cv2.cvtColor(patch, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(cv2.createCLAHE(clipLimit=1.6, tileGridSize=(4, 4)).apply(gray), 24, 96)
    laplacian = float(cv2.Laplacian(gray, cv2.CV_64F).var())
    return {
        "blue_ratio": float(np.mean(blue)),
        "mean_saturation": float(np.mean(saturation)) / 255.0,
        "mean_value": float(np.mean(value)) / 255.0,
        "gray_contrast": float(np.std(gray)) / 128.0,
        "edge_density": float(np.mean(edges > 0)),
        "laplacian_log": float(np.log1p(laplacian)) / 10.0,
    }


def _percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(quantile * len(ordered)) - 1))
    return float(ordered[index])


def _corrected_icon_records(
    icon_rows: list[dict],
    split_rows: dict[str, dict],
    include_test_labels: bool = False,
) -> tuple[list[dict], dict[tuple[str, str], str]]:
    template_rows: list[dict] = []
    labels: dict[tuple[str, str], str] = {}
    for source in icon_rows:
        annotation_id = source["annotation_id"]
        split_row = split_rows.get(annotation_id)
        if split_row is None:
            continue
        if split_row["split"] == "test" and not include_test_labels:
            continue
        corrected_ground_truth = split_row["ground_truth"]
        side = source["side"]
        if corrected_ground_truth == "EV" and source["icon_label"] == "present" and not source.get("exclusion_reason", ""):
            label = "present"
        elif corrected_ground_truth == "NON_EV":
            label = "absent"
        else:
            label = "excluded"
        if split_row["split"] == "train" and label in {"present", "absent"}:
            item = dict(source)
            item["ground_truth"] = corrected_ground_truth
            item["icon_label"] = label
            template_rows.append(item)
        labels[(annotation_id, side)] = label
    return template_rows, labels


def _build_template_cache(template_rows: list[dict]) -> dict[tuple[str, str, str], list]:
    cache: dict[tuple[str, str, str], list] = {}
    positive_ids = {row["annotation_id"] for row in template_rows if row["icon_label"] == "present"}
    negative_ids = {row["annotation_id"] for row in template_rows if row["icon_label"] == "absent"}
    for side in ("left", "right"):
        positives = build_templates(template_rows, side)
        reference_bbox = median_bbox(positives, side)
        negatives = build_absent_templates(template_rows, side, reference_bbox)
        cache[(side, "positive", "")] = positives
        cache[(side, "negative", "")] = negatives
        for annotation_id in positive_ids:
            cache[(side, "positive", annotation_id)] = [
                item for item in positives if item.annotation_id != annotation_id
            ]
        for annotation_id in negative_ids:
            cache[(side, "negative", annotation_id)] = [
                item for item in negatives if item.annotation_id != annotation_id
            ]
    return cache


def _templates_for(
    cache: dict[tuple[str, str, str], list], split: str, annotation_id: str, side: str, label: str
) -> tuple[list, list]:
    positive_leave_out = annotation_id if split == "train" and label == "present" else ""
    negative_leave_out = annotation_id if split == "train" and label == "absent" else ""
    return (
        cache[(side, "positive", positive_leave_out)],
        cache[(side, "negative", negative_leave_out)],
    )


def _evaluate_variant(
    image: np.ndarray,
    candidates: list[dict],
    gt_rectified: np.ndarray,
    variant: str,
    template_cache: dict[tuple[str, str, str], list],
    split: str,
    annotation_id: str,
    icon_labels: dict[tuple[str, str], str],
    icon_strategy: str,
    fallback_gate: str,
    fallback_min: float,
    contrastive_edge_weight: float,
) -> dict:
    started = time.perf_counter()
    def rectify_candidates(rectifier) -> list[dict]:
        outputs: list[dict] = []
        for candidate in candidates:
            x1, y1, x2, y2 = candidate["bbox"]
            crop = image[y1:y2, x1:x2]
            if crop.size == 0:
                continue
            rectified = rectifier(crop, target_size=TARGET_SIZE)
            outputs.append(
                {
                    **candidate,
                    "rect": rectified,
                    "quality": rectification_quality_score(rectified.plate_image),
                }
            )
        return outputs

    legacy_evaluated = rectify_candidates(rectify_plate_legacy)
    fallback_used = 0
    if variant == "baseline":
        evaluated = legacy_evaluated
    else:
        legacy_selection = select_perspective_preferred_candidate(legacy_evaluated) if legacy_evaluated else None
        if legacy_selection is not None and legacy_selection[2].method == "perspective":
            evaluated = legacy_evaluated
        else:
            evaluated = rectify_candidates(rectify_plate)
            fallback_used = 1
    if not evaluated:
        return {
            "selected_rank": 0,
            "selected_bbox": [],
            "selected_iou": 0.0,
            "rectification_method": "failed",
            "rectification_quality": 0.0,
            "rectification_geometry_confidence": 0.0,
            "geometric_success": 0,
            "alignment_ssim": 0.0,
            "left_icon_score": 0.0,
            "right_icon_score": 0.0,
            "quality_aware_fallback_used": fallback_used,
            "icon_fallback_eligible": 0,
            "processing_ms": (time.perf_counter() - started) * 1000.0,
        }
    selected_rank, selected, rectification = select_perspective_preferred_candidate(evaluated)
    selected_iou = float(selected.get("gt_iou", 0.0))
    geometric_success = int(rectification.method in GEOMETRIC_METHODS and selected_iou >= IOU_THRESHOLD)
    fallback_gate_value = (
        float(rectification.rect_quality_input)
        if fallback_gate == "geometry_confidence"
        else float(selected["quality"])
    )
    fallback_eligible = int(not fallback_used or fallback_gate_value >= fallback_min)
    result = {
        "selected_rank": selected_rank,
        "selected_bbox": selected["bbox"],
        "selected_iou": selected_iou,
        "rectification_method": rectification.method,
        "rectification_quality": float(selected["quality"]),
        "rectification_geometry_confidence": float(rectification.rect_quality_input),
        "geometric_success": geometric_success,
        "alignment_ssim": alignment_ssim(rectification.plate_image, gt_rectified) if selected_iou >= IOU_THRESHOLD else 0.0,
        "quality_aware_fallback_used": fallback_used,
        "icon_fallback_gate": fallback_gate,
        "icon_fallback_gate_value": fallback_gate_value,
        "icon_fallback_eligible": fallback_eligible,
    }
    for side in ("left", "right"):
        label = icon_labels.get((annotation_id, side), "excluded")
        positive_templates, negative_templates = _templates_for(template_cache, split, annotation_id, side, label)
        edge_detection = detect_icon(rectification.plate_image, side, positive_templates)
        robust_detection = detect_icon_robust(rectification.plate_image, side, positive_templates)
        contrastive_detection = (
            detect_icon_contrastive(
                rectification.plate_image,
                side,
                positive_templates,
                negative_templates,
                contrastive_edge_weight,
            )
            if variant == "quality_aware" and icon_strategy in {"contrastive", "logistic"}
            else {
                "score": -1.0,
                "bbox": edge_detection.get("bbox", []),
                "positive_edge_similarity": 0.0,
                "negative_edge_similarity": 0.0,
                "positive_robust_similarity": 0.0,
                "negative_robust_similarity": 0.0,
            }
        )
        edge_score = float(edge_detection["score"]) if geometric_success else 0.0
        robust_score = float(robust_detection["score"]) if geometric_success else 0.0
        contrastive_score = float(contrastive_detection["score"]) if geometric_success else -1.0
        result[f"{side}_icon_edge_score"] = edge_score
        result[f"{side}_icon_robust_score"] = robust_score
        result[f"{side}_icon_contrastive_score"] = contrastive_score
        result[f"{side}_feature_template_edge_margin"] = float(
            contrastive_detection.get("positive_edge_similarity", 0.0)
            - contrastive_detection.get("negative_edge_similarity", 0.0)
        )
        result[f"{side}_feature_template_robust_margin"] = float(
            contrastive_detection.get("positive_robust_similarity", 0.0)
            - contrastive_detection.get("negative_robust_similarity", 0.0)
        )
        for feature_name in (
            "positive_edge_similarity",
            "negative_edge_similarity",
            "positive_robust_similarity",
            "negative_robust_similarity",
        ):
            result[f"{side}_feature_{feature_name}"] = float(contrastive_detection.get(feature_name, 0.0))
        patch_features = _icon_patch_features(rectification.plate_image, contrastive_detection.get("bbox", []))
        for feature_name, value in patch_features.items():
            result[f"{side}_feature_{feature_name}"] = value
        selected_icon_score = (
            contrastive_score if variant == "quality_aware" and icon_strategy == "contrastive" else edge_score
        )
        result[f"{side}_icon_score"] = selected_icon_score if fallback_eligible else -1.0
    result["processing_ms"] = (time.perf_counter() - started) * 1000.0
    return result


def _feature_vector(row: dict, side: str) -> np.ndarray:
    values = [float(row.get(f"{side}_feature_{name}", 0.0) or 0.0) for name in LOGISTIC_SIDE_FEATURES]
    values.extend(float(row.get(name, 0.0) or 0.0) for name in LOGISTIC_GLOBAL_FEATURES)
    return np.asarray(values, dtype=np.float64)


def _sigmoid(values: np.ndarray) -> np.ndarray:
    clipped = np.clip(values, -30.0, 30.0)
    return 1.0 / (1.0 + np.exp(-clipped))


def _fit_weighted_logistic(rows: list[dict], side: str) -> dict:
    x = np.vstack([_feature_vector(row, side) for row in rows])
    y = np.asarray([1.0 if row[f"{side}_icon_label"] == "present" else 0.0 for row in rows], dtype=np.float64)
    positive_count = int(np.sum(y == 1.0))
    negative_count = int(np.sum(y == 0.0))
    if not positive_count or not negative_count:
        raise ValueError(f"logistic_training_class_missing:{side}")
    mean = np.mean(x, axis=0)
    scale = np.std(x, axis=0)
    scale[scale < 1e-6] = 1.0
    normalized = (x - mean) / scale
    design = np.column_stack([normalized, np.ones(len(normalized), dtype=np.float64)])
    sample_weight = np.where(y == 1.0, len(y) / (2.0 * positive_count), len(y) / (2.0 * negative_count))
    coefficients = np.zeros(design.shape[1], dtype=np.float64)
    learning_rate = 0.08
    regularization = 0.02
    for _ in range(700):
        probabilities = _sigmoid(design @ coefficients)
        gradient = design.T @ (sample_weight * (probabilities - y)) / float(np.sum(sample_weight))
        gradient[:-1] += regularization * coefficients[:-1]
        coefficients -= learning_rate * gradient
    return {
        "mean": mean,
        "scale": scale,
        "coefficients": coefficients,
        "positive_count": positive_count,
        "negative_count": negative_count,
        "iterations": 700,
        "learning_rate": learning_rate,
        "regularization": regularization,
    }


def _predict_logistic(model: dict, row: dict, side: str) -> float:
    normalized = (_feature_vector(row, side) - model["mean"]) / model["scale"]
    design = np.append(normalized, 1.0)
    return float(_sigmoid(np.asarray([design @ model["coefficients"]]))[0])


def _group_fold(annotation_id: str, fold_count: int = 5) -> int:
    digest = hashlib.sha256(annotation_id.encode("utf-8")).hexdigest()
    return int(digest[:8], 16) % fold_count


def apply_group_oof_logistic(details: list[dict]) -> dict:
    """Apply Train group-OOF scores and one Train-only final model to Validation."""
    candidate_rows = [row for row in details if row["variant"] == "quality_aware"]
    result: dict[str, dict] = {}
    for side in ("left", "right"):
        training_rows = [
            row
            for row in candidate_rows
            if row["split"] == "train"
            and row["environment"] in LOW_QUALITY_ENVIRONMENTS
            and row[f"{side}_icon_label"] in {"present", "absent"}
        ]
        fold_models: list[dict] = []
        fold_audit: list[dict] = []
        for fold in range(5):
            fit_rows = [row for row in training_rows if _group_fold(row["annotation_id"]) != fold]
            holdout_ids = {row["annotation_id"] for row in training_rows if _group_fold(row["annotation_id"]) == fold}
            fit_ids = {row["annotation_id"] for row in fit_rows}
            if fit_ids & holdout_ids:
                raise ValueError(f"logistic_group_leak:{side}:{fold}")
            model = _fit_weighted_logistic(fit_rows, side)
            fold_models.append(model)
            fold_audit.append(
                {
                    "fold": fold,
                    "fit_annotation_count": len(fit_ids),
                    "holdout_annotation_count": len(holdout_ids),
                    "group_overlap": 0,
                }
            )
            for row in candidate_rows:
                if row["split"] == "train" and _group_fold(row["annotation_id"]) == fold:
                    score = _predict_logistic(model, row, side)
                    row[f"{side}_icon_logistic_score"] = score
                    row[f"{side}_icon_score"] = score if int(row["geometric_success"]) and int(row["icon_fallback_eligible"]) else -1.0
        final_model = _fit_weighted_logistic(training_rows, side)
        for row in candidate_rows:
            if row["split"] == "validation":
                score = _predict_logistic(final_model, row, side)
                row[f"{side}_icon_logistic_score"] = score
                row[f"{side}_icon_score"] = score if int(row["geometric_success"]) and int(row["icon_fallback_eligible"]) else -1.0
        result[side] = {
            "feature_names": [*LOGISTIC_SIDE_FEATURES, *LOGISTIC_GLOBAL_FEATURES],
            "fold_audit": fold_audit,
            "final_model": {
                "mean": final_model["mean"].tolist(),
                "scale": final_model["scale"].tolist(),
                "coefficients": final_model["coefficients"].tolist(),
                "positive_count": final_model["positive_count"],
                "negative_count": final_model["negative_count"],
                "iterations": final_model["iterations"],
                "learning_rate": final_model["learning_rate"],
                "regularization": final_model["regularization"],
            },
        }
    return result


def choose_train_threshold(details: list[dict], variant: str, side: str, fpr_cap: float = ICON_TRAIN_FPR_CAP) -> dict:
    """Select a threshold from Train low-quality rows only, maximizing Recall under the FPR cap."""
    rows = [
        row
        for row in details
        if row["split"] == "train"
        and row["variant"] == variant
        and row["environment"] in LOW_QUALITY_ENVIRONMENTS
        and row[f"{side}_icon_label"] in {"present", "absent"}
    ]
    positives = [row for row in rows if row[f"{side}_icon_label"] == "present"]
    negatives = [row for row in rows if row[f"{side}_icon_label"] == "absent"]
    scores = sorted({float(row[f"{side}_icon_score"]) for row in rows})
    candidates = [max(scores, default=0.0) + 1e-6, *scores]
    best = {"threshold": candidates[0], "recall": 0.0, "fpr": 0.0, "positive_count": len(positives), "negative_count": len(negatives)}
    best_key = (-1.0, -1.0, -1.0)
    for threshold in candidates:
        recall = sum(float(row[f"{side}_icon_score"]) >= threshold for row in positives) / max(1, len(positives))
        fpr = sum(float(row[f"{side}_icon_score"]) >= threshold for row in negatives) / max(1, len(negatives))
        if fpr > fpr_cap:
            continue
        key = (recall, -fpr, threshold)
        if key > best_key:
            best_key = key
            best = {
                "threshold": float(threshold),
                "recall": float(recall),
                "fpr": float(fpr),
                "positive_count": len(positives),
                "negative_count": len(negatives),
            }
    return best


def _apply_thresholds(details: list[dict], thresholds: dict[str, dict[str, dict]]) -> None:
    for row in details:
        variant = row["variant"]
        for side in ("left", "right"):
            threshold = float(thresholds[variant][side]["threshold"])
            row[f"{side}_icon_threshold"] = threshold
            row[f"{side}_icon_detected"] = int(float(row[f"{side}_icon_score"]) >= threshold)
        row["vehicle_icon_present"] = int(
            bool(row["left_icon_detected"]) or bool(row["right_icon_detected"])
        )
        row["presence_decision"] = (
            "REVIEW" if not row["geometric_success"] else "PRESENT" if row["vehicle_icon_present"] else "ABSENT"
        )


def _metric_row(rows: list[dict], split: str, environment: str, variant: str) -> dict:
    group = [row for row in rows if row["split"] == split and row["variant"] == variant]
    if environment == "synthetic_low_quality":
        group = [row for row in group if row["environment"] in LOW_QUALITY_ENVIRONMENTS]
    elif environment == "quality_below_0_25":
        group = [row for row in group if row["quality_below_threshold"]]
    else:
        group = [row for row in group if row["environment"] == environment]
    times = [float(row["processing_ms"]) for row in group]
    geometric_rows = [row for row in group if int(row["geometric_success"])]
    side_metrics: dict[str, tuple[int, int, float, float]] = {}
    for side in ("left", "right"):
        positives = [row for row in group if row[f"{side}_icon_label"] == "present"]
        negatives = [row for row in group if row[f"{side}_icon_label"] == "absent"]
        recall = sum(int(row[f"{side}_icon_detected"]) for row in positives) / max(1, len(positives))
        fpr = sum(int(row[f"{side}_icon_detected"]) for row in negatives) / max(1, len(negatives))
        side_metrics[side] = (len(positives), len(negatives), recall, fpr)
    recalls = [value[2] for value in side_metrics.values() if value[0]]
    fprs = [value[3] for value in side_metrics.values() if value[1]]
    ev_with_icon = [
        row
        for row in group
        if row["ground_truth"] == "EV"
        and (row["left_icon_label"] == "present" or row["right_icon_label"] == "present")
    ]
    non_ev = [row for row in group if row["ground_truth"] == "NON_EV"]
    return {
        "split": split,
        "environment": environment,
        "variant": variant,
        "count": len(group),
        "measured_low_quality_count": sum(int(row["quality_below_threshold"]) for row in group),
        "roi_top3_recall": sum(int(row["top3_hit"]) for row in group) / max(1, len(group)),
        "geometric_rectification_success": sum(int(row["geometric_success"]) for row in group) / max(1, len(group)),
        "mean_alignment_ssim": statistics.fmean(float(row["alignment_ssim"]) for row in group) if group else 0.0,
        "mean_geometric_alignment_ssim": statistics.fmean(float(row["alignment_ssim"]) for row in geometric_rows) if geometric_rows else 0.0,
        "left_positive_count": side_metrics["left"][0],
        "left_negative_count": side_metrics["left"][1],
        "left_icon_recall": side_metrics["left"][2],
        "left_icon_fpr": side_metrics["left"][3],
        "right_positive_count": side_metrics["right"][0],
        "right_negative_count": side_metrics["right"][1],
        "right_icon_recall": side_metrics["right"][2],
        "right_icon_fpr": side_metrics["right"][3],
        "icon_macro_recall": statistics.fmean(recalls) if recalls else 0.0,
        "icon_macro_fpr": statistics.fmean(fprs) if fprs else 0.0,
        "vehicle_icon_recall": sum(int(row["vehicle_icon_present"]) for row in ev_with_icon) / max(1, len(ev_with_icon)),
        "non_ev_vehicle_icon_fpr": sum(int(row["vehicle_icon_present"]) for row in non_ev) / max(1, len(non_ev)),
        "review_rate": sum(row["presence_decision"] == "REVIEW" for row in group) / max(1, len(group)),
        "mean_processing_ms": statistics.fmean(times) if times else 0.0,
        "p95_processing_ms": _percentile(times, 0.95),
        "ocr_exact_match": "N/A",
        "ocr_cer": "N/A",
    }


def _write_csv(path: Path, rows: list[dict]) -> None:
    fieldnames: list[str] = []
    for row in rows:
        for fieldname in row:
            if fieldname not in fieldnames:
                fieldnames.append(fieldname)
    if not fieldnames:
        fieldnames = ["annotation_id"]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def _report(summary: dict, metrics: list[dict]) -> str:
    lookup = {(row["split"], row["environment"], row["variant"]): row for row in metrics}
    evaluation_split = summary["evaluation_split"]
    section_title = {
        "train": "Train diagnostic",
        "validation": "Validation",
        "test": "Formal Test",
    }[evaluation_split]
    lines = [
        f"# {summary['experiment_id']}",
        "",
        f"- Status: `{summary['status']}`",
        f"- Train rows: {summary['train_rows']}; Validation rows: {summary['validation_rows']}; Test rows included: {summary['test_rows_included']}; Test rows excluded: {summary['test_rows_excluded']}",
        f"- Formal Test accessed in this run: `{str(summary['formal_test_accessed_in_this_run']).lower()}`; previously opened: `{str(summary['formal_test_previously_opened']).lower()}`",
        "- OCR Exact Match/CER: `N/A` (OCR ground truth 없음)",
        "- 실제 야간/IR·Pi 운영 성능: 미검증",
        "",
        f"## {section_title} synthetic low-quality",
        "",
        "| Variant | ROI Top-3 Recall | Geometric rectification | Conditional alignment SSIM | Icon macro Recall | Vehicle icon Recall | Icon macro FPR | NON_EV vehicle FPR | REVIEW | Mean/P95 ms |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for variant in VARIANTS:
        row = lookup.get((evaluation_split, "synthetic_low_quality", variant), {})
        lines.append(
            "| {variant} | {roi:.2%} | {rect:.2%} | {alignment:.4f} | {recall:.2%} | {vehicle_recall:.2%} | {fpr:.2%} | {vehicle_fpr:.2%} | {review:.2%} | {mean:.2f}/{p95:.2f} |".format(
                variant=variant,
                roi=float(row.get("roi_top3_recall", 0.0)),
                rect=float(row.get("geometric_rectification_success", 0.0)),
                alignment=float(row.get("mean_geometric_alignment_ssim", 0.0)),
                recall=float(row.get("icon_macro_recall", 0.0)),
                vehicle_recall=float(row.get("vehicle_icon_recall", 0.0)),
                fpr=float(row.get("icon_macro_fpr", 0.0)),
                vehicle_fpr=float(row.get("non_ev_vehicle_icon_fpr", 0.0)),
                review=float(row.get("review_rate", 0.0)),
                mean=float(row.get("mean_processing_ms", 0.0)),
                p95=float(row.get("p95_processing_ms", 0.0)),
            )
        )
    lines.extend(
        [
            "",
            "## Gate",
            "",
            *[f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()],
            "",
            "아이콘 결과는 존재 여부 진단이며 아이콘 단독 EV 운영 판정이 아니다. 불확실하거나 평면화에 실패한 입력은 `REVIEW`로 유지한다.",
        ]
    )
    return "\n".join(lines) + "\n"


def run(args: argparse.Namespace) -> dict:
    annotation_rows = read_csv(Path(args.annotation_manifest).resolve())
    split_source = read_csv(Path(args.split_manifest).resolve())
    split_rows = {row["annotation_id"]: row for row in split_source}
    icon_rows = read_csv(Path(args.icon_manifest).resolve())
    thresholds_config = json.loads(Path(args.thresholds).resolve().read_text(encoding="utf-8-sig"))
    include_validation = args.phase == "train-validation"
    include_formal_test = args.phase == "formal-test"
    if include_validation:
        allowed_splits = {"train", "validation"}
    elif include_formal_test:
        allowed_splits = {"train", "test"}
    else:
        allowed_splits = {"train"}
    rows = []
    for source in annotation_rows:
        split_row = split_rows.get(source["annotation_id"])
        if split_row is None or split_row["split"] not in allowed_splits:
            continue
        item = dict(source)
        item["split"] = split_row["split"]
        item["ground_truth"] = split_row["ground_truth"]
        rows.append(item)
    template_rows, icon_labels = _corrected_icon_records(
        icon_rows,
        split_rows,
        include_test_labels=include_formal_test,
    )
    template_cache = _build_template_cache(template_rows)
    details: list[dict] = []
    errors: list[dict] = []
    for row in rows:
        annotation_id = row["annotation_id"]
        source_path = Path(row["source_path"])
        image = read_image(source_path)
        gt_bbox = parse_box(row.get("plate_bbox", ""))
        gt_quad = parse_box(row.get("plate_quad", ""))
        if len(gt_bbox) != 4 and len(gt_quad) == 8:
            xs, ys = gt_quad[0::2], gt_quad[1::2]
            gt_bbox = [min(xs), min(ys), max(xs), max(ys)]
        if len(gt_quad) != 8 and len(gt_bbox) == 4:
            x1, y1, x2, y2 = gt_bbox
            gt_quad = [x1, y1, x2, y1, x2, y2, x1, y2]
        if image is None or len(gt_bbox) != 4 or len(gt_quad) != 8:
            errors.append({"annotation_id": annotation_id, "reason": "input_or_ground_truth_missing"})
            continue
        for environment in ENVIRONMENTS:
            try:
                degraded = apply_environment(image, environment)
                x1, y1, x2, y2 = gt_bbox
                gt_crop = degraded[y1:y2, x1:x2]
                gt_quality = rectification_quality_score(gt_crop)
                gt_rectified = _warp_ground_truth(degraded, gt_quad)
                candidates = ranked_candidates(
                    degraded,
                    thresholds_config,
                    origins=("geometry", "character_edge"),
                    limit=3,
                    adaptive=True,
                    component_group=True,
                    reserve_coarse=True,
                )
                for candidate in candidates:
                    candidate["gt_iou"] = intersection_over_union(candidate["bbox"], gt_bbox)
                top3_iou = max((float(candidate["gt_iou"]) for candidate in candidates), default=0.0)
                for variant in VARIANTS:
                    result = _evaluate_variant(
                        degraded,
                        candidates,
                        gt_rectified,
                        variant,
                        template_cache,
                        row["split"],
                        annotation_id,
                        icon_labels,
                        args.icon_strategy,
                        args.fallback_gate,
                        args.fallback_min,
                        args.contrastive_edge_weight,
                    )
                    details.append(
                        {
                            "annotation_id": annotation_id,
                            "source_path": str(source_path),
                            "split": row["split"],
                            "ground_truth": row["ground_truth"],
                            "plate_status": row.get("plate_status", ""),
                            "environment": environment,
                            "variant": variant,
                            "gt_crop_quality": gt_quality,
                            "quality_threshold": QUALITY_THRESHOLD,
                            "quality_below_threshold": int(gt_quality < QUALITY_THRESHOLD),
                            "candidate_count": len(candidates),
                            "top3_best_iou": top3_iou,
                            "top3_hit": int(top3_iou >= IOU_THRESHOLD),
                            **result,
                            "left_icon_label": icon_labels.get((annotation_id, "left"), "excluded"),
                            "right_icon_label": icon_labels.get((annotation_id, "right"), "excluded"),
                            "error_reason": "",
                        }
                    )
            except Exception as exc:
                errors.append(
                    {"annotation_id": annotation_id, "environment": environment, "reason": f"{type(exc).__name__}:{exc}"}
                )

    logistic_models = apply_group_oof_logistic(details) if args.icon_strategy == "logistic" else {}
    icon_thresholds = {
        variant: {
            side: choose_train_threshold(
                details,
                variant,
                side,
                fpr_cap=args.icon_train_fpr_cap,
            )
            for side in ("left", "right")
        }
        for variant in VARIANTS
    }
    _apply_thresholds(details, icon_thresholds)
    environments = [*ENVIRONMENTS, "synthetic_low_quality", "quality_below_0_25"]
    splits = [
        "train",
        *(["validation"] if include_validation else []),
        *(["test"] if include_formal_test else []),
    ]
    metrics = [
        _metric_row(details, split, environment, variant)
        for split in splits
        for environment in environments
        for variant in VARIANTS
    ]
    metric_lookup = {(row["split"], row["environment"], row["variant"]): row for row in metrics}
    gate: dict[str, bool] = {}
    if include_validation:
        baseline = metric_lookup[("validation", "synthetic_low_quality", "baseline")]
        candidate = metric_lookup[("validation", "synthetic_low_quality", "quality_aware")]
        normal_baseline = metric_lookup[("validation", "normal", "baseline")]
        normal_candidate = metric_lookup[("validation", "normal", "quality_aware")]
        if args.experiment_id in {
            "low-quality-presence-rectification-002",
            "low-quality-presence-rectification-003",
        }:
            gate = {
                "validation_roi_top3_non_regression": candidate["roi_top3_recall"] >= baseline["roi_top3_recall"],
                "validation_geometric_rectification_at_least_step89": candidate["geometric_rectification_success"] >= 0.5476190476190477,
                "validation_geometric_alignment_at_least_0_3526": candidate["mean_geometric_alignment_ssim"] >= 0.3526,
                "validation_icon_macro_recall_improved": candidate["icon_macro_recall"] > baseline["icon_macro_recall"],
                "validation_vehicle_icon_recall_improved": candidate["vehicle_icon_recall"] > baseline["vehicle_icon_recall"],
                "validation_icon_macro_fpr_increase_le_0_10": candidate["icon_macro_fpr"] - baseline["icon_macro_fpr"] <= 0.10,
                "normal_geometric_regression_le_0_05": candidate["geometric_rectification_success"] >= normal_baseline["geometric_rectification_success"] - 0.05,
                "processing_errors_zero": len(errors) == 0,
            }
        elif args.experiment_id == "low-quality-presence-fpr-calibration-004":
            measured_candidate = metric_lookup[("validation", "quality_below_0_25", "quality_aware")]
            quality_thresholds = icon_thresholds["quality_aware"]
            gate = {
                "train_oof_left_fpr_le_0_10": quality_thresholds["left"]["fpr"] <= 0.10,
                "train_oof_right_fpr_le_0_10": quality_thresholds["right"]["fpr"] <= 0.10,
                "validation_roi_top3_non_regression": candidate["roi_top3_recall"] >= baseline["roi_top3_recall"],
                "validation_geometric_rectification_at_least_step89": candidate["geometric_rectification_success"] >= 0.5476190476190477,
                "validation_geometric_alignment_at_least_0_3526": candidate["mean_geometric_alignment_ssim"] >= 0.3526,
                "validation_icon_macro_recall_gt_0_0952": candidate["icon_macro_recall"] > 0.0952,
                "validation_vehicle_icon_recall_gt_0_1786": candidate["vehicle_icon_recall"] > 0.1786,
                "validation_icon_macro_fpr_le_0_15": candidate["icon_macro_fpr"] <= 0.15,
                "validation_non_ev_vehicle_icon_fpr_le_0_20": candidate["non_ev_vehicle_icon_fpr"] <= 0.20,
                "validation_quality_below_0_25_icon_recall_gt_zero": measured_candidate["icon_macro_recall"] > 0.0,
                "normal_geometric_regression_le_0_05": candidate["geometric_rectification_success"] >= normal_baseline["geometric_rectification_success"] - 0.05,
                "processing_errors_zero": len(errors) == 0,
            }
        else:
            gate = {
                "validation_roi_top3_non_regression": candidate["roi_top3_recall"] >= baseline["roi_top3_recall"],
                "validation_geometric_rectification_improved": candidate["geometric_rectification_success"] > baseline["geometric_rectification_success"],
                "validation_icon_macro_recall_improved": candidate["icon_macro_recall"] > baseline["icon_macro_recall"],
                "validation_icon_macro_fpr_increase_le_0_10": candidate["icon_macro_fpr"] - baseline["icon_macro_fpr"] <= 0.10,
                "normal_geometric_regression_le_0_05": candidate["geometric_rectification_success"] >= normal_baseline["geometric_rectification_success"] - 0.05,
                "processing_errors_zero": len(errors) == 0,
            }
    elif include_formal_test:
        baseline = metric_lookup[("test", "synthetic_low_quality", "baseline")]
        candidate = metric_lookup[("test", "synthetic_low_quality", "quality_aware")]
        normal_baseline = metric_lookup[("test", "normal", "baseline")]
        normal_candidate = metric_lookup[("test", "normal", "quality_aware")]
        gate = {
            "test_roi_top3_non_regression": candidate["roi_top3_recall"] >= baseline["roi_top3_recall"],
            "test_geometric_rectification_improved": candidate["geometric_rectification_success"] > baseline["geometric_rectification_success"],
            "test_geometric_alignment_regression_le_0_02": candidate["mean_geometric_alignment_ssim"] >= baseline["mean_geometric_alignment_ssim"] - 0.02,
            "test_icon_macro_recall_improved": candidate["icon_macro_recall"] > baseline["icon_macro_recall"],
            "test_vehicle_icon_recall_improved": candidate["vehicle_icon_recall"] > baseline["vehicle_icon_recall"],
            "test_icon_macro_fpr_increase_le_0_10": candidate["icon_macro_fpr"] - baseline["icon_macro_fpr"] <= 0.10,
            "normal_geometric_regression_le_0_05": candidate["geometric_rectification_success"] >= normal_baseline["geometric_rectification_success"] - 0.05,
            "processing_errors_zero": len(errors) == 0,
        }
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _write_csv(output_dir / "details.csv", details)
    _write_csv(output_dir / "manifest.csv", details)
    _write_csv(output_dir / "environment_summary.csv", metrics)
    if include_formal_test:
        status = "test_pass" if gate and all(gate.values()) else "test_fail"
        evaluation_split = "test"
    elif include_validation:
        if args.experiment_id == "low-quality-presence-fpr-calibration-004" and gate and all(gate.values()):
            status = "development_candidate"
        else:
            status = "candidate" if gate and all(gate.values()) else "rejected"
        evaluation_split = "validation"
    else:
        status = "train_diagnostic_complete"
        evaluation_split = "train"
    test_rows_included = sum(row["split"] == "test" for row in rows)
    total_test_rows = sum(row["split"] == "test" for row in split_source)
    formal_test_previously_opened = include_formal_test or args.formal_test_previously_opened
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "phase": args.phase,
        "evaluation_split": evaluation_split,
        "train_rows": sum(row["split"] == "train" for row in rows),
        "validation_rows": sum(row["split"] == "validation" for row in rows),
        "test_rows_included": test_rows_included,
        "test_rows_excluded": total_test_rows - test_rows_included,
        "detail_rows": len(details),
        "error_count": len(errors),
        "errors": errors,
        "quality_threshold": QUALITY_THRESHOLD,
        "iou_threshold": IOU_THRESHOLD,
        "low_quality_environments": sorted(LOW_QUALITY_ENVIRONMENTS),
        "icon_threshold_selection": f"Train synthetic-low-quality only; maximize Recall with FPR <= {args.icon_train_fpr_cap}",
        "icon_train_fpr_cap": args.icon_train_fpr_cap,
        "icon_strategy": args.icon_strategy,
        "icon_fallback_gate": args.fallback_gate,
        "icon_fallback_min": args.fallback_min,
        "contrastive_edge_weight": args.contrastive_edge_weight,
        "logistic_models": logistic_models,
        "icon_thresholds": icon_thresholds,
        "gate": gate,
        "ocr_exact_match": "N/A_no_ground_truth",
        "ocr_cer": "N/A_no_ground_truth",
        "formal_test_opened": formal_test_previously_opened,
        "formal_test_accessed_in_this_run": include_formal_test,
        "formal_test_previously_opened": formal_test_previously_opened,
        "test_reuse_for_tuning_prohibited": formal_test_previously_opened,
        "operational_ev_classifier": False,
        "real_ir_evaluated": False,
        "source_images_modified": False,
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "REPORT.md").write_text(_report(summary, metrics), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation-manifest", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--thresholds", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--phase", choices=("train", "train-validation", "formal-test"), default="train")
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    parser.add_argument("--icon-strategy", choices=("edge", "contrastive", "logistic"), default="edge")
    parser.add_argument("--fallback-gate", choices=("output_quality", "geometry_confidence"), default="output_quality")
    parser.add_argument("--fallback-min", type=float, default=ICON_FALLBACK_RECT_QUALITY_MIN)
    parser.add_argument("--contrastive-edge-weight", type=float, default=0.70)
    parser.add_argument("--icon-train-fpr-cap", type=float, default=ICON_TRAIN_FPR_CAP)
    parser.add_argument("--formal-test-previously-opened", action="store_true")
    return parser


def main() -> int:
    result = run(build_parser().parse_args())
    return 0 if result["error_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

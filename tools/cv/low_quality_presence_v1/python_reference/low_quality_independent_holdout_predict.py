"""Create frozen baseline/candidate prediction CSVs for a new audited RGB/IR holdout.

Ground-truth geometry and OCR text are intentionally read only after runtime
candidate selection.  They are evaluation metadata, never model inputs.
"""

from __future__ import annotations

import argparse
import csv
import json
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .low_quality_candidate_selector import SELECTOR_FEATURES, predict_selector
from .low_quality_selector_ocr_stress import (
    build_candidate_features,
    choose_frozen_candidate,
)
from .low_quality_presence_rectification import (
    IOU_THRESHOLD,
    LOGISTIC_GLOBAL_FEATURES,
    LOGISTIC_SIDE_FEATURES,
    TARGET_SIZE,
    _build_template_cache,
    _corrected_icon_records,
    _predict_logistic,
    _warp_ground_truth,
    alignment_ssim,
)
from .low_quality_runtime_observability_guard import (
    MODEL_FEATURE_NAMES as GUARD_FEATURE_NAMES,
    attach_runtime_features,
    predict_guard_model,
)
from .low_quality_spatial_icon import (
    MODEL_FEATURE_NAMES as SPATIAL_MODEL_FEATURE_NAMES,
    extract_spatial_features,
    predict_spatial_model,
)
from .low_quality_vehicle_presence_calibration import (
    _extract_icon_features,
    _read_unicode,
    apply_policy,
)
from .ocr_rectification_guard import _ocr
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .roi_annotation import parse_box, read_csv
from .top3_localization import intersection_over_union, ranked_candidates


EXPERIMENT_ID = "low-quality-raw-independent-holdout-predictions-019"
PREDICTION_FIELDS = (
    "annotation_id",
    "icon_presence_predicted",
    "plate_presence_predicted",
    "presence_decision",
    "rectification_success",
    "rectification_method",
    "alignment_ssim",
    "ocr_text",
    "ocr_error",
    "processing_ms",
)


def _load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _as_model(source: dict, expected_features: tuple[str, ...]) -> dict:
    if tuple(source.get("feature_names", ())) != expected_features:
        raise ValueError("frozen_model_feature_schema_mismatch")
    model = {
        "mean": np.asarray(source["mean"], dtype=np.float64),
        "scale": np.asarray(source["scale"], dtype=np.float64),
        "coefficients": np.asarray(source["coefficients"], dtype=np.float64),
    }
    if model["mean"].shape != (len(expected_features),) or model["scale"].shape != (len(expected_features),):
        raise ValueError("frozen_model_vector_shape_mismatch")
    if model["coefficients"].shape != (len(expected_features) + 1,):
        raise ValueError("frozen_model_coefficient_shape_mismatch")
    if np.any(model["scale"] <= 0.0):
        raise ValueError("frozen_model_nonpositive_scale")
    return model


def _as_legacy_known_feature_model(source: dict, expected_features: tuple[str, ...]) -> dict:
    """Validate a pre-schema artifact against its documented fixed feature order."""
    if source.get("feature_names"):
        return _as_model(source, expected_features)
    return _as_model({**source, "feature_names": list(expected_features)}, expected_features)


def _load_frozen_models(
    selector_summary: Path,
    selector_side_summary: Path,
    spatial_summary: Path,
    guard_summary: Path,
) -> dict:
    selector = _load_json(selector_summary)
    side = _load_json(selector_side_summary)
    spatial = _load_json(spatial_summary)
    guard = _load_json(guard_summary)
    if selector.get("status") != "selector_development_candidate":
        raise ValueError("selector_summary_not_frozen_candidate")
    if spatial.get("status") != "spatial_descriptor_development_candidate":
        raise ValueError("spatial_summary_not_frozen_candidate")
    if guard.get("status") != "runtime_observability_guard_development_candidate":
        raise ValueError("guard_summary_not_frozen_candidate")
    frozen_selector = _as_model(selector["frozen_model"], SELECTOR_FEATURES)
    selector_sides = {
        name: _as_legacy_known_feature_model(
            side["logistic_models"][name]["final_model"],
            (*LOGISTIC_SIDE_FEATURES, *LOGISTIC_GLOBAL_FEATURES),
        )
        for name in ("left", "right")
    }
    return {
        "selector": frozen_selector,
        "selector_sides": selector_sides,
        "spatial_sides": {
            name: _as_model(spatial["frozen_side_models"][name], SPATIAL_MODEL_FEATURE_NAMES)
            for name in ("left", "right")
        },
        "policies": spatial["frozen_vehicle_policies"],
        "guard": _as_model(guard["frozen_model"], GUARD_FEATURE_NAMES)
        | {"threshold": float(guard["frozen_model"]["threshold"])},
    }


def _template_context(split_manifest: Path, icon_manifest: Path) -> dict:
    split_rows = {row["annotation_id"]: row for row in read_csv(split_manifest)}
    template_rows, overlap = _corrected_icon_records(
        read_csv(icon_manifest), split_rows, include_test_labels=False
    )
    if overlap:
        raise ValueError("template_formal_test_overlap")
    return _build_template_cache(template_rows)


def _global_icon_probability(
    plate_image: np.ndarray, side: str, template_cache: dict, model: dict, base: dict
) -> float:
    features = _extract_icon_features(plate_image, side, template_cache, base)
    return _predict_logistic(model, features, side)


def _runtime_presence_row(
    plate_image: np.ndarray,
    rectification_method: str,
    geometry_confidence: float,
    output_quality: float,
    input_quality: float,
    frozen: dict,
    template_cache: dict,
) -> dict:
    runtime_geometric = int(rectification_method in GEOMETRIC_METHODS)
    row = {
        "geometric_success": runtime_geometric,
        "rectification_geometry_confidence": float(geometry_confidence),
        "rectification_quality": float(output_quality),
        "quality_aware_fallback_used": 1.0,
    }
    for side in ("left", "right"):
        row.update(extract_spatial_features(plate_image, side))
        row.update(_extract_icon_features(plate_image, side, template_cache, row))
        probability = predict_spatial_model(frozen["spatial_sides"][side], row, side)
        row[f"{side}_icon_probability"] = probability
        row[f"{side}_icon_score"] = probability if runtime_geometric else -1.0
    # The low-quality cohort is a frozen raw-input property.  Rectification
    # output quality is observable evidence, but must not move a sample
    # between the pre-registered Recall/FPR cohorts.
    row["input_quality"] = float(input_quality)
    row["quality_below_threshold"] = int(input_quality < 0.25)
    cohort = "quality_below_0_25" if row["quality_below_threshold"] else "quality_at_or_above_0_25"
    apply_policy(row, frozen["policies"][cohort], "runtime")
    attach_runtime_features([row])
    risks = {}
    for side in ("left", "right"):
        risks[side] = predict_guard_model(frozen["guard"], row, side)
    row["guard_risk_max"] = max(risks.values())
    row["guard_flag"] = int(any(value >= frozen["guard"]["threshold"] for value in risks.values()))
    row["runtime_presence_decision_before_guard"] = row["runtime_presence_decision"]
    if row["runtime_presence_decision"] == "ABSENT" and row["guard_flag"]:
        row["runtime_presence_decision"] = "REVIEW"
        row["runtime_decision_reason"] = "review_runtime_icon_observability_low_confidence"
    else:
        row["runtime_decision_reason"] = "frozen_spatial_policy"
    return row


def _candidate_records(
    image: np.ndarray,
    thresholds: dict,
    frozen: dict,
    template_cache: dict,
    evaluation_limit: int | None = None,
) -> list[dict]:
    input_quality = rectification_quality_score(cv2.resize(image, TARGET_SIZE, interpolation=cv2.INTER_AREA))
    candidates = ranked_candidates(
        image,
        thresholds,
        origins=("geometry", "character_edge"),
        limit=3,
        adaptive=True,
        component_group=True,
        reserve_coarse=True,
    )
    records: list[dict] = []
    for rank, candidate in enumerate(candidates, 1):
        x1, y1, x2, y2 = [int(value) for value in candidate["bbox"]]
        crop = image[y1:y2, x1:x2]
        if crop.size == 0:
            continue
        rectified = rectify_plate(crop, TARGET_SIZE)
        output_quality = rectification_quality_score(rectified.plate_image)
        selector_base = {
            "rectification_geometry_confidence": float(rectified.rect_quality_input),
            "rectification_quality": float(output_quality),
            "quality_aware_fallback_used": 1.0,
        }
        selector_icons = tuple(
            _global_icon_probability(
                rectified.plate_image, side, template_cache, frozen["selector_sides"][side], selector_base
            )
            for side in ("left", "right")
        )
        features = build_candidate_features(
            candidate,
            rank,
            image.shape,
            rectified.method,
            float(rectified.rect_quality_input),
            float(output_quality),
            float(input_quality),
            selector_icons,
        )
        runtime = _runtime_presence_row(
            rectified.plate_image,
            rectified.method,
            float(rectified.rect_quality_input),
            float(output_quality),
            float(input_quality),
            frozen,
            template_cache,
        )
        records.append(
            {
                "candidate_rank": rank,
                "candidate_origin": candidate["origin"],
                "candidate_bbox": [x1, y1, x2, y2],
                "rectified_image": rectified.plate_image,
                "rectification_method": rectified.method,
                "runtime_geometric": int(rectified.method in GEOMETRIC_METHODS),
                "frozen_selector_probability": predict_selector(frozen["selector"], features),
                "runtime": runtime,
            }
        )
        if evaluation_limit is not None and len(records) >= evaluation_limit:
            break
    return records


def _geometry_evaluation(image: np.ndarray, audit_row: dict, selected: dict | None) -> tuple[int, str]:
    if selected is None or audit_row.get("geometry_label_status") != "available":
        return 0, ""
    quad = parse_box(audit_row.get("plate_quad"))
    if len(quad) != 8:
        raise ValueError(f"geometry_quad_missing:{audit_row['annotation_id']}")
    points = np.asarray(quad, dtype=np.float32).reshape(4, 2)
    x1, y1 = np.min(points, axis=0)
    x2, y2 = np.max(points, axis=0)
    gt_box = [int(x1), int(y1), int(x2), int(y2)]
    selected_iou = intersection_over_union(selected["candidate_bbox"], gt_box)
    success = int(selected["runtime_geometric"] and selected_iou >= IOU_THRESHOLD)
    if not success:
        return 0, ""
    expected = _warp_ground_truth(image, quad)
    return 1, f"{alignment_ssim(selected['rectified_image'], expected):.12f}"


def _prediction(annotation_id: str, selected: dict | None, geometry: tuple[int, str], ocr: tuple[str, str], elapsed_ms: float) -> dict:
    if selected is None:
        return {
            "annotation_id": annotation_id,
            "icon_presence_predicted": 0,
            "plate_presence_predicted": 0,
            "presence_decision": "REVIEW",
            "rectification_success": 0,
            "rectification_method": "no_candidate",
            "alignment_ssim": "",
            "ocr_text": ocr[0],
            "ocr_error": ocr[1],
            "processing_ms": f"{elapsed_ms:.6f}",
        }
    runtime = selected["runtime"]
    return {
        "annotation_id": annotation_id,
        "icon_presence_predicted": int(runtime["runtime_vehicle_present"]),
        "plate_presence_predicted": 1,
        "presence_decision": runtime["runtime_presence_decision"],
        "rectification_success": geometry[0],
        "rectification_method": selected["rectification_method"],
        "alignment_ssim": geometry[1],
        "ocr_text": ocr[0],
        "ocr_error": ocr[1],
        "processing_ms": f"{elapsed_ms:.6f}",
    }


def _write_csv(path: Path, rows: list[dict], fields: tuple[str, ...] | None = None) -> None:
    actual_fields = list(fields or tuple(dict.fromkeys(field for row in rows for field in row)) or ("annotation_id",))
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=actual_fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def run(args: argparse.Namespace) -> dict:
    audit_summary = _load_json(Path(args.audit_summary))
    if audit_summary.get("status") != "holdout_ready" or not all(audit_summary.get("gate", {}).values()):
        raise ValueError("holdout_audit_not_ready")
    audit_rows = [row for row in read_csv(Path(args.audit_details)) if row.get("eligible_holdout") == "1"]
    if not audit_rows:
        raise ValueError("eligible_holdout_rows_empty")
    ids = [row.get("annotation_id", "") for row in audit_rows]
    if not all(ids) or len(ids) != len(set(ids)):
        raise ValueError("eligible_holdout_annotation_id_invalid")
    frozen = _load_frozen_models(
        Path(args.selector_summary), Path(args.selector_side_summary), Path(args.spatial_summary), Path(args.guard_summary)
    )
    with Path(args.thresholds).open(encoding="utf-8") as handle:
        thresholds = json.load(handle)
    template_cache = _template_context(Path(args.split_manifest), Path(args.icon_manifest))
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    baseline_rows: list[dict] = []
    candidate_rows: list[dict] = []
    details: list[dict] = []
    for audit_row in audit_rows:
        annotation_id = audit_row["annotation_id"]
        source_path = Path(audit_row.get("source_path", ""))
        image = _read_unicode(source_path)
        if image is None:
            raise ValueError(f"holdout_source_unreadable:{annotation_id}")
        baseline_started = time.perf_counter()
        baseline_records = _candidate_records(
            image, thresholds, frozen, template_cache, evaluation_limit=1
        )
        baseline_pre_ocr_ms = (time.perf_counter() - baseline_started) * 1000.0
        baseline = baseline_records[0] if baseline_records else None
        candidate_started = time.perf_counter()
        candidate_records = _candidate_records(image, thresholds, frozen, template_cache)
        candidate_pre_ocr_ms = (time.perf_counter() - candidate_started) * 1000.0
        candidate = choose_frozen_candidate(candidate_records)
        baseline_geometry = _geometry_evaluation(image, audit_row, baseline)
        candidate_geometry = _geometry_evaluation(image, audit_row, candidate)
        ocr_started = time.perf_counter()
        ocr_text, ocr_error = "", ""
        if audit_row.get("ocr_label_status") == "available":
            if baseline is None:
                ocr_error = "baseline_no_candidate"
            elif not args.tessdata_dir or not args.tesseract_executable:
                ocr_error = "ocr_runtime_not_configured"
            else:
                ocr_text, ocr_error = _ocr(
                    baseline["rectified_image"], Path(args.tessdata_dir), Path(args.tesseract_executable), float(args.ocr_timeout_seconds)
                )
        ocr_ms = (time.perf_counter() - ocr_started) * 1000.0
        baseline_total_ms = baseline_pre_ocr_ms + ocr_ms
        candidate_total_ms = candidate_pre_ocr_ms + ocr_ms
        baseline_rows.append(_prediction(annotation_id, baseline, baseline_geometry, (ocr_text, ocr_error), baseline_total_ms))
        candidate_rows.append(_prediction(annotation_id, candidate, candidate_geometry, (ocr_text, ocr_error), candidate_total_ms))
        details.append({
            "annotation_id": annotation_id,
            "candidate_count": len(candidate_records),
            "baseline_candidate_rank": "" if baseline is None else baseline["candidate_rank"],
            "frozen_candidate_rank": "" if candidate is None else candidate["candidate_rank"],
            "baseline_bbox": "" if baseline is None else json.dumps(baseline["candidate_bbox"]),
            "frozen_bbox": "" if candidate is None else json.dumps(candidate["candidate_bbox"]),
            "ocr_crop_route": "baseline_selected_crop_shared_with_candidate",
            "baseline_pre_ocr_ms": f"{baseline_pre_ocr_ms:.6f}",
            "candidate_pre_ocr_ms": f"{candidate_pre_ocr_ms:.6f}",
            "shared_baseline_ocr_ms": f"{ocr_ms:.6f}",
            "evaluation_gt_used_after_runtime_selection": 1,
        })
    _write_csv(output_dir / "baseline_predictions.csv", baseline_rows, PREDICTION_FIELDS)
    _write_csv(output_dir / "frozen_candidate_predictions.csv", candidate_rows, PREDICTION_FIELDS)
    _write_csv(output_dir / "details.csv", details)
    _write_csv(output_dir / "manifest.csv", [{"annotation_id": row["annotation_id"]} for row in audit_rows])
    environment_rows = [
        {"camera_mode": key[0], "ir_mode": key[1], "count": count}
        for key, count in sorted(Counter((row.get("camera_mode", ""), row.get("ir_mode", "")) for row in audit_rows).items())
    ]
    _write_csv(output_dir / "environment_summary.csv", environment_rows)
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "raw_holdout_predictions_ready",
        "eligible_holdout_rows": len(audit_rows),
        "baseline_predictions": str(output_dir / "baseline_predictions.csv"),
        "candidate_predictions": str(output_dir / "frozen_candidate_predictions.csv"),
        "runtime_decision_inputs": ["raw_image", "candidate_geometry", "rectification_method", "rectification_quality", "frozen_models"],
        "evaluation_only_inputs": ["plate_quad", "ocr_ground_truth", "physical_presence_label", "visibility_labels"],
        "ocr_route": "baseline_selected_crop_shared_with_candidate",
        "performance_claim": False,
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit-summary", required=True)
    parser.add_argument("--audit-details", required=True)
    parser.add_argument("--thresholds", required=True)
    parser.add_argument("--split-manifest", required=True)
    parser.add_argument("--icon-manifest", required=True)
    parser.add_argument("--selector-summary", required=True)
    parser.add_argument("--selector-side-summary", required=True)
    parser.add_argument("--spatial-summary", required=True)
    parser.add_argument("--guard-summary", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--tessdata-dir")
    parser.add_argument("--tesseract-executable")
    parser.add_argument("--ocr-timeout-seconds", type=float, default=10.0)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    print(json.dumps(run(args), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

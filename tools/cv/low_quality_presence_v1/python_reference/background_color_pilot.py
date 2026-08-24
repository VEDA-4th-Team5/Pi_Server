from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .color_features import build_color_mask
from .roi_annotation import read_csv
from .top3_localization import read_image


def central_background_features(plate: np.ndarray) -> dict[str, float | int]:
    if plate is None or plate.size == 0:
        raise ValueError("empty plate")
    height, width = plate.shape[:2]
    x1, x2 = int(round(width * 0.15)), int(round(width * 0.85))
    y1, y2 = int(round(height * 0.12)), int(round(height * 0.88))
    center = plate[y1:y2, x1:x2]
    hsv = cv2.cvtColor(center, cv2.COLOR_BGR2HSV)
    lab = cv2.cvtColor(center, cv2.COLOR_BGR2LAB)
    gray = cv2.cvtColor(center, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(gray, 45, 140)
    foreground = cv2.dilate(edges, np.ones((3, 3), np.uint8), iterations=1) > 0
    valid = (~foreground) & (hsv[:, :, 2] >= 35) & (hsv[:, :, 2] <= 250)
    valid_count = int(np.count_nonzero(valid))
    if valid_count < max(20, int(0.08 * valid.size)):
        valid = (hsv[:, :, 2] >= 25) & (hsv[:, :, 2] <= 252)
        valid_count = int(np.count_nonzero(valid))
    h, s, v = hsv[:, :, 0], hsv[:, :, 1], hsv[:, :, 2]
    pale_cyan = valid & (h >= 78) & (h <= 116) & (s >= 15) & (s <= 210) & (v >= 60)
    raw_ratio = float(np.count_nonzero(pale_cyan) / max(1, valid_count))
    normalized_v = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 4)).apply(v)
    normalized_cyan = valid & (h >= 76) & (h <= 118) & (s >= 12) & (s <= 220) & (normalized_v >= 55)
    normalized_ratio = float(np.count_nonzero(normalized_cyan) / max(1, valid_count))
    lab_b = lab[:, :, 2].astype(np.float32)
    lab_blue = np.clip((132.0 - lab_b) / 34.0, 0.0, 1.0)
    lab_blue_score = float(np.mean(lab_blue[valid])) if valid_count else 0.0
    b_std = float(np.std(lab_b[valid])) if valid_count else 255.0
    uniformity = float(1.0 / (1.0 + b_std / 18.0))
    score = float(
        0.34 * min(1.0, raw_ratio / 0.55)
        + 0.26 * min(1.0, normalized_ratio / 0.60)
        + 0.28 * lab_blue_score
        + 0.12 * uniformity
    )
    return {
        "central_raw_cyan_ratio": raw_ratio,
        "central_normalized_cyan_ratio": normalized_ratio,
        "central_lab_blue_score": lab_blue_score,
        "central_uniformity": uniformity,
        "central_color_score": score,
        "valid_pixel_ratio": float(valid_count / max(1, valid.size)),
        "dark_ratio": float(np.mean(v < 40)),
        "overexposed_ratio": float(np.mean(v > 245)),
        "low_saturation_ratio": float(np.mean(s < 12)),
        "color_unavailable": int(valid_count < max(20, int(0.08 * valid.size)) or np.mean(s < 12) > 0.92),
    }


def binary_auc(positive: list[float], negative: list[float]) -> float:
    if not positive or not negative:
        return 0.0
    wins = 0.0
    for pos in positive:
        for neg in negative:
            wins += 1.0 if pos > neg else 0.5 if pos == neg else 0.0
    return float(wins / (len(positive) * len(negative)))


def evaluate(args: argparse.Namespace) -> int:
    manifest_rows = read_csv(Path(args.manifest).resolve())
    rectification_rows = read_csv(Path(args.rectification_details).resolve())
    thresholds = json.loads(Path(args.config).read_text(encoding="utf-8-sig"))
    rectified_by_id: dict[str, list[dict]] = defaultdict(list)
    for row in rectification_rows:
        rectified_by_id[row["annotation_id"]].append(row)
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    details: list[dict] = []
    manifest_out: list[dict] = []
    errors: list[dict] = []

    for row in manifest_rows:
        annotation_id = row["annotation_id"]
        source = read_image(Path(row["source_path"]))
        if source is None:
            errors.append({"annotation_id": annotation_id, "reason": "source_read_failed"})
            continue
        _, whole_metrics = build_color_mask(source, thresholds)
        candidate_rows = rectified_by_id.get(annotation_id, [])
        feature_rows: list[dict] = []
        for candidate in candidate_rows:
            plate = read_image(Path(candidate["after_path"]))
            if plate is None:
                errors.append({"annotation_id": annotation_id, "reason": "rectified_read_failed", "candidate_rank": candidate["candidate_rank"]})
                continue
            try:
                features = central_background_features(plate)
            except Exception as exc:
                errors.append({"annotation_id": annotation_id, "reason": f"{type(exc).__name__}:{exc}", "candidate_rank": candidate["candidate_rank"]})
                continue
            detail = {
                "annotation_id": annotation_id,
                "source_path": row["source_path"],
                "ground_truth": row.get("ground_truth", ""),
                "plate_status": row.get("plate_status", ""),
                "candidate_rank": int(candidate["candidate_rank"]),
                "candidate_iou": candidate["candidate_iou"],
                "rectification_method": candidate["method"],
                "rectification_status": candidate["status"],
                **{key: f"{value:.6f}" if isinstance(value, float) else value for key, value in features.items()},
                "whole_image_color_ratio": f"{whole_metrics['global_ratio']:.6f}",
                "error_reason": "",
            }
            feature_rows.append(detail)
            details.append(detail)
        max_score = max((float(item["central_color_score"]) for item in feature_rows), default=0.0)
        top1_score = next((float(item["central_color_score"]) for item in feature_rows if int(item["candidate_rank"]) == 1), 0.0)
        oracle_row = max(feature_rows, key=lambda item: float(item["candidate_iou"]), default=None)
        manifest_out.append(
            {
                "annotation_id": annotation_id,
                "source_path": row["source_path"],
                "ground_truth": row.get("ground_truth", ""),
                "plate_status": row.get("plate_status", ""),
                "whole_image_color_ratio": f"{whole_metrics['global_ratio']:.6f}",
                "top1_central_color_score": f"{top1_score:.6f}",
                "top3_max_central_color_score": f"{max_score:.6f}",
                "oracle_best_iou_color_score_diagnostic_only": f"{float(oracle_row['central_color_score']) if oracle_row else 0.0:.6f}",
                "candidate_count": len(feature_rows),
                "color_unavailable_count": sum(int(item["color_unavailable"]) for item in feature_rows),
                "decision": "FEATURE_ONLY",
                "error_reason": "",
            }
        )

    for name, rows in (("details.csv", details), ("manifest.csv", manifest_out)):
        fields = list(rows[0].keys()) if rows else ["annotation_id"]
        with (output_dir / name).open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    score_names = ["whole_image_color_ratio", "top1_central_color_score", "top3_max_central_color_score", "oracle_best_iou_color_score_diagnostic_only"]
    metrics: dict[str, dict] = {}
    environment_rows: list[dict] = []
    for score_name in score_names:
        positives = [float(item[score_name]) for item in manifest_out if item["ground_truth"] == "EV"]
        negatives = [float(item[score_name]) for item in manifest_out if item["ground_truth"] == "NON_EV"]
        metrics[score_name] = {
            "auc": binary_auc(positives, negatives),
            "ev_mean": statistics.fmean(positives) if positives else 0.0,
            "non_ev_mean": statistics.fmean(negatives) if negatives else 0.0,
        }
        environment_rows.append({"feature": score_name, **metrics[score_name]})
    with (output_dir / "environment_summary.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(environment_rows[0].keys()))
        writer.writeheader()
        writer.writerows(environment_rows)

    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "pilot_feature_extraction_complete" if len(manifest_out) == len(manifest_rows) and not errors else "pilot_failed",
        "input_rows": len(manifest_rows),
        "manifest_rows": len(manifest_out),
        "detail_rows": len(details),
        "error_count": len(errors),
        "errors": errors,
        "metrics": metrics,
        "formal_step4_gate_passed": False,
        "formal_gate_blocker": "group-separated Train/Validation split is not available; all Pilot rows are unassigned",
        "threshold_selected": False,
        "independent_test": False,
        "performance_claim": False,
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if not errors else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser("Extract central plate background color features")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--rectification-details", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--experiment-id", default="ev-roi-icon-002-step04-background-color-pilot107")
    return parser


def main() -> int:
    return evaluate(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())


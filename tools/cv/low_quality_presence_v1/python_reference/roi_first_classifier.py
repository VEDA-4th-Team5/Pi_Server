"""Conservative, color-independent plate-ROI-first EV candidate evaluator."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import time
from concurrent.futures import ProcessPoolExecutor
from collections import Counter, defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path

import cv2
import numpy as np

from .color_features import build_color_mask
from .icon_features import analyze_icon
from .plate_background import compute_background_scores
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate
from .rectification_quality import rectification_quality_score
from .top3_localization import ranked_candidates


@dataclass
class RoiFirstRecord:
    source_path: str
    ground_truth: str
    split: str
    candidate_count: int
    candidate_origin: str
    candidate_rank: int
    candidate_score: float
    plate_roi_bbox: list[int]
    rectification_method: str
    rectification_quality: float
    central_blue_ratio: float
    lab_color_score: float
    left_icon_score: float
    right_icon_score: float
    predicted_class: str
    review_reason: str
    processing_ms: float
    error_reason: str


def select_perspective_preferred_candidate(candidates: list[dict]) -> tuple[int, dict, object]:
    """Select the strongest structural candidate, preferring a valid perspective ROI."""
    if not candidates:
        raise ValueError("candidates must not be empty")
    ranked: list[tuple[tuple[float, float, float, float], int, dict, object]] = []
    for index, item in enumerate(candidates, start=1):
        rect = item["rect"]
        method_priority = 2.0 if rect.method == "perspective" else 1.0 if rect.method in GEOMETRIC_METHODS else 0.0
        key = (
            method_priority,
            float(item["score"]),
            float(item["quality"]),
            -float(index),
        )
        ranked.append((key, index, item, rect))
    _, rank, selected, rectification = max(ranked, key=lambda value: value[0])
    return rank, selected, rectification

def resolve_rectified_size(config: dict) -> tuple[int, int]:
    raw = config.get("roi_first", {}).get("rectified_size", [440, 100])
    if not isinstance(raw, (list, tuple)) or len(raw) != 2:
        raise ValueError("rectified_size must be [width, height]")
    width, height = int(raw[0]), int(raw[1])
    if width < 32 or height < 16:
        raise ValueError("rectified_size is too small")
    return width, height

def decide_roi_first(
    *,
    candidate_count: int,
    candidate_score: float,
    rectification_method: str,
    rectification_quality: float,
    central_blue_ratio: float,
    lab_color_score: float,
    config: dict,
) -> tuple[str, str]:
    policy = config["roi_first"]
    if candidate_count == 0:
        return "NON_EV", "no_color_independent_roi_candidate"
    if candidate_score < float(policy["candidate_score_min"]):
        return "REVIEW", "roi_candidate_score_low"
    if bool(policy["automatic_requires_perspective"]) and rectification_method != "perspective":
        return "REVIEW", "perspective_plate_quad_missing"
    if rectification_quality < float(policy["rectification_quality_min"]):
        return "REVIEW", "rectification_quality_low"
    if central_blue_ratio < float(policy["central_blue_ratio_min"]):
        return "REVIEW", "roi_blue_signal_low"
    if lab_color_score < float(policy["lab_color_score_min"]):
        return "REVIEW", "roi_lab_color_low"
    return "EV", "roi_geometry_color_verified"


def _read_image(path: str) -> np.ndarray | None:
    try:
        encoded = np.fromfile(path, dtype=np.uint8)
    except OSError:
        return None
    return cv2.imdecode(encoded, cv2.IMREAD_COLOR) if encoded.size else None


def classify_roi_first(path: str, ground_truth: str, split: str, thresholds: dict, config: dict) -> RoiFirstRecord:
    started = time.perf_counter()
    image = _read_image(path)
    if image is None:
        return RoiFirstRecord(path, ground_truth, split, 0, "", 0, 0.0, [], "failed", 0.0, 0.0, 0.0, 0.0, 0.0, "REVIEW", "read_error", 0.0, "read_error")
    candidate_selection = str(config["roi_first"].get("candidate_selection", "top1"))
    target_size = resolve_rectified_size(config)
    candidates = ranked_candidates(
        image,
        thresholds,
        origins=("geometry", "character_edge"),
        limit=3 if candidate_selection == "top3_perspective_preferred" else 1,
        adaptive=True,
        component_group=bool(config["roi_first"].get("include_component_group", False)),
        reserve_coarse=True,
    )
    if not candidates:
        elapsed = (time.perf_counter() - started) * 1000.0
        return RoiFirstRecord(path, ground_truth, split, 0, "", 0, 0.0, [], "failed", 0.0, 0.0, 0.0, 0.0, 0.0, "NON_EV", "no_color_independent_roi_candidate", elapsed, "")
    evaluated: list[dict] = []
    for candidate in candidates:
        x1, y1, x2, y2 = candidate["bbox"]
        crop = image[y1:y2, x1:x2]
        if crop.size == 0:
            continue
        rect = rectify_plate(crop, target_size=target_size)
        evaluated.append({**candidate, "rect": rect, "quality": rectification_quality_score(rect.plate_image)})
    if not evaluated:
        elapsed = (time.perf_counter() - started) * 1000.0
        first = candidates[0]
        return RoiFirstRecord(path, ground_truth, split, len(candidates), first["origin"], 0, float(first["score"]), first["bbox"], "failed", 0.0, 0.0, 0.0, 0.0, 0.0, "REVIEW", "empty_roi_crop", elapsed, "empty_roi_crop")
    if candidate_selection == "top3_perspective_preferred":
        candidate_rank, candidate, rect = select_perspective_preferred_candidate(evaluated)
    else:
        candidate_rank, candidate, rect = 1, evaluated[0], evaluated[0]["rect"]
    quality = float(candidate["quality"])
    background = compute_background_scores(rect.plate_image)
    gray = cv2.cvtColor(rect.plate_image, cv2.COLOR_BGR2GRAY)
    left_score, _, _ = analyze_icon(gray, "left")
    right_score, _, _ = analyze_icon(gray, "right")
    decision, reason = decide_roi_first(
        candidate_count=len(candidates),
        candidate_score=float(candidate["score"]),
        rectification_method=rect.method,
        rectification_quality=quality,
        central_blue_ratio=float(background["blue_ratio"]),
        lab_color_score=float(background["lab_score"]),
        config=config,
    )
    elapsed = (time.perf_counter() - started) * 1000.0
    return RoiFirstRecord(
        path, ground_truth, split, len(candidates), str(candidate["origin"]), candidate_rank, float(candidate["score"]), candidate["bbox"], rect.method,
        quality, float(background["blue_ratio"]), float(background["lab_score"]), left_score, right_score, decision, reason, elapsed, ""
    )


def _metrics(records: list[RoiFirstRecord]) -> dict:
    counts = Counter((row.ground_truth, row.predicted_class) for row in records)
    tp, fp = counts[("EV", "EV")], counts[("NON_EV", "EV")]
    fn = counts[("EV", "NON_EV")] + counts[("EV", "REVIEW")]
    non_ev_total = sum(counts[("NON_EV", label)] for label in ("EV", "NON_EV", "REVIEW"))
    return {
        "tp": tp, "fp": fp, "fn": fn,
        "ev_recall": tp / (tp + fn) if tp + fn else 0.0,
        "ev_precision": tp / (tp + fp) if tp + fp else 0.0,
        "non_ev_fpr": fp / non_ev_total if non_ev_total else 0.0,
        "decision_counts": dict(Counter(row.predicted_class for row in records)),
        "confusion": {f"{key[0]}|{key[1]}": value for key, value in sorted(counts.items())},
    }


def _classify_job(job: tuple[dict, dict, dict]) -> RoiFirstRecord:
    row, thresholds, config = job
    return classify_roi_first(row["source_path"], row.get("ground_truth", "UNKNOWN"), row.get("split", ""), thresholds, config)


def evaluate(manifest: Path, output_dir: Path, thresholds: dict, config: dict, workers: int = 1, shard_index: int = 0, shard_count: int = 1) -> dict:
    if shard_count < 1 or shard_index < 0 or shard_index >= shard_count:
        raise ValueError("invalid shard index/count")
    with manifest.open("r", encoding="utf-8-sig", newline="") as handle:
        all_inputs = list(csv.DictReader(handle))
    inputs = [row for index, row in enumerate(all_inputs) if index % shard_count == shard_index]
    jobs = [(row, thresholds, config) for row in inputs]
    if workers > 1:
        with ProcessPoolExecutor(max_workers=workers) as executor:
            records = list(executor.map(_classify_job, jobs, chunksize=32))
    else:
        records = [_classify_job(job) for job in jobs]
    output_dir.mkdir(parents=True, exist_ok=True)
    fieldnames = list(asdict(records[0]).keys()) if records else list(RoiFirstRecord.__annotations__)
    for name in ("manifest.csv", "details.csv"):
        with (output_dir / name).open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(asdict(row) for row in records)
    environments = defaultdict(list)
    for row in records:
        environments[row.split or "all"].append(row)
    with (output_dir / "environment_summary.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["split", "count", "mean_processing_ms", "mean_candidate_count"])
        writer.writeheader()
        for split, items in sorted(environments.items()):
            writer.writerow({"split": split, "count": len(items), "mean_processing_ms": statistics.fmean(item.processing_ms for item in items), "mean_candidate_count": statistics.fmean(item.candidate_count for item in items)})
    times = sorted(row.processing_ms for row in records)
    errors = sum(bool(row.error_reason) for row in records)
    summary = {
        "experiment_id": str(config.get("candidate_id", "ev-roi-first-candidate-001")),
        "status": "exploratory_holdout_measurement",
        "original_input_rows": len(all_inputs), "input_rows": len(inputs), "processed_rows": len(records), "error_count": errors, "shard_index": shard_index, "shard_count": shard_count,
        "metrics": _metrics(records),
        "processing_ms": {"mean": statistics.fmean(times) if times else 0.0, "median": statistics.median(times) if times else 0.0, "p95": times[max(0, int(np.ceil(len(times) * .95)) - 1)] if times else 0.0},
        "rectification_methods": dict(Counter(row.rectification_method for row in records)),
        "review_reasons": dict(Counter(row.review_reason for row in records)),
        "fixed_config": config,
        "formal_test": False,
        "automatic_selection": False,
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate conservative ROI-first EV candidate")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--thresholds", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--shard-index", type=int, default=0)
    parser.add_argument("--shard-count", type=int, default=1)
    args = parser.parse_args()
    thresholds = json.loads(args.thresholds.read_text(encoding="utf-8"))
    config = json.loads(args.config.read_text(encoding="utf-8"))
    print(json.dumps(evaluate(args.manifest, args.output_dir, thresholds, config, max(1, args.workers), args.shard_index, args.shard_count), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

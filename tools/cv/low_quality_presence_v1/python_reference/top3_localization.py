from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .color_features import build_color_mask
from .plate_candidates import detect_candidate_regions
from .roi_annotation import parse_box, read_csv


def read_image(path: Path) -> np.ndarray | None:
    try:
        encoded = np.fromfile(str(path), dtype=np.uint8)
    except OSError:
        return None
    if encoded.size == 0:
        return None
    return cv2.imdecode(encoded, cv2.IMREAD_COLOR)


PIPELINES = (
    "current_color_top1",
    "geometry_only",
    "character_edge_only",
    "color_plus_geometry",
    "combined_top3",
    "combined_corrected_top3",
    "combined_adaptive_top3",
    "combined_component_group_top3",
    "combined_coarse_diverse_top3",
)


def xywh_to_xyxy(box: list[int]) -> list[int]:
    x, y, width, height = box
    return [x, y, x + width, y + height]


def ground_truth_box(row: dict) -> list[int]:
    bbox = parse_box(row.get("plate_bbox"))
    if len(bbox) == 4:
        return bbox
    quad = parse_box(row.get("plate_quad"))
    if len(quad) == 8:
        xs, ys = quad[0::2], quad[1::2]
        return [min(xs), min(ys), max(xs), max(ys)]
    return []


def intersection_over_union(left: list[int], right: list[int]) -> float:
    if len(left) != 4 or len(right) != 4:
        return 0.0
    x1, y1 = max(left[0], right[0]), max(left[1], right[1])
    x2, y2 = min(left[2], right[2]), min(left[3], right[3])
    intersection = max(0, x2 - x1) * max(0, y2 - y1)
    left_area = max(0, left[2] - left[0]) * max(0, left[3] - left[1])
    right_area = max(0, right[2] - right[0]) * max(0, right[3] - right[1])
    union = left_area + right_area - intersection
    return float(intersection / union) if union else 0.0


def _contour_boxes(mask: np.ndarray, image_shape: tuple[int, ...]) -> list[list[int]]:
    height, width = image_shape[:2]
    total = max(1, height * width)
    contours, _ = cv2.findContours(mask, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    boxes: list[list[int]] = []
    for contour in contours:
        x, y, box_width, box_height = cv2.boundingRect(contour)
        if box_width < 12 or box_height < 6:
            continue
        aspect = box_width / float(box_height)
        area_ratio = box_width * box_height / float(total)
        if not 1.35 <= aspect <= 7.5 or not 0.0008 <= area_ratio <= 0.45:
            continue
        contour_area = cv2.contourArea(contour)
        if contour_area / float(max(1, box_width * box_height)) < 0.12:
            continue
        boxes.append([x, y, x + box_width, y + box_height])
    return boxes


def geometry_candidates(bgr: np.ndarray) -> list[list[int]]:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)
    median = float(np.median(gray))
    lower = int(max(20, 0.66 * median))
    upper = int(min(255, max(lower + 20, 1.33 * median)))
    edges = cv2.Canny(gray, lower, upper)
    horizontal = cv2.getStructuringElement(cv2.MORPH_RECT, (9, 3))
    closed = cv2.morphologyEx(edges, cv2.MORPH_CLOSE, horizontal, iterations=2)
    return _contour_boxes(closed, bgr.shape)


def character_edge_candidates(bgr: np.ndarray) -> list[list[int]]:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    blackhat_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (13, 5))
    blackhat = cv2.morphologyEx(gray, cv2.MORPH_BLACKHAT, blackhat_kernel)
    gradient = cv2.Sobel(blackhat, cv2.CV_32F, 1, 0, ksize=-1)
    gradient = np.absolute(gradient)
    maximum = float(gradient.max())
    if maximum > 0:
        gradient = (255 * gradient / maximum).astype(np.uint8)
    else:
        gradient = np.zeros_like(gray)
    gradient = cv2.morphologyEx(
        gradient,
        cv2.MORPH_CLOSE,
        cv2.getStructuringElement(cv2.MORPH_RECT, (17, 3)),
    )
    _, threshold = cv2.threshold(gradient, 0, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)
    threshold = cv2.morphologyEx(
        threshold,
        cv2.MORPH_CLOSE,
        cv2.getStructuringElement(cv2.MORPH_RECT, (9, 3)),
        iterations=2,
    )
    return _contour_boxes(threshold, bgr.shape)


def character_component_group_candidates(bgr: np.ndarray) -> list[list[int]]:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    _, otsu = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
    adaptive = cv2.adaptiveThreshold(
        gray,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV,
        21,
        7,
    )
    image_height, image_width = gray.shape
    groups: list[list[int]] = []
    for binary in (otsu, adaptive):
        binary = cv2.morphologyEx(
            binary,
            cv2.MORPH_OPEN,
            cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2)),
        )
        contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        components: list[list[int]] = []
        for contour in contours:
            x, y, width, height = cv2.boundingRect(contour)
            if not 0.07 * image_height <= height <= 0.70 * image_height:
                continue
            if not 1 <= width <= 0.28 * image_width:
                continue
            aspect = width / float(max(1, height))
            fill = cv2.contourArea(contour) / float(max(1, width * height))
            if not 0.08 <= aspect <= 1.60 or fill < 0.06:
                continue
            components.append([x, y, x + width, y + height])
        for seed in components:
            seed_height = max(1, seed[3] - seed[1])
            seed_center_y = (seed[1] + seed[3]) / 2.0
            compatible = []
            for item in components:
                item_height = max(1, item[3] - item[1])
                item_center_y = (item[1] + item[3]) / 2.0
                height_ratio = item_height / float(seed_height)
                if 0.35 <= height_ratio <= 2.85 and abs(item_center_y - seed_center_y) <= 0.75 * max(seed_height, item_height):
                    compatible.append(item)
            compatible.sort(key=lambda box: box[0])
            if len(compatible) < 3:
                continue
            union = [
                min(box[0] for box in compatible),
                min(box[1] for box in compatible),
                max(box[2] for box in compatible),
                max(box[3] for box in compatible),
            ]
            aspect = (union[2] - union[0]) / float(max(1, union[3] - union[1]))
            if 1.2 <= aspect <= 8.0:
                groups.append(union)
    unique: list[list[int]] = []
    for box in sorted(groups):
        if any(intersection_over_union(box, existing) >= 0.85 for existing in unique):
            continue
        unique.append(box)
    return unique

def _clip_box(box: list[int], width: int, height: int) -> list[int]:
    return [
        max(0, min(width, int(box[0]))),
        max(0, min(height, int(box[1]))),
        max(0, min(width, int(box[2]))),
        max(0, min(height, int(box[3]))),
    ]


def expand_box(
    box: list[int],
    image_shape: tuple[int, ...],
    horizontal_ratio: float,
    vertical_ratio: float,
) -> list[int]:
    height, width = image_shape[:2]
    box_width, box_height = box[2] - box[0], box[3] - box[1]
    dx = int(round(box_width * horizontal_ratio))
    dy = int(round(box_height * vertical_ratio))
    return _clip_box(
        [box[0] - dx, box[1] - dy, box[2] + dx, box[3] + dy], width, height
    )


def expand_to_aspect(
    box: list[int],
    image_shape: tuple[int, ...],
    target_aspect: float = 3.43,
    vertical_ratio: float = 0.20,
) -> list[int]:
    height, width = image_shape[:2]
    vertically_padded = expand_box(box, image_shape, 0.0, vertical_ratio)
    box_width = vertically_padded[2] - vertically_padded[0]
    box_height = max(1, vertically_padded[3] - vertically_padded[1])
    target_width = max(box_width, int(round(target_aspect * box_height)))
    extra = target_width - box_width
    left_extra = extra // 2
    right_extra = extra - left_extra
    return _clip_box(
        [
            vertically_padded[0] - left_extra,
            vertically_padded[1],
            vertically_padded[2] + right_extra,
            vertically_padded[3],
        ],
        width,
        height,
    )

def merge_aligned_boxes(boxes: list[list[int]]) -> list[list[int]]:
    merged: list[list[int]] = []
    ordered = sorted(boxes, key=lambda box: (box[0], box[1]))
    for index, left in enumerate(ordered):
        left_height = max(1, left[3] - left[1])
        for right in ordered[index + 1 :]:
            right_height = max(1, right[3] - right[1])
            overlap = max(0, min(left[3], right[3]) - max(left[1], right[1]))
            vertical_overlap = overlap / float(min(left_height, right_height))
            gap = max(0, right[0] - left[2])
            if vertical_overlap < 0.45 or gap > 3 * max(left_height, right_height):
                continue
            merged.append(
                [
                    min(left[0], right[0]),
                    min(left[1], right[1]),
                    max(left[2], right[2]),
                    max(left[3], right[3]),
                ]
            )
    return merged

def candidate_score(bgr: np.ndarray, box: list[int], color_mask: np.ndarray, origin: str) -> float:
    height, width = bgr.shape[:2]
    x1, y1, x2, y2 = _clip_box(box, width, height)
    box_width, box_height = x2 - x1, y2 - y1
    if box_width <= 0 or box_height <= 0:
        return 0.0
    roi = bgr[y1:y2, x1:x2]
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    aspect = box_width / float(box_height)
    aspect_score = max(0.0, 1.0 - abs(aspect - 4.0) / 4.0)
    area_ratio = box_width * box_height / float(max(1, width * height))
    area_score = min(1.0, area_ratio / 0.05)
    edge_density = float(np.mean(cv2.Canny(gray, 60, 160) > 0))
    edge_score = max(0.0, 1.0 - abs(edge_density - 0.18) / 0.18)
    _, binary = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
    count, _, stats, _ = cv2.connectedComponentsWithStats(binary, 8)
    component_count = 0
    for index in range(1, count):
        _, _, cw, ch, area = stats[index]
        if 0.22 * box_height <= ch <= 0.95 * box_height and area >= 3 and cw <= 0.35 * box_width:
            component_count += 1
    component_score = min(1.0, component_count / 5.0) if component_count <= 12 else 0.5
    color_ratio = float(np.mean(color_mask[y1:y2, x1:x2] > 0))
    origin_bonus = 0.07 if origin == "character_group" else 0.06 if origin == "character_edge" else 0.03 if origin == "geometry" else 0.0
    return float(
        0.29 * aspect_score
        + 0.16 * area_score
        + 0.24 * edge_score
        + 0.23 * component_score
        + 0.08 * min(1.0, color_ratio / 0.25)
        + origin_bonus
    )


def select_ranked_candidates(
    candidates: list[dict], limit: int, reserve_coarse: bool
) -> list[dict]:
    if not reserve_coarse or len(candidates) <= limit:
        return candidates[:limit]
    selected = list(candidates[: max(0, limit - 1)])
    coarse = max(
        candidates,
        key=lambda item: (item["bbox"][2] - item["bbox"][0])
        * (item["bbox"][3] - item["bbox"][1]),
    )
    if coarse not in selected:
        selected.append(coarse)
    for item in candidates:
        if len(selected) >= limit:
            break
        if item not in selected:
            selected.append(item)
    return selected[:limit]

def ranked_candidates(
    bgr: np.ndarray,
    thresholds: dict,
    origins: tuple[str, ...],
    limit: int = 3,
    corrected: bool = False,
    adaptive: bool = False,
    component_group: bool = False,
    reserve_coarse: bool = False,
) -> list[dict]:
    color_mask, _ = build_color_mask(bgr, thresholds)
    raw: list[tuple[list[int], str]] = []
    if "color" in origins:
        color_regions, _, _ = detect_candidate_regions(bgr, thresholds)
        color_boxes = [xywh_to_xyxy(region.bbox) for region in color_regions]
        if corrected or adaptive:
            color_boxes = [expand_box(box, bgr.shape, 0.08, 0.10) for box in color_boxes]
        raw.extend((box, "color") for box in color_boxes)
    if "geometry" in origins:
        geometry_boxes = geometry_candidates(bgr)
        if adaptive:
            geometry_boxes = [
                expand_to_aspect(box, bgr.shape, 3.43, 0.12) for box in geometry_boxes
            ]
        elif corrected:
            geometry_boxes = [
                expand_box(box, bgr.shape, 0.20, 0.14) for box in geometry_boxes
            ]
        raw.extend((box, "geometry") for box in geometry_boxes)
    if "character_edge" in origins:
        character_boxes = character_edge_candidates(bgr)
        if corrected or adaptive:
            character_boxes = character_boxes + merge_aligned_boxes(character_boxes)
        if adaptive:
            character_boxes = [
                expand_to_aspect(box, bgr.shape, 3.43, 0.20) for box in character_boxes
            ]
        elif corrected:
            character_boxes = [
                expand_box(box, bgr.shape, 0.45, 0.22) for box in character_boxes
            ]
        raw.extend((box, "character_edge") for box in character_boxes)
    if component_group:
        grouped_boxes = [
            expand_to_aspect(box, bgr.shape, 3.43, 0.18)
            for box in character_component_group_candidates(bgr)
        ]
        raw.extend((box, "character_group") for box in grouped_boxes)
    scored = [
        {"bbox": box, "origin": origin, "score": candidate_score(bgr, box, color_mask, origin)}
        for box, origin in raw
    ]
    scored.sort(key=lambda item: (-item["score"], item["bbox"]))
    kept: list[dict] = []
    for item in scored:
        if any(intersection_over_union(item["bbox"], other["bbox"]) >= 0.55 for other in kept):
            continue
        kept.append(item)
        if len(kept) >= (20 if reserve_coarse else limit):
            break
    return select_ranked_candidates(kept, limit, reserve_coarse)


def current_color_candidate(bgr: np.ndarray, thresholds: dict) -> list[dict]:
    regions, _, _ = detect_candidate_regions(bgr, thresholds)
    if not regions:
        return []
    region = regions[0]
    return [{"bbox": xywh_to_xyxy(region.bbox), "origin": "color", "score": region.score}]


def pipeline_candidates(bgr: np.ndarray, thresholds: dict, pipeline: str) -> list[dict]:
    if pipeline == "current_color_top1":
        return current_color_candidate(bgr, thresholds)
    origin_map = {
        "geometry_only": ("geometry",),
        "character_edge_only": ("character_edge",),
        "color_plus_geometry": ("color", "geometry"),
        "combined_top3": ("color", "geometry", "character_edge"),
        "combined_corrected_top3": ("color", "geometry", "character_edge"),
        "combined_adaptive_top3": ("color", "geometry", "character_edge"),
        "combined_component_group_top3": ("color", "geometry", "character_edge"),
        "combined_coarse_diverse_top3": ("color", "geometry", "character_edge"),
    }
    return ranked_candidates(
        bgr,
        thresholds,
        origin_map[pipeline],
        limit=3,
        corrected=pipeline == "combined_corrected_top3",
        adaptive=pipeline in {"combined_adaptive_top3", "combined_component_group_top3", "combined_coarse_diverse_top3"},
        component_group=pipeline in {"combined_component_group_top3", "combined_coarse_diverse_top3"},
        reserve_coarse=pipeline == "combined_coarse_diverse_top3",
    )


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(quantile * len(ordered)) - 1))
    return float(ordered[index])


def evaluate(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve()
    config_path = Path(args.config).resolve()
    output_dir = Path(args.output_dir).resolve()
    rows = read_csv(manifest_path)
    thresholds = json.loads(config_path.read_text(encoding="utf-8-sig"))
    details: list[dict] = []
    errors: list[dict] = []
    for row in rows:
        gt_box = ground_truth_box(row)
        source = Path(row["source_path"])
        bgr = read_image(source)
        if bgr is None or not gt_box:
            errors.append(
                {
                    "annotation_id": row.get("annotation_id", ""),
                    "reason": "image_read_failed" if bgr is None else "ground_truth_roi_missing",
                }
            )
            continue
        for pipeline in PIPELINES:
            started = time.perf_counter()
            try:
                candidates = pipeline_candidates(bgr, thresholds, pipeline)
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                ious = [intersection_over_union(item["bbox"], gt_box) for item in candidates]
                best_top1 = ious[0] if ious else 0.0
                best_top3 = max(ious, default=0.0)
                details.append(
                    {
                        "annotation_id": row.get("annotation_id", ""),
                        "source_path": str(source),
                        "ground_truth": row.get("ground_truth", ""),
                        "sample_category": row.get("sample_category", ""),
                        "plate_status": row.get("plate_status", ""),
                        "pipeline": pipeline,
                        "gt_bbox": json.dumps(gt_box),
                        "candidate_count": len(candidates),
                        "top1_iou": f"{best_top1:.6f}",
                        "top3_iou": f"{best_top3:.6f}",
                        "top1_hit": int(best_top1 >= args.iou_threshold),
                        "top3_hit": int(best_top3 >= args.iou_threshold),
                        "candidates": json.dumps(candidates, ensure_ascii=False),
                        "processing_ms": f"{elapsed_ms:.3f}",
                        "error_reason": "",
                    }
                )
            except Exception as exc:  # keep row-level audit instead of aborting the batch
                errors.append(
                    {
                        "annotation_id": row.get("annotation_id", ""),
                        "pipeline": pipeline,
                        "reason": f"{type(exc).__name__}:{exc}",
                    }
                )

    fieldnames = list(details[0].keys()) if details else ["annotation_id"]
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in ("manifest.csv", "details.csv"):
        with (output_dir / name).open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(details)

    summaries: list[dict] = []
    grouped: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for detail in details:
        grouped[(detail["pipeline"], detail["ground_truth"])].append(detail)
        grouped[(detail["pipeline"], "ALL")].append(detail)
    for (pipeline, ground_truth), group in sorted(grouped.items()):
        times = [float(item["processing_ms"]) for item in group]
        summaries.append(
            {
                "pipeline": pipeline,
                "ground_truth": ground_truth,
                "count": len(group),
                "top1_recall": sum(int(item["top1_hit"]) for item in group) / len(group),
                "top3_recall": sum(int(item["top3_hit"]) for item in group) / len(group),
                "miss_rate": 1.0 - sum(int(item["top3_hit"]) for item in group) / len(group),
                "mean_candidate_count": statistics.fmean(int(item["candidate_count"]) for item in group),
                "mean_ms": statistics.fmean(times),
                "median_ms": statistics.median(times),
                "p95_ms": percentile(times, 0.95),
            }
        )
    summary_fields = [
        "pipeline",
        "ground_truth",
        "count",
        "top1_recall",
        "top3_recall",
        "miss_rate",
        "mean_candidate_count",
        "mean_ms",
        "median_ms",
        "p95_ms",
    ]
    with (output_dir / "environment_summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=summary_fields)
        writer.writeheader()
        writer.writerows(summaries)

    metric_map = {
        f"{item['pipeline']}:{item['ground_truth']}": item for item in summaries
    }
    gate_ev = metric_map.get(f"{args.gate_pipeline}:EV", {})
    baseline_ev = metric_map.get(f"{args.baseline_pipeline}:EV", {})
    pilot_gate = {
        "ev_top3_recall_at_least_0_95": gate_ev.get("top3_recall", 0.0) >= 0.95,
        "processing_errors_zero": len(errors) == 0,
        "candidate_not_below_baseline": gate_ev.get("top3_recall", 0.0)
        >= baseline_ev.get("top3_recall", 0.0),
    }
    summary = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "pilot_passed" if all(pilot_gate.values()) else "pilot_failed",
        "input_rows": len(rows),
        "evaluated_detail_rows": len(details),
        "iou_threshold": args.iou_threshold,
        "errors": errors,
        "error_count": len(errors),
        "metrics": metric_map,
        "pilot_gate": pilot_gate,
        "gate_pipeline": args.gate_pipeline,
        "baseline_pipeline": args.baseline_pipeline,
        "formal_step2_gate_passed": False,
        "independent_test": False,
        "performance_claim": False,
        "source_manifest": str(manifest_path),
        "config": str(config_path),
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if not errors else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser("Evaluate classical Top-3 plate localization")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--iou-threshold", type=float, default=0.5)
    parser.add_argument(
        "--experiment-id", default="ev-roi-icon-002-step02-top3-pilot107"
    )
    parser.add_argument("--gate-pipeline", choices=PIPELINES, default="combined_top3")
    parser.add_argument("--baseline-pipeline", choices=PIPELINES, default="current_color_top1")
    return parser


def main() -> int:
    return evaluate(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())


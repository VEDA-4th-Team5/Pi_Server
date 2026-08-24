from __future__ import annotations

import numpy as np

from .icon_template_bank import IconTemplate, SIDE_REGIONS, bbox_descriptor, median_bbox, robust_bbox_descriptor


def detect_icon(image: np.ndarray, side: str, templates: list[IconTemplate]) -> dict:
    if not templates:
        return {"score": 0.0, "bbox": [], "template_count": 0, "matched_annotation_id": ""}
    predicted_bbox = median_bbox(templates, side)
    vector = bbox_descriptor(image, predicted_bbox)
    scores = [float(np.dot(vector, item.descriptor)) for item in templates]
    index = int(np.argmax(scores))
    return {
        "score": scores[index],
        "bbox": predicted_bbox,
        "template_count": len(templates),
        "matched_annotation_id": templates[index].annotation_id,
    }


def _search_boxes(base: list[int], side: str) -> list[list[int]]:
    x1, y1, x2, y2 = base
    center_x = (x1 + x2) / 2.0
    center_y = (y1 + y2) / 2.0
    width = max(2.0, float(x2 - x1))
    height = max(2.0, float(y2 - y1))
    region_x1, region_y1, region_x2, region_y2 = SIDE_REGIONS[side]
    boxes: list[list[int]] = []
    for scale in (0.90, 1.0, 1.10):
        scaled_width = width * scale
        scaled_height = height * scale
        for dx, dy in ((0, 0), (-4, 0), (4, 0), (0, -3), (0, 3)):
            left = max(region_x1, int(round(center_x + dx - scaled_width / 2.0)))
            top = max(region_y1, int(round(center_y + dy - scaled_height / 2.0)))
            right = min(region_x2, int(round(center_x + dx + scaled_width / 2.0)))
            bottom = min(region_y2, int(round(center_y + dy + scaled_height / 2.0)))
            box = [left, top, right, bottom]
            if right > left and bottom > top and box not in boxes:
                boxes.append(box)
    return boxes


def detect_icon_robust(image: np.ndarray, side: str, templates: list[IconTemplate]) -> dict:
    """Position-prior search using descriptors that retain signal after blur or dimming."""
    robust_templates = [item for item in templates if item.robust_descriptor is not None]
    if not robust_templates:
        return {"score": 0.0, "bbox": [], "template_count": 0, "matched_annotation_id": ""}
    predicted_bbox = median_bbox(robust_templates, side)
    best_score = -1.0
    best_bbox: list[int] = predicted_bbox
    best_template = robust_templates[0]
    for box in _search_boxes(predicted_bbox, side):
        vector = robust_bbox_descriptor(image, box)
        scores = [float(np.dot(vector, item.robust_descriptor)) for item in robust_templates]
        index = int(np.argmax(scores))
        if scores[index] > best_score:
            best_score = scores[index]
            best_bbox = box
            best_template = robust_templates[index]
    return {
        "score": max(0.0, best_score),
        "bbox": best_bbox,
        "template_count": len(robust_templates),
        "matched_annotation_id": best_template.annotation_id,
    }


def _contrastive_search_boxes(base: list[int], side: str) -> list[list[int]]:
    x1, y1, x2, y2 = base
    center_x = (x1 + x2) / 2.0
    center_y = (y1 + y2) / 2.0
    width = max(2.0, float(x2 - x1))
    height = max(2.0, float(y2 - y1))
    region_x1, region_y1, region_x2, region_y2 = SIDE_REGIONS[side]
    boxes: list[list[int]] = []
    for scale in (0.90, 1.0, 1.10):
        scaled_width = width * scale
        scaled_height = height * scale
        for dx in (-8, 0, 8):
            for dy in (-6, 0, 6):
                left = max(region_x1, int(round(center_x + dx - scaled_width / 2.0)))
                top = max(region_y1, int(round(center_y + dy - scaled_height / 2.0)))
                right = min(region_x2, int(round(center_x + dx + scaled_width / 2.0)))
                bottom = min(region_y2, int(round(center_y + dy + scaled_height / 2.0)))
                box = [left, top, right, bottom]
                if right > left and bottom > top and box not in boxes:
                    boxes.append(box)
    return boxes


def detect_icon_contrastive(
    image: np.ndarray,
    side: str,
    positive_templates: list[IconTemplate],
    negative_templates: list[IconTemplate],
    edge_weight: float = 0.70,
) -> dict:
    """Score icon evidence against both present and human-confirmed absent templates."""
    if not positive_templates or not negative_templates:
        return {
            "score": -1.0,
            "bbox": [],
            "positive_edge_similarity": 0.0,
            "negative_edge_similarity": 0.0,
            "positive_robust_similarity": 0.0,
            "negative_robust_similarity": 0.0,
        }
    base = median_bbox(positive_templates, side)
    best: dict | None = None
    robust_positives = [item for item in positive_templates if item.robust_descriptor is not None]
    robust_negatives = [item for item in negative_templates if item.robust_descriptor is not None]
    positive_edge_matrix = np.vstack([item.descriptor for item in positive_templates])
    negative_edge_matrix = np.vstack([item.descriptor for item in negative_templates])
    positive_robust_matrix = np.vstack([item.robust_descriptor for item in robust_positives])
    negative_robust_matrix = np.vstack([item.robust_descriptor for item in robust_negatives])
    for box in _contrastive_search_boxes(base, side):
        edge_vector = bbox_descriptor(image, box)
        robust_vector = robust_bbox_descriptor(image, box)
        positive_edge = float(np.max(positive_edge_matrix @ edge_vector))
        negative_edge = float(np.max(negative_edge_matrix @ edge_vector))
        positive_robust = float(np.max(positive_robust_matrix @ robust_vector))
        negative_robust = float(np.max(negative_robust_matrix @ robust_vector))
        edge_margin = positive_edge - negative_edge
        robust_margin = positive_robust - negative_robust
        score = float(edge_weight * edge_margin + (1.0 - edge_weight) * robust_margin)
        candidate = {
            "score": score,
            "bbox": box,
            "positive_edge_similarity": positive_edge,
            "negative_edge_similarity": negative_edge,
            "positive_robust_similarity": positive_robust,
            "negative_robust_similarity": negative_robust,
        }
        if best is None or candidate["score"] > best["score"]:
            best = candidate
    return best or {
        "score": -1.0,
        "bbox": [],
        "positive_edge_similarity": 0.0,
        "negative_edge_similarity": 0.0,
        "positive_robust_similarity": 0.0,
        "negative_robust_similarity": 0.0,
    }

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Tuple

import cv2
import numpy as np


GEOMETRIC_METHODS = frozenset({"perspective", "rotated_rect"})


@dataclass
class PlateRectificationResult:
    status: str
    method: str
    roi_bbox: List[int]
    plate_image: np.ndarray
    rect_quality_input: float


def _pad_box(x: int, y: int, w: int, h: int, H: int, W: int, pad_ratio: float) -> Tuple[int, int, int, int]:
    pad_x = int(w * pad_ratio)
    pad_y = int(h * pad_ratio)
    x0 = max(0, x - pad_x)
    y0 = max(0, y - pad_y)
    x1 = min(W, x + w + pad_x)
    y1 = min(H, y + h + pad_y)
    return x0, y0, max(1, x1 - x0), max(1, y1 - y0)


def order_quad(points: np.ndarray) -> np.ndarray:
    """Return quadrilateral corners in TL, TR, BR, BL order."""
    points = np.asarray(points, dtype=np.float32).reshape(4, 2)
    sums = points.sum(axis=1)
    differences = np.diff(points, axis=1).reshape(-1)
    return np.array(
        [
            points[np.argmin(sums)],
            points[np.argmin(differences)],
            points[np.argmax(sums)],
            points[np.argmax(differences)],
        ],
        dtype=np.float32,
    )


def _find_quad_legacy(points: list[np.ndarray]) -> list[tuple[int, int]] | None:
    """Original largest-quad implementation retained for baseline comparison."""
    if not points:
        return None
    best = None
    best_area = 0.0
    for contour in points:
        area = cv2.contourArea(contour)
        if area < 150.0:
            continue
        perimeter = cv2.arcLength(contour, True)
        approx = cv2.approxPolyDP(contour, 0.04 * perimeter, True)
        if len(approx) != 4 or not cv2.isContourConvex(approx):
            continue
        if area > best_area:
            best_area = area
            best = approx[:, 0, :].astype(np.float32)
    if best is None:
        return None
    return [(int(x), int(y)) for x, y in best]


def _quad_geometry(quad: np.ndarray, shape: tuple[int, ...]) -> tuple[float, float, float]:
    ordered = order_quad(quad)
    top = float(np.linalg.norm(ordered[1] - ordered[0]))
    right = float(np.linalg.norm(ordered[2] - ordered[1]))
    bottom = float(np.linalg.norm(ordered[2] - ordered[3]))
    left = float(np.linalg.norm(ordered[3] - ordered[0]))
    first_axis = (top + bottom) / 2.0
    second_axis = (left + right) / 2.0
    aspect = max(first_axis, second_axis) / max(1.0, min(first_axis, second_axis))
    height, width = shape[:2]
    area = abs(float(cv2.contourArea(ordered)))
    area_ratio = area / float(max(1, height * width))
    min_rect = cv2.minAreaRect(ordered)
    rect_area = float(min_rect[1][0] * min_rect[1][1])
    rectangularity = min(1.0, area / max(1.0, rect_area))
    return aspect, area_ratio, rectangularity


def _edge_maps(roi: np.ndarray) -> list[np.ndarray]:
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (5, 5), 0)
    current = cv2.Canny(blurred, 40, 140)

    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    denoised = cv2.bilateralFilter(clahe, 5, 35, 35)
    median = float(np.median(denoised))
    lower = int(max(12, 0.45 * median))
    upper = int(min(255, max(lower + 24, 1.55 * median)))
    contrast = cv2.Canny(denoised, lower, upper)

    gradient = cv2.morphologyEx(
        denoised,
        cv2.MORPH_GRADIENT,
        cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3)),
    )
    _, gradient = cv2.threshold(gradient, 0, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)

    close_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (7, 3))
    return [
        cv2.morphologyEx(edge_map, cv2.MORPH_CLOSE, close_kernel, iterations=2)
        for edge_map in (current, contrast, gradient)
    ]


def _find_quad_quality_aware(roi: np.ndarray) -> tuple[np.ndarray | None, float]:
    best: np.ndarray | None = None
    best_quality = 0.0
    for edge_map in _edge_maps(roi):
        contours, _ = cv2.findContours(edge_map, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
        for contour in contours:
            perimeter = cv2.arcLength(contour, True)
            if perimeter <= 0:
                continue
            for epsilon in (0.02, 0.03, 0.04):
                approx = cv2.approxPolyDP(contour, epsilon * perimeter, True)
                if len(approx) != 4 or not cv2.isContourConvex(approx):
                    continue
                quad = approx[:, 0, :].astype(np.float32)
                aspect, area_ratio, rectangularity = _quad_geometry(quad, roi.shape)
                if not 1.4 <= aspect <= 8.5 or not 0.08 <= area_ratio <= 0.98:
                    continue
                aspect_score = max(0.0, 1.0 - abs(aspect - 4.0) / 4.0)
                quality = (
                    0.55 * min(1.0, area_ratio / 0.65)
                    + 0.25 * aspect_score
                    + 0.20 * rectangularity
                )
                if quality > best_quality:
                    best = order_quad(quad)
                    best_quality = float(quality)
                break
    return best, best_quality


def _find_rotated_quad(roi: np.ndarray) -> tuple[np.ndarray | None, float]:
    combined = np.maximum.reduce(_edge_maps(roi))
    combined = cv2.morphologyEx(
        combined,
        cv2.MORPH_CLOSE,
        cv2.getStructuringElement(cv2.MORPH_RECT, (13, 5)),
        iterations=2,
    )
    contours, _ = cv2.findContours(combined, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    best: np.ndarray | None = None
    best_quality = 0.0
    for contour in contours:
        if len(contour) < 4:
            continue
        box = cv2.boxPoints(cv2.minAreaRect(contour)).astype(np.float32)
        aspect, area_ratio, rectangularity = _quad_geometry(box, roi.shape)
        if not 1.4 <= aspect <= 8.5 or not 0.10 <= area_ratio <= 0.98:
            continue
        fill = abs(float(cv2.contourArea(contour))) / max(1.0, abs(float(cv2.contourArea(box))))
        aspect_score = max(0.0, 1.0 - abs(aspect - 4.0) / 4.0)
        quality = (
            0.45 * min(1.0, area_ratio / 0.65)
            + 0.20 * aspect_score
            + 0.15 * rectangularity
            + 0.20 * min(1.0, fill)
        )
        if quality > best_quality:
            best = order_quad(box)
            best_quality = float(quality)
    return best, best_quality


def _warp_quad(roi: np.ndarray, quad: np.ndarray, target_size: tuple[int, int]) -> np.ndarray:
    source = order_quad(quad)
    destination = np.array(
        [
            [0.0, 0.0],
            [target_size[0] - 1.0, 0.0],
            [target_size[0] - 1.0, target_size[1] - 1.0],
            [0.0, target_size[1] - 1.0],
        ],
        dtype=np.float32,
    )
    transform = cv2.getPerspectiveTransform(source, destination)
    return cv2.warpPerspective(
        roi,
        transform,
        target_size,
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_REPLICATE,
    )


def rectify_plate_legacy(roi: np.ndarray, target_size: tuple[int, int] = (440, 100)) -> PlateRectificationResult:
    """Frozen pre-Step-89 implementation used only for reproducible comparison."""
    if roi is None or roi.size == 0:
        raise ValueError("empty roi for rectification")
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (5, 5), 0)
    edges = cv2.Canny(blurred, 40, 140)
    contours, _ = cv2.findContours(edges, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    quad = _find_quad_legacy(contours)
    height, width = roi.shape[:2]
    if quad is None:
        resized = cv2.resize(roi, target_size, interpolation=cv2.INTER_LINEAR)
        return PlateRectificationResult(
            "fallback_crop_normalized", "crop_normalized", [0, 0, width, height], resized, 0.0
        )
    source = np.array(quad, dtype=np.float32)
    destination = np.array(
        [[0.0, 0.0], [target_size[0] - 1.0, 0.0], [target_size[0] - 1.0, target_size[1] - 1.0], [0.0, target_size[1] - 1.0]],
        dtype=np.float32,
    )
    transform = cv2.getPerspectiveTransform(source, destination)
    warped = cv2.warpPerspective(roi, transform, target_size, flags=cv2.INTER_LINEAR)
    return PlateRectificationResult(
        "ok", "perspective", [0, 0, target_size[0], target_size[1]], warped, float(np.mean(edges > 0))
    )


def rectify_plate(roi: np.ndarray, target_size: tuple[int, int] = (440, 100)) -> PlateRectificationResult:
    """Quality-aware rectification with ordered corners and a safe geometric fallback."""
    if roi is None or roi.size == 0:
        raise ValueError("empty roi for rectification")
    height, width = roi.shape[:2]
    quad, quality = _find_quad_quality_aware(roi)
    method = "perspective"
    if quad is None:
        quad, quality = _find_rotated_quad(roi)
        method = "rotated_rect"
    if quad is None:
        resized = cv2.resize(roi, target_size, interpolation=cv2.INTER_CUBIC)
        return PlateRectificationResult(
            "fallback_crop_normalized", "crop_normalized", [0, 0, width, height], resized, 0.0
        )
    warped = _warp_quad(roi, quad, target_size)
    return PlateRectificationResult(
        "ok", method, [0, 0, target_size[0], target_size[1]], warped, quality
    )


def crop_plate_candidate(
    bgr: np.ndarray,
    candidate_bbox: List[int] | None,
    pad_ratio: float = 0.15,
) -> np.ndarray:
    if candidate_bbox is None or len(candidate_bbox) != 4:
        return bgr
    x, y, w, h = candidate_bbox
    height, width = bgr.shape[:2]
    x, y, w, h = _pad_box(x, y, w, h, height, width, pad_ratio)
    return bgr[y : y + h, x : x + w]

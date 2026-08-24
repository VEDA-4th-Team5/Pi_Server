from __future__ import annotations

from typing import Dict

import cv2
import numpy as np


def _blue_hsv_mask(hsv: np.ndarray) -> np.ndarray:
    lower = np.array([90, 35, 40], dtype=np.uint8)
    upper = np.array([126, 255, 255], dtype=np.uint8)
    return cv2.inRange(hsv, lower, upper)


def _blue_lab_mask(lab: np.ndarray) -> np.ndarray:
    L, A, B = cv2.split(lab)
    return cv2.inRange(np.stack([A, B], axis=-1), np.array([30, 20], dtype=np.uint8), np.array([145, 180], dtype=np.uint8))


def central_background_ratio(hsv: np.ndarray, roi_x: int, roi_y: int, roi_w: int, roi_h: int) -> float:
    H, W = hsv.shape[:2]
    lx = max(0, int(roi_x + 0.15 * roi_w))
    rx = min(W, int(roi_x + 0.85 * roi_w))
    ly = max(0, int(roi_y + 0.20 * roi_h))
    ry = min(H, int(roi_y + 0.80 * roi_h))
    if rx <= lx or ry <= ly:
        return 0.0
    center = hsv[ly:ry, lx:rx]
    if center.size == 0:
        return 0.0
    mask = _blue_hsv_mask(center)
    return float(np.count_nonzero(mask) / float(mask.size))


def compute_background_scores(
    plate_img: np.ndarray,
    roi_bbox: list[int] | None = None,
) -> Dict[str, float]:
    if plate_img is None or plate_img.size == 0:
        return {
            "blue_ratio": 0.0,
            "lab_score": 0.0,
            "contrast": 0.0,
        }

    hsv = cv2.cvtColor(plate_img, cv2.COLOR_BGR2HSV)
    lab = cv2.cvtColor(plate_img, cv2.COLOR_BGR2Lab)

    H, W = hsv.shape[:2]
    if roi_bbox is None:
        roi_bbox = [0, 0, W, H]
    bx = central_background_ratio(hsv, *roi_bbox)

    lab_mask = _blue_lab_mask(lab)
    lab_ratio = float(np.mean(lab_mask > 0))
    gray = cv2.cvtColor(plate_img, cv2.COLOR_BGR2GRAY)
    contrast = float(gray.std() / 255.0)

    return {
        "blue_ratio": float(bx),
        "lab_score": float(lab_ratio),
        "contrast": float(max(0.0, min(1.0, contrast))),
    }

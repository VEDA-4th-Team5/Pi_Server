from __future__ import annotations

from typing import Dict, Tuple

import cv2
import numpy as np


def _icon_roi(gray_plate: np.ndarray, side: str) -> np.ndarray:
    H, W = gray_plate.shape[:2]
    if side == "left":
        x0, x1 = int(0.02 * W), int(0.30 * W)
    else:
        x0, x1 = int(0.70 * W), int(0.98 * W)
    y0 = int(0.15 * H)
    y1 = int(0.90 * H)
    return gray_plate[y0:y1, x0:x1]


def _edge_density(gray_roi: np.ndarray) -> float:
    edges = cv2.Canny(gray_roi, 40, 120)
    return float(np.mean(edges > 0))


def _vertical_symmetry(gray_roi: np.ndarray) -> float:
    h, w = gray_roi.shape[:2]
    if w < 2:
        return 0.0
    left = gray_roi[:, : w // 2]
    right = gray_roi[:, w // 2 :]
    if right.shape[1] == 0:
        return 0.0
    rh = min(right.shape[1], left.shape[1])
    if rh <= 0:
        return 0.0
    left = left[:, :rh]
    right = cv2.flip(right[:, :rh], 1)
    diff = np.abs(left.astype(np.float32) - right.astype(np.float32))
    return float(1.0 - np.mean(diff) / 255.0)


def analyze_icon(gray_plate: np.ndarray, side: str) -> Tuple[float, str, Dict[str, float]]:
    roi = _icon_roi(gray_plate, side)
    if roi.size == 0:
        return 0.0, "missing", {"edge": 0.0, "sym": 0.0, "density": 0.0}
    density = _edge_density(roi)
    sym = _vertical_symmetry(roi)
    density_n = min(1.0, density * 1.8)
    score = float(0.6 * density_n + 0.4 * sym)
    if score >= 0.62:
        status = "strong"
    elif score >= 0.32:
        status = "weak"
    else:
        status = "missing"
    return score, status, {"edge": density, "sym": sym, "density": float(density)}

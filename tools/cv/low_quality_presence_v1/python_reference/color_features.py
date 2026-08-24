from __future__ import annotations

from typing import Dict, Tuple

import cv2
import numpy as np


def _to_numpy_mask(pair: Tuple[int, int, int], pair2: Tuple[int, int, int]) -> Tuple[np.ndarray, np.ndarray]:
    low = np.array(pair, dtype=np.uint8)
    high = np.array(pair2, dtype=np.uint8)
    return low, high


def build_color_mask(bgr: np.ndarray, thresholds: Dict) -> Tuple[np.ndarray, Dict[str, float]]:
    hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)

    green_conf = thresholds["hsv"]["green"]
    blue_conf = thresholds["hsv"]["blue"]
    g_low, g_high = _to_numpy_mask(tuple(green_conf["lower"]), tuple(green_conf["upper"]))
    b_low, b_high = _to_numpy_mask(tuple(blue_conf["lower"]), tuple(blue_conf["upper"]))

    green_mask = cv2.inRange(hsv, g_low, g_high)
    blue_mask = cv2.inRange(hsv, b_low, b_high)
    combined = cv2.bitwise_or(green_mask, blue_mask)

    h, w = bgr.shape[:2]
    area = max(1, h * w)
    green_ratio = float(cv2.countNonZero(green_mask)) / float(area)
    blue_ratio = float(cv2.countNonZero(blue_mask)) / float(area)
    global_ratio = float(cv2.countNonZero(combined)) / float(area)

    metrics = {
        "green_ratio": green_ratio,
        "blue_ratio": blue_ratio,
        "global_ratio": global_ratio,
    }
    return combined, metrics


def exposure_flags(bgr: np.ndarray) -> bool:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    dark_ratio = float(np.mean(gray < 40) * 100.0)
    bright_ratio = float(np.mean(gray > 245) * 100.0)
    return (dark_ratio > 75.0) or (bright_ratio > 70.0)

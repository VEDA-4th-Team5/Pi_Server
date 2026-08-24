from __future__ import annotations

from typing import Dict, List

import cv2
import numpy as np

from .types import CandidateRegion


def morph_cleanup(mask: np.ndarray, open_kernel: int, close_kernel: int) -> np.ndarray:
    k_open = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (open_kernel, open_kernel))
    k_close = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_kernel, close_kernel))
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, k_open)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, k_close)
    return mask


def region_score(
    x: int,
    y: int,
    w: int,
    h: int,
    bgr: np.ndarray,
    mask: np.ndarray,
    thresholds: Dict,
) -> float:
    H, W = bgr.shape[:2]
    area = max(1, w * h)
    area_ratio = area / float(W * H)
    aspect = w / float(h)
    area_norm = np.clip(area_ratio / thresholds["region"]["max_area_ratio"], 0.0, 1.0)
    aspect_penalty = 1.0 - min(
        1.0,
        abs(np.clip(aspect, thresholds["region"]["min_aspect_ratio"], thresholds["region"]["max_aspect_ratio"]) - aspect)
        / 6.0,
    )

    roi = bgr[y : y + h, x : x + w]
    hsv = cv2.cvtColor(roi, cv2.COLOR_BGR2HSV)
    sat = float(np.mean(hsv[:, :, 1]) / 255.0)
    bright = float(np.mean(roi[:, :, :]) / 255.0)
    mask_area = float(np.count_nonzero(mask[y : y + h, x : x + w])) / float(area)

    score = (
        thresholds["region"]["area_weight"] * area_norm
        + thresholds["region"]["aspect_weight"] * aspect_penalty
        + thresholds["region"]["saturation_weight"] * sat
        + thresholds["region"]["brightness_weight"] * bright
    )
    score *= 0.7
    score += 0.3 * mask_area
    return float(score), area_ratio


def detect_regions(
    bgr: np.ndarray,
    mask: np.ndarray,
    thresholds: Dict,
) -> List[CandidateRegion]:
    cleaned = morph_cleanup(mask, thresholds["mask"]["open_kernel"], thresholds["mask"]["close_kernel"])
    contours, _ = cv2.findContours(cleaned, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    h, w = bgr.shape[:2]
    total_area = max(1.0, h * w)
    min_area = thresholds["region"]["min_area_ratio"] * total_area
    max_area = thresholds["region"]["max_area_ratio"] * total_area

    results: List[CandidateRegion] = []
    for c in contours:
        x, y, cw, ch = cv2.boundingRect(c)
        area = cw * ch
        if area < min_area or area > max_area:
            continue
        if cw <= 8 or ch <= 4:
            continue

        aspect = cw / float(ch)
        if aspect < thresholds["region"]["min_aspect_ratio"] or aspect > thresholds["region"]["max_aspect_ratio"]:
            continue

        score, area_ratio = region_score(x, y, cw, ch, bgr, cleaned, thresholds)
        results.append(CandidateRegion(x=x, y=y, w=cw, h=ch, area_ratio=area_ratio, score=score))

    results.sort(key=lambda r: r.score, reverse=True)
    return results

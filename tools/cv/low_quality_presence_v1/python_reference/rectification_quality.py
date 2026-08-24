from __future__ import annotations

import cv2
import numpy as np


def _to_gray(image: np.ndarray) -> np.ndarray:
    if image.ndim == 2:
        return image
    return cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)


def laplacian_sharpness(image: np.ndarray) -> float:
    gray = _to_gray(image)
    return float(cv2.Laplacian(gray, cv2.CV_64F).var())


def edge_uniformity(image: np.ndarray) -> float:
    gray = _to_gray(image)
    gx = cv2.Sobel(gray, cv2.CV_16S, 1, 0, ksize=3)
    gy = cv2.Sobel(gray, cv2.CV_16S, 0, 1, ksize=3)
    mag = cv2.convertScaleAbs(np.sqrt(gx.astype(np.float32) ** 2 + gy.astype(np.float32) ** 2))
    if mag.size == 0:
        return 0.0
    mean = float(np.mean(mag))
    if mean <= 0:
        return 0.0
    return float(1.0 / (1.0 + np.std(mag.astype(np.float32)) / mean))


def rectification_quality_score(image: np.ndarray) -> float:
    if image is None or image.size == 0:
        return 0.0
    h_score = laplacian_sharpness(image)
    e_score = edge_uniformity(image)
    h_norm = min(1.0, h_score / 450.0)
    return float(0.7 * h_norm + 0.3 * e_score)

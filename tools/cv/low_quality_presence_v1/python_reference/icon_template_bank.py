from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from .top3_localization import read_image


SIDE_REGIONS = {"left": (0, 0, 100, 100), "right": (340, 0, 440, 100)}


@dataclass(frozen=True)
class IconTemplate:
    annotation_id: str
    descriptor: np.ndarray
    bbox: list[int]
    robust_descriptor: np.ndarray | None = None


def side_region(image: np.ndarray, side: str) -> np.ndarray:
    x1, y1, x2, y2 = SIDE_REGIONS[side]
    return image[y1:y2, x1:x2]


def edge_descriptor(image: np.ndarray, side: str) -> np.ndarray:
    region = side_region(image, side)
    gray = cv2.cvtColor(region, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    edges = cv2.Canny(gray, 40, 135)
    edges[:2, :] = 0; edges[-2:, :] = 0; edges[:, :2] = 0; edges[:, -2:] = 0
    small = cv2.resize(edges, (64, 64), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0
    vector = small.reshape(-1)
    norm = float(np.linalg.norm(vector))
    return vector / norm if norm else vector



def bbox_descriptor(image: np.ndarray, bbox: list[int]) -> np.ndarray:
    x1, y1, x2, y2 = bbox
    patch = image[y1:y2, x1:x2]
    if patch.size == 0:
        return np.zeros(4096, dtype=np.float32)
    gray = cv2.cvtColor(patch, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    edges = cv2.Canny(gray, 40, 135)
    small = cv2.resize(edges, (64, 64), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0
    vector = small.reshape(-1)
    norm = float(np.linalg.norm(vector))
    return vector / norm if norm else vector


def robust_bbox_descriptor(image: np.ndarray, bbox: list[int]) -> np.ndarray:
    """Low-quality descriptor combining intensity, gradient, and edge structure."""
    x1, y1, x2, y2 = bbox
    patch = image[y1:y2, x1:x2]
    if patch.size == 0:
        return np.zeros(3072, dtype=np.float32)
    gray = cv2.cvtColor(patch, cv2.COLOR_BGR2GRAY)
    gray = cv2.bilateralFilter(gray, 5, 30, 30)
    gray = cv2.createCLAHE(clipLimit=1.6, tileGridSize=(4, 4)).apply(gray)

    intensity = cv2.resize(gray, (32, 32), interpolation=cv2.INTER_AREA).astype(np.float32)
    intensity = (intensity - float(np.mean(intensity))) / max(1.0, float(np.std(intensity)))

    gx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)
    gradient = cv2.magnitude(gx, gy)
    gradient = cv2.resize(gradient, (32, 32), interpolation=cv2.INTER_AREA)
    gradient = gradient / max(1.0, float(np.linalg.norm(gradient)))

    edges = cv2.Canny(gray, 24, 96)
    edges = cv2.resize(edges, (32, 32), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0

    vector = np.concatenate(
        [0.60 * intensity.reshape(-1), 0.30 * gradient.reshape(-1), 0.10 * edges.reshape(-1)]
    ).astype(np.float32)
    norm = float(np.linalg.norm(vector))
    return vector / norm if norm else vector


def build_templates(records: list[dict], side: str, exclude_annotation_id: str | None = None) -> list[IconTemplate]:
    templates: list[IconTemplate] = []
    for record in records:
        if record["side"] != side or record["icon_label"] != "present":
            continue
        if record["annotation_id"] == exclude_annotation_id:
            continue
        image = read_image(Path(record["canonical_path"]))
        if image is None:
            continue
        bbox = [int(value) for value in record["icon_bbox"].strip("[]").split(",")]
        templates.append(
            IconTemplate(
                record["annotation_id"],
                bbox_descriptor(image, bbox),
                bbox,
                robust_bbox_descriptor(image, bbox),
            )
        )
    return templates


def build_absent_templates(
    records: list[dict],
    side: str,
    reference_bbox: list[int],
    exclude_annotation_id: str | None = None,
) -> list[IconTemplate]:
    """Build Train negative templates at the positive position prior."""
    templates: list[IconTemplate] = []
    for record in records:
        if record["side"] != side or record["icon_label"] != "absent":
            continue
        if record["annotation_id"] == exclude_annotation_id:
            continue
        image = read_image(Path(record["canonical_path"]))
        if image is None:
            continue
        templates.append(
            IconTemplate(
                record["annotation_id"],
                bbox_descriptor(image, reference_bbox),
                list(reference_bbox),
                robust_bbox_descriptor(image, reference_bbox),
            )
        )
    return templates


def median_bbox(templates: list[IconTemplate], side: str) -> list[int]:
    if not templates:
        return []
    values = np.median(np.array([item.bbox for item in templates], dtype=np.float32), axis=0).round().astype(int).tolist()
    x1, y1, x2, y2 = values
    rx1, ry1, rx2, ry2 = SIDE_REGIONS[side]
    return [max(rx1, x1), max(ry1, y1), min(rx2, x2), min(ry2, y2)]

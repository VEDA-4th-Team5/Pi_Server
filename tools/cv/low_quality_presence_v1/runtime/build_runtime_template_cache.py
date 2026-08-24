"""Build a self-contained runtime template cache from reviewed development crops.

This creates descriptors only; source images and labels are not copied into the
server bundle. The bootstrap source used for the current bundle is explicitly
development-only and must not be treated as an independent performance test.
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import sys
from pathlib import Path

import cv2
import numpy as np

# Allow the handoff folder to run as a standalone package without requiring
# PYTHONPATH configuration on the server.
PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(PACKAGE_ROOT))

from python_reference.icon_template_bank import (
    SIDE_REGIONS,
    bbox_descriptor,
    median_bbox,
    robust_bbox_descriptor,
)


TARGET_SIZE = (440, 100)


def _read_image(path: Path) -> np.ndarray | None:
    data = np.fromfile(str(path), dtype=np.uint8)
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def _encode(values: np.ndarray) -> str:
    array = np.asarray(values, dtype=np.float32)
    return base64.b64encode(array.tobytes()).decode("ascii")


def _scaled_bbox(raw: list[int], shape: tuple[int, ...]) -> list[int]:
    height, width = shape[:2]
    sx = TARGET_SIZE[0] / float(width)
    sy = TARGET_SIZE[1] / float(height)
    x1, y1, x2, y2 = [int(round(value)) for value in raw]
    scaled = [round(x1 * sx), round(y1 * sy), round(x2 * sx), round(y2 * sy)]
    return [
        max(0, min(TARGET_SIZE[0] - 1, scaled[0])),
        max(0, min(TARGET_SIZE[1] - 1, scaled[1])),
        max(1, min(TARGET_SIZE[0], scaled[2])),
        max(1, min(TARGET_SIZE[1], scaled[3])),
    ]


def _clip_to_side(bbox: list[int], side: str) -> list[int]:
    rx1, ry1, rx2, ry2 = SIDE_REGIONS[side]
    x1, y1, x2, y2 = bbox
    return [max(rx1, x1), max(ry1, y1), min(rx2, x2), min(ry2, y2)]


def _record(annotation_id: str, image: np.ndarray, side: str, bbox: list[int]) -> dict:
    return {
        "annotation_id": annotation_id,
        "bbox": bbox,
        "descriptor_length": 4096,
        "robust_descriptor_length": 3072,
        "descriptor_b64": _encode(bbox_descriptor(image, bbox)),
        "robust_descriptor_b64": _encode(robust_bbox_descriptor(image, bbox)),
        "side": side,
    }


def _positive_templates(positive_dir: Path, details_path: Path) -> dict[str, list[dict]]:
    result = {"left": [], "right": []}
    with details_path.open(encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            side = row.get("side", "")
            if side not in result:
                continue
            annotation_id = row["annotation_id"]
            source = positive_dir / f"{annotation_id}.png"
            image = _read_image(source)
            if image is None:
                continue
            raw_bbox = json.loads(row["region_bbox"])
            resized = cv2.resize(image, TARGET_SIZE, interpolation=cv2.INTER_AREA)
            bbox = _clip_to_side(_scaled_bbox(raw_bbox, image.shape), side)
            if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
                continue
            result[side].append(_record(annotation_id, resized, side, bbox))
    return result


def _negative_templates(negative_dir: Path, positive: dict[str, list[dict]], limit: int) -> dict[str, list[dict]]:
    result = {"left": [], "right": []}
    paths = sorted(
        path for path in negative_dir.iterdir() if path.suffix.lower() in {".jpg", ".jpeg", ".png"}
    )[:limit]
    for side in result:
        positive_records = positive[side]
        if not positive_records:
            continue
        median = median_bbox(
            [
                type("Template", (), {"bbox": row["bbox"]})()
                for row in positive_records
            ],
            side,
        )
        for path in paths:
            image = _read_image(path)
            if image is None:
                continue
            resized = cv2.resize(image, TARGET_SIZE, interpolation=cv2.INTER_AREA)
            result[side].append(_record(f"negative-{path.stem}", resized, side, median))
    return result


def build(args: argparse.Namespace) -> dict:
    positive = _positive_templates(Path(args.positive_dir), Path(args.positive_details))
    if min(len(positive["left"]), len(positive["right"])) < args.min_positive_per_side:
        raise ValueError("positive_template_count_below_minimum")
    negative = _negative_templates(Path(args.negative_dir), positive, args.negative_limit)
    if min(len(negative["left"]), len(negative["right"])) < args.min_negative_per_side:
        raise ValueError("negative_template_count_below_minimum")
    payload = {
        "schema": "low_quality_runtime_template_cache_v1",
        "target_size": list(TARGET_SIZE),
        "template_source": "development_bootstrap_manual_rectified_ev_and_inferred_negative",
        "performance_claim": False,
        "positive": positive,
        "negative": negative,
        "counts": {
            "positive_left": len(positive["left"]),
            "positive_right": len(positive["right"]),
            "negative_left": len(negative["left"]),
            "negative_right": len(negative["right"]),
        },
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    return payload["counts"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--positive-dir", required=True)
    parser.add_argument("--positive-details", required=True)
    parser.add_argument("--negative-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--negative-limit", type=int, default=120)
    parser.add_argument("--min-positive-per-side", type=int, default=10)
    parser.add_argument("--min-negative-per-side", type=int, default=20)
    args = parser.parse_args()
    print(json.dumps(build(args), ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

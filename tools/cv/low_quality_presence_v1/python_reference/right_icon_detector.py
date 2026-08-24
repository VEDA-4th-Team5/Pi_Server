from __future__ import annotations

from .icon_detector_common import detect_icon


def detect(image, templates):
    return detect_icon(image, "right", templates)


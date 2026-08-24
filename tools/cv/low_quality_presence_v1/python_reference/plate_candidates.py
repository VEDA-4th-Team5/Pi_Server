from __future__ import annotations

from typing import Dict, List, Tuple

from .candidate_regions import CandidateRegion, detect_regions
from .color_features import build_color_mask


def detect_candidate_regions(
    bgr,
    thresholds: Dict,
) -> Tuple[List[CandidateRegion], Dict[str, object], object]:
    mask, mask_metrics = build_color_mask(bgr, thresholds)
    max_k = int(thresholds.get("candidate", {}).get("top_k", 3))
    candidates = detect_regions(bgr, mask, thresholds)
    top_candidates = candidates[: max(1, max_k)]
    ctx = {
        "candidate_count": len(candidates),
        "top_k_count": len(top_candidates),
    }
    ctx.update(mask_metrics)
    return top_candidates, ctx, mask


def candidate_signature(candidate: CandidateRegion | None) -> List[int] | None:
    if candidate is None:
        return None
    return candidate.bbox

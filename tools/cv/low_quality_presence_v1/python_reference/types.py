from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple


@dataclass
class CandidateRegion:
    x: int
    y: int
    w: int
    h: int
    area_ratio: float
    score: float

    @property
    def bbox(self) -> List[int]:
        return [int(self.x), int(self.y), int(self.w), int(self.h)]


@dataclass
class PrescreenRecord:
    source_path: str
    sha256: str
    width: int
    height: int
    status: str
    decision: str
    confidence: float
    global_color_ratio: float
    best_region_score: float
    best_region_bbox: Optional[List[int]]
    best_region_area_ratio: float
    candidate_region_count: int
    exposure_warning: bool
    processing_ms: int
    error_reason: str

    def to_csv_row(self, header: List[str]) -> Dict[str, Any]:
        return {
            "source_path": self.source_path,
            "sha256": self.sha256,
            "width": self.width,
            "height": self.height,
            "status": self.status,
            "decision": self.decision,
            "confidence": f"{self.confidence:.6f}",
            "global_color_ratio": f"{self.global_color_ratio:.8f}",
            "best_region_score": f"{self.best_region_score:.6f}",
            "best_region_bbox": "" if self.best_region_bbox is None else self.best_region_bbox,
            "best_region_area_ratio": f"{self.best_region_area_ratio:.8f}",
            "candidate_region_count": self.candidate_region_count,
            "exposure_warning": str(bool(self.exposure_warning)).lower(),
            "processing_ms": self.processing_ms,
            "error_reason": self.error_reason,
        }

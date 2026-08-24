"""Run one raw image through the low-quality presence runtime contract.

The command writes a JSON result and rectified JPEGs. The EV field is an
EV-prescreen candidate only; final EV/NON_EV classification is intentionally
not implemented by this development-only bundle.
"""

from __future__ import annotations

import argparse
import base64
from functools import lru_cache
import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np


PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(PACKAGE_ROOT))

from python_reference.icon_template_bank import IconTemplate  # noqa: E402
from python_reference.low_quality_independent_holdout_predict import (  # noqa: E402
    _candidate_records,
    _load_frozen_models,
)
from python_reference.low_quality_selector_ocr_stress import (  # noqa: E402
    choose_frozen_candidate,
)
from python_reference.top3_localization import read_image  # noqa: E402


MODEL_BUNDLE = PACKAGE_ROOT / "model_bundle"
MODEL_VERSION = "low_quality_presence_v1_dev"


def _load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _decode_vector(value: str, length: int) -> np.ndarray:
    raw = base64.b64decode(value.encode("ascii"))
    result = np.frombuffer(raw, dtype=np.float32).copy()
    if result.shape != (length,) or not np.all(np.isfinite(result)):
        raise ValueError("runtime_template_descriptor_schema_mismatch")
    return result


@lru_cache(maxsize=4)
def _load_template_cache(path: Path) -> dict[tuple[str, str, str], list[IconTemplate]]:
    payload = _load_json(path)
    if payload.get("schema") != "low_quality_runtime_template_cache_v1":
        raise ValueError("runtime_template_cache_schema_mismatch")
    cache: dict[tuple[str, str, str], list[IconTemplate]] = {}
    for label in ("positive", "negative"):
        for side in ("left", "right"):
            records = payload.get(label, {}).get(side, [])
            templates = []
            for record in records:
                templates.append(
                    IconTemplate(
                        annotation_id=str(record["annotation_id"]),
                        descriptor=_decode_vector(record["descriptor_b64"], 4096),
                        bbox=[int(value) for value in record["bbox"]],
                        robust_descriptor=_decode_vector(record["robust_descriptor_b64"], 3072),
                    )
                )
            if not templates:
                raise ValueError(f"runtime_template_cache_empty:{label}:{side}")
            cache[(side, label, "")] = templates
    return cache


@lru_cache(maxsize=4)
def _load_model_bundle(model_bundle_dir: Path) -> dict:
    return _load_frozen_models(
        model_bundle_dir / "selector_summary.json",
        model_bundle_dir / "side_model_summary.json",
        model_bundle_dir / "spatial_summary.json",
        model_bundle_dir / "guard_summary.json",
    )


def load_runtime_artifacts(
    model_bundle_dir: Path, template_cache: Path, thresholds: Path
) -> tuple[dict, dict, dict]:
    """모델 계약을 검증하고 프로세스 수명 동안 읽기 전용으로 재사용한다."""
    normalized_bundle = model_bundle_dir.resolve()
    normalized_cache = template_cache.resolve()
    normalized_thresholds = thresholds.resolve()
    return (
        _load_json(normalized_thresholds),
        _load_model_bundle(normalized_bundle),
        _load_template_cache(normalized_cache),
    )


def _write_image(path: Path, image: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ok, encoded = cv2.imencode(".jpg", image, [int(cv2.IMWRITE_JPEG_QUALITY), 95])
    if not ok:
        raise ValueError(f"jpeg_encode_failed:{path.name}")
    encoded.tofile(str(path))


def _base_result(args: argparse.Namespace) -> dict:
    return {
        "schema": "low_quality_presence_runtime_result_v1",
        "request_id": args.request_id,
        "model_version": MODEL_VERSION,
        "source_mode": args.source_mode,
        "input_path": str(Path(args.image).resolve()),
        "status": "error",
        "final_ev_classification": "NOT_AVAILABLE",
        "ev_prescreen_decision": "REVIEW",
        "presence_decision": "REVIEW",
        "icon_presence_predicted": 0,
        "plate_candidate_found": False,
        "ocr_route": "baseline_rectified",
        "ocr_status": "not_run_by_runtime",
        "ocr_input_path": "",
        "processing_ms": 0.0,
        "error": "",
    }


def run(args: argparse.Namespace) -> dict:
    started = time.perf_counter()
    result = _base_result(args)
    image = read_image(Path(args.image))
    if image is None:
        result["error"] = "image_decode_failed"
        result["processing_ms"] = (time.perf_counter() - started) * 1000.0
        return result

    thresholds, frozen, template_cache = load_runtime_artifacts(
        Path(args.model_bundle_dir),
        Path(args.template_cache),
        Path(args.thresholds),
    )
    candidates = _candidate_records(image, thresholds, frozen, template_cache)
    if not candidates:
        result["error"] = "no_candidate"
        result["reason"] = "no_candidate"
        result["processing_ms"] = (time.perf_counter() - started) * 1000.0
        return result

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    baseline = candidates[0]
    selected = choose_frozen_candidate(candidates)
    baseline_path = output_dir / "baseline_rectified.jpg"
    _write_image(baseline_path, baseline["rectified_image"])
    for candidate in candidates:
        _write_image(output_dir / f"candidate_rank_{candidate['candidate_rank']}.jpg", candidate["rectified_image"])

    if selected is None:
        result["error"] = "no_selected_candidate"
        result["reason"] = "no_selected_candidate"
        result["ocr_input_path"] = str(baseline_path.resolve())
        result["processing_ms"] = (time.perf_counter() - started) * 1000.0
        return result

    selected_path = output_dir / "selected_rectified.jpg"
    _write_image(selected_path, selected["rectified_image"])
    runtime = selected["runtime"]
    decision = str(runtime["runtime_presence_decision"])
    result.update(
        {
            "status": "ok",
            "input_quality": float(runtime["input_quality"]),
            "quality_below_threshold": bool(runtime["quality_below_threshold"]),
            "plate_candidate_found": True,
            "icon_presence_predicted": int(runtime["runtime_vehicle_present"]),
            "presence_decision": decision,
            "ev_prescreen_decision": {
                "PRESENT": "EV_CANDIDATE",
                "ABSENT": "NON_EV_CANDIDATE",
                "REVIEW": "REVIEW",
            }.get(decision, "REVIEW"),
            "reason": str(runtime["runtime_decision_reason"]),
            "rectification_success": bool(selected["runtime_geometric"]),
            "rectification_method": str(selected["rectification_method"]),
            "selected_candidate_rank": int(selected["candidate_rank"]),
            "selected_candidate_bbox": [int(value) for value in selected["candidate_bbox"]],
            "selected_rectified_path": str(selected_path.resolve()),
            "baseline_rectified_path": str(baseline_path.resolve()),
            "ocr_input_path": str(baseline_path.resolve()),
            "candidate_count": len(candidates),
            "ocr_status": "not_run_by_runtime",
            "error": "",
        }
    )
    result["processing_ms"] = (time.perf_counter() - started) * 1000.0
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--request-id", default="local-runtime-request")
    parser.add_argument("--source-mode", choices=("rgb", "ir", "rgb_ir"), default="rgb")
    parser.add_argument("--model-bundle-dir", default=str(MODEL_BUNDLE))
    parser.add_argument("--template-cache", default=str(MODEL_BUNDLE / "runtime_template_cache.json"))
    parser.add_argument("--thresholds", default=str(MODEL_BUNDLE / "default_thresholds.json"))
    args = parser.parse_args()
    try:
        result = run(args)
    except Exception as exc:  # runtime boundary: return a structured error to the server
        result = _base_result(args)
        result["error"] = f"runtime_exception:{type(exc).__name__}:{exc}"
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = output_dir / "result.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"result_path": str(result_path.resolve()), **result}, ensure_ascii=False))
    return 0 if result.get("error", "") == "" else 2


if __name__ == "__main__":
    raise SystemExit(main())

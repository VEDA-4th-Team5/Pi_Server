"""Pi Server용 지속 실행 EV 사전 판별 worker.

표준 입력으로 한 줄짜리 JSON 요청을 받고 표준 출력으로 한 줄짜리 JSON
결과를 반환한다. 모델 결과는 최종 EV 인증값이 아니라 사전 판별값이다.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import cv2

from server_runtime import MODEL_BUNDLE, _base_result, load_runtime_artifacts, run


REQUEST_SCHEMA = "entrance_ev_analysis_request_v1"
READY_SCHEMA = "entrance_ev_worker_ready_v1"


def _write_result(output_dir: Path, result: dict) -> str:
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = output_dir / "result.json"
    result_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return str(result_path.resolve())


def _error_result(request_id: str, message: str) -> dict:
    args = SimpleNamespace(
        request_id=request_id,
        source_mode="rgb",
        image="",
    )
    result = _base_result(args)
    result["error"] = message
    return result


def _handle(request: dict, options: argparse.Namespace) -> dict:
    request_id = str(request.get("request_id", ""))
    if request.get("schema") != REQUEST_SCHEMA:
        return _error_result(request_id, "request_schema_mismatch")
    image = str(request.get("image_path", ""))
    output_dir = str(request.get("output_dir", ""))
    source_mode = str(request.get("source_mode", "rgb"))
    if not request_id or not image or not output_dir:
        return _error_result(request_id, "required_request_field_missing")
    if source_mode not in ("rgb", "ir", "rgb_ir"):
        return _error_result(request_id, "source_mode_invalid")

    args = SimpleNamespace(
        request_id=request_id,
        source_mode=source_mode,
        image=image,
        output_dir=output_dir,
        model_bundle_dir=options.model_bundle_dir,
        template_cache=options.template_cache,
        thresholds=options.thresholds,
    )
    try:
        result = run(args)
    except Exception as error:  # 프로세스 경계에서 오류를 구조화한다.
        result = _error_result(
            request_id, f"runtime_exception:{type(error).__name__}:{error}"
        )
    try:
        result["result_path"] = _write_result(Path(output_dir), result)
    except Exception as error:
        result["error"] = f"result_write_failed:{type(error).__name__}:{error}"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-bundle-dir", default=str(MODEL_BUNDLE))
    parser.add_argument(
        "--template-cache",
        default=str(MODEL_BUNDLE / "runtime_template_cache.json"),
    )
    parser.add_argument(
        "--thresholds", default=str(MODEL_BUNDLE / "default_thresholds.json")
    )
    parser.add_argument("--opencv-threads", type=int, default=1)
    options = parser.parse_args()

    cv2.setNumThreads(max(1, options.opencv_threads))
    cv2.ocl.setUseOpenCL(False)
    try:
        load_runtime_artifacts(
            Path(options.model_bundle_dir),
            Path(options.template_cache),
            Path(options.thresholds),
        )
    except Exception as error:
        print(
            json.dumps(
                {
                    "schema": READY_SCHEMA,
                    "status": "error",
                    "error": f"artifact_validation_failed:{type(error).__name__}",
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
        return 2
    print(
        json.dumps(
            {"schema": READY_SCHEMA, "status": "ready"},
            separators=(",", ":"),
        ),
        flush=True,
    )
    for line in sys.stdin:
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError("request_must_be_object")
            result = _handle(request, options)
        except Exception as error:
            result = _error_result(
                "", f"protocol_error:{type(error).__name__}:{error}"
            )
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

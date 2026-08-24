"""Validation-only OCR non-regression guard for the quality-aware plate rectifier."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
import subprocess
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .low_quality_presence_rectification import (
    ENVIRONMENTS,
    LOW_QUALITY_ENVIRONMENTS,
    QUALITY_THRESHOLD,
    TARGET_SIZE,
    apply_environment,
)
from .plate_rectifier import GEOMETRIC_METHODS, rectify_plate, rectify_plate_legacy
from .rectification_quality import rectification_quality_score


EXPERIMENT_ID = "low-quality-rectification-ocr-guard-001"
VARIANTS = ("baseline", "quality_aware")
ALLOWED_PLATE_TEXT = re.compile(r"[^0-9A-Za-z\uac00-\ud7a3]")
CAPTURE_SUFFIX = re.compile(r"-\d+$")


def normalize_plate(value: str) -> str:
    return ALLOWED_PLATE_TEXT.sub("", value or "").upper()


def ground_truth_from_path(path: Path) -> str:
    return normalize_plate(CAPTURE_SUFFIX.sub("", path.stem))


def levenshtein(left: str, right: str) -> int:
    if len(left) < len(right):
        left, right = right, left
    previous = list(range(len(right) + 1))
    for left_index, left_char in enumerate(left, 1):
        current = [left_index]
        for right_index, right_char in enumerate(right, 1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[right_index] + 1,
                    previous[right_index - 1] + (left_char != right_char),
                )
            )
        previous = current
    return previous[-1]


def _stable_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def select_validation_representatives(manifest_path: Path, sample_size: int) -> list[dict[str, str]]:
    if sample_size <= 0:
        raise ValueError("sample_size must be positive")
    with manifest_path.open(encoding="utf-8", newline="") as stream:
        rows = [
            row
            for row in csv.DictReader(stream)
            if row.get("split") == "validation" and Path(row.get("input_path", "")).is_file()
        ]
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        group_id = row.get("vehicle_id") or row.get("source_id")
        if not group_id:
            continue
        grouped[group_id].append(row)
    representatives: list[dict[str, str]] = []
    for group_id, candidates in grouped.items():
        representative = min(candidates, key=lambda item: _stable_hash(str(item["input_path"])))
        selected = dict(representative)
        selected["selection_group"] = group_id
        selected["selection_hash"] = _stable_hash(group_id)
        selected["ground_truth"] = ground_truth_from_path(Path(selected["input_path"]))
        representatives.append(selected)
    representatives.sort(key=lambda item: (item["selection_hash"], item["selection_group"]))
    return representatives[:sample_size]


def _read_unicode(path: Path) -> np.ndarray | None:
    try:
        return cv2.imdecode(np.fromfile(str(path), dtype=np.uint8), cv2.IMREAD_COLOR)
    except OSError:
        return None


def _ocr(
    image: np.ndarray,
    tessdata_dir: Path,
    executable: Path,
    timeout_seconds: float,
) -> tuple[str, str]:
    encoded, payload = cv2.imencode(".png", image)
    if not encoded:
        return "", "png_encode_failed"
    command = [
        str(executable),
        "stdin",
        "stdout",
        "--tessdata-dir",
        str(tessdata_dir.resolve()),
        "-l",
        "kor",
        "--psm",
        "7",
    ]
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        completed = subprocess.run(
            command,
            input=payload.tobytes(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout_seconds,
            creationflags=creation_flags,
        )
    except subprocess.TimeoutExpired:
        return "", "tesseract_timeout"
    except OSError as exc:
        return "", f"tesseract_os_error:{str(exc)[:120]}"
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").splitlines()
        return "", f"tesseract_exit_{completed.returncode}:{(message[0] if message else '')[:120]}"
    return normalize_plate(completed.stdout.decode("utf-8", errors="replace")), ""


def _percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(np.ceil(len(ordered) * quantile)) - 1))
    return float(ordered[index])


def summarize_rows(rows: list[dict[str, object]], environment: str, variant: str) -> dict[str, object]:
    group = [row for row in rows if row["variant"] == variant]
    if environment == "synthetic_low_quality":
        group = [row for row in group if row["environment"] in LOW_QUALITY_ENVIRONMENTS]
    elif environment == "quality_below_0_25":
        group = [row for row in group if int(row["quality_below_threshold"]) == 1]
    else:
        group = [row for row in group if row["environment"] == environment]
    valid = [row for row in group if not row["error"]]
    rectification_times = [float(row["rectification_ms"]) for row in valid]
    ocr_times = [float(row["ocr_ms"]) for row in valid]
    total_times = [float(row["total_ms"]) for row in valid]
    return {
        "split": "validation",
        "environment": environment,
        "variant": variant,
        "count": len(group),
        "unique_source_count": len({str(row["source_id"]) for row in group}),
        "measured_low_quality_count": sum(int(row["quality_below_threshold"]) for row in group),
        "exact_match_rate": (
            sum(int(row["exact_match"]) for row in valid) / len(valid) if valid else 0.0
        ),
        "mean_cer": statistics.mean(float(row["cer"]) for row in valid) if valid else 0.0,
        "geometric_rectification_success": (
            sum(int(row["geometric_success"]) for row in valid) / len(valid) if valid else 0.0
        ),
        "error_count": len(group) - len(valid),
        "mean_rectification_ms": statistics.mean(rectification_times) if rectification_times else 0.0,
        "p95_rectification_ms": _percentile(rectification_times, 0.95),
        "mean_ocr_ms": statistics.mean(ocr_times) if ocr_times else 0.0,
        "p95_ocr_ms": _percentile(ocr_times, 0.95),
        "mean_total_ms": statistics.mean(total_times) if total_times else 0.0,
        "p95_total_ms": _percentile(total_times, 0.95),
    }


def _write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        raise ValueError(f"no rows for {path.name}")
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=tuple(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def _report(summary: dict[str, object], lookup: dict[tuple[str, str], dict[str, object]]) -> str:
    baseline = lookup[("synthetic_low_quality", "baseline")]
    candidate = lookup[("synthetic_low_quality", "quality_aware")]
    low_baseline = lookup[("quality_below_0_25", "baseline")]
    low_candidate = lookup[("quality_below_0_25", "quality_aware")]
    gate_lines = "\n".join(
        f"- {name}: **{'PASS' if passed else 'FAIL'}**" for name, passed in summary["gate"].items()
    )
    return f"""# {summary['experiment_id']}

- Status: `{summary['status']}`
- OCR Validation sources: {summary['sample_size']} unique plates
- OCR Test split opened: `false`
- EV formal Test opened: `false`
- Scope: 일반 NON_EV 번호판 OCR downstream guard; EV·IR 운영 성능이 아님

## Synthetic low-quality

| Variant | Exact Match | Mean CER | Geometric rectification | Mean/P95 total ms |
|---|---:|---:|---:|---:|
| baseline | {100 * float(baseline['exact_match_rate']):.2f}% | {float(baseline['mean_cer']):.4f} | {100 * float(baseline['geometric_rectification_success']):.2f}% | {float(baseline['mean_total_ms']):.2f}/{float(baseline['p95_total_ms']):.2f} |
| quality_aware | {100 * float(candidate['exact_match_rate']):.2f}% | {float(candidate['mean_cer']):.4f} | {100 * float(candidate['geometric_rectification_success']):.2f}% | {float(candidate['mean_total_ms']):.2f}/{float(candidate['p95_total_ms']):.2f} |

## Measured quality below 0.25

| Variant | Rows | Exact Match | Mean CER | Geometric rectification |
|---|---:|---:|---:|---:|
| baseline | {int(low_baseline['count'])} | {100 * float(low_baseline['exact_match_rate']):.2f}% | {float(low_baseline['mean_cer']):.4f} | {100 * float(low_baseline['geometric_rectification_success']):.2f}% |
| quality_aware | {int(low_candidate['count'])} | {100 * float(low_candidate['exact_match_rate']):.2f}% | {float(low_candidate['mean_cer']):.4f} | {100 * float(low_candidate['geometric_rectification_success']):.2f}% |

## Gate

{gate_lines}

이 결과는 고정 Validation 50개 번호판의 보조 비회귀 검사다. 실제 EV 번호판, 야간/IR, 번호판 전용 OCR, Raspberry Pi 처리시간, 독립 Test를 검증하지 않는다.
"""


def run(args: argparse.Namespace) -> dict[str, object]:
    manifest_path = args.manifest.resolve()
    output_dir = args.output.resolve()
    tessdata_dir = args.tessdata.resolve()
    executable = args.tesseract.resolve()
    if not executable.is_file():
        raise FileNotFoundError(f"tesseract executable not found: {executable}")
    if not (tessdata_dir / "kor.traineddata").is_file():
        raise FileNotFoundError(f"kor.traineddata not found: {tessdata_dir}")
    selected = select_validation_representatives(manifest_path, args.sample_size)
    if len(selected) < args.sample_size:
        raise ValueError(f"requested {args.sample_size} sources but only {len(selected)} are available")

    manifest_rows: list[dict[str, object]] = []
    detail_rows: list[dict[str, object]] = []
    rectifiers = {"baseline": rectify_plate_legacy, "quality_aware": rectify_plate}
    for rank, source in enumerate(selected, 1):
        input_path = Path(source["input_path"])
        ground_truth = source["ground_truth"]
        manifest_rows.append(
            {
                "selection_rank": rank,
                "selection_hash": source["selection_hash"],
                "source_id": source["source_id"],
                "vehicle_id": source["vehicle_id"],
                "split": "validation",
                "ground_truth": ground_truth,
                "input_path": str(input_path),
            }
        )
        image = _read_unicode(input_path)
        for environment in ENVIRONMENTS:
            degraded = apply_environment(image, environment) if image is not None else None
            if degraded is None:
                input_quality = 0.0
            else:
                normalized_input = cv2.resize(degraded, TARGET_SIZE, interpolation=cv2.INTER_AREA)
                input_quality = rectification_quality_score(normalized_input)
            for variant in VARIANTS:
                error = ""
                rectification_status = "error"
                rectification_method = "none"
                geometry_confidence = 0.0
                output_quality = 0.0
                text = ""
                rectification_ms = 0.0
                ocr_ms = 0.0
                started = time.perf_counter()
                if degraded is None:
                    error = "unreadable_input"
                else:
                    rectification_started = time.perf_counter()
                    try:
                        result = rectifiers[variant](degraded, TARGET_SIZE)
                        rectification_ms = (time.perf_counter() - rectification_started) * 1000.0
                        rectification_status = result.status
                        rectification_method = result.method
                        geometry_confidence = float(result.rect_quality_input)
                        output_quality = rectification_quality_score(result.plate_image)
                        ocr_started = time.perf_counter()
                        text, error = _ocr(
                            result.plate_image,
                            tessdata_dir,
                            executable,
                            args.ocr_timeout_seconds,
                        )
                        ocr_ms = (time.perf_counter() - ocr_started) * 1000.0
                    except (cv2.error, ValueError) as exc:
                        rectification_ms = (time.perf_counter() - rectification_started) * 1000.0
                        error = f"rectification_error:{str(exc).splitlines()[0][:120]}"
                distance = levenshtein(ground_truth, text)
                detail_rows.append(
                    {
                        "experiment_id": args.experiment_id,
                        "source_id": source["source_id"],
                        "vehicle_id": source["vehicle_id"],
                        "split": "validation",
                        "environment": environment,
                        "variant": variant,
                        "ground_truth": ground_truth,
                        "input_path": str(input_path),
                        "input_quality": input_quality,
                        "quality_below_threshold": int(input_quality < QUALITY_THRESHOLD),
                        "rectification_status": rectification_status,
                        "rectification_method": rectification_method,
                        "geometric_success": int(rectification_method in GEOMETRIC_METHODS),
                        "geometry_confidence": geometry_confidence,
                        "output_quality": output_quality,
                        "ocr_text": text,
                        "exact_match": int(text == ground_truth and not error),
                        "edit_distance": distance,
                        "cer": distance / max(1, len(ground_truth)),
                        "error": error,
                        "rectification_ms": rectification_ms,
                        "ocr_ms": ocr_ms,
                        "total_ms": (time.perf_counter() - started) * 1000.0,
                    }
                )

    summary_environments = [*ENVIRONMENTS, "synthetic_low_quality", "quality_below_0_25"]
    environment_rows = [
        summarize_rows(detail_rows, environment, variant)
        for environment in summary_environments
        for variant in VARIANTS
    ]
    lookup = {(str(row["environment"]), str(row["variant"])): row for row in environment_rows}
    baseline = lookup[("synthetic_low_quality", "baseline")]
    candidate = lookup[("synthetic_low_quality", "quality_aware")]
    low_baseline = lookup[("quality_below_0_25", "baseline")]
    low_candidate = lookup[("quality_below_0_25", "quality_aware")]
    error_count = sum(bool(row["error"]) for row in detail_rows)
    gate = {
        "synthetic_low_quality_exact_match_non_regression": float(candidate["exact_match_rate"]) >= float(baseline["exact_match_rate"]),
        "synthetic_low_quality_cer_non_regression": float(candidate["mean_cer"]) <= float(baseline["mean_cer"]),
        "quality_below_0_25_exact_match_non_regression": float(low_candidate["exact_match_rate"]) >= float(low_baseline["exact_match_rate"]),
        "quality_below_0_25_cer_non_regression": float(low_candidate["mean_cer"]) <= float(low_baseline["mean_cer"]),
        "processing_errors_zero": error_count == 0,
    }
    summary: dict[str, object] = {
        "experiment_id": args.experiment_id,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "ocr_guard_pass" if all(gate.values()) else "ocr_guard_fail",
        "sample_size": len(selected),
        "detail_rows": len(detail_rows),
        "source_split": "validation",
        "source_unique_vehicle_count": len({row["vehicle_id"] for row in selected}),
        "environments": list(ENVIRONMENTS),
        "quality_threshold": QUALITY_THRESHOLD,
        "target_size": list(TARGET_SIZE),
        "tesseract_language": "kor",
        "tesseract_executable": str(executable),
        "tessdata_dir": str(tessdata_dir),
        "error_count": error_count,
        "gate": gate,
        "ocr_test_split_opened": False,
        "formal_ev_test_opened": False,
        "operational_ev_classifier": False,
        "real_ir_evaluated": False,
        "source_images_modified": False,
    }
    output_dir.mkdir(parents=True, exist_ok=False)
    _write_csv(output_dir / "manifest.csv", manifest_rows)
    _write_csv(output_dir / "details.csv", detail_rows)
    _write_csv(output_dir / "environment_summary.csv", environment_rows)
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_dir / "REPORT.md").write_text(_report(summary, lookup), encoding="utf-8")
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tessdata", type=Path, required=True)
    parser.add_argument("--tesseract", type=Path, required=True)
    parser.add_argument("--sample-size", type=int, default=50)
    parser.add_argument("--ocr-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--experiment-id", default=EXPERIMENT_ID)
    return parser


def main() -> int:
    summary = run(build_parser().parse_args())
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

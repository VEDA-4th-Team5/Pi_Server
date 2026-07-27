#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUIDE_HTML="${PROJECT_ROOT}/docs/code_guide/index.html"
OUTPUT_PDF="${PROJECT_ROOT}/Pi_Server_Code_Guide.pdf"

for command_name in chromium; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "문서 생성 실패: ${command_name} 명령이 필요합니다." >&2
        exit 1
    fi
done

cd "${PROJECT_ROOT}"

chromium \
    --headless \
    --no-sandbox \
    --disable-gpu \
    --disable-dev-shm-usage \
    --no-pdf-header-footer \
    --print-to-pdf="${OUTPUT_PDF}" \
    "file://${GUIDE_HTML}" >/dev/null 2>&1

echo "코드 가이드 PDF: ${OUTPUT_PDF}"

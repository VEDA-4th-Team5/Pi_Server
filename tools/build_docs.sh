#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOC_BUILD_DIR="${DOC_BUILD_DIR:-${PROJECT_ROOT}/build/docs}"
GENERATED_GUIDE_MD="${PROJECT_ROOT}/docs/generated/PI_SERVER_CODE_GUIDE.md"
GUIDE_HTML="${DOC_BUILD_DIR}/guide/html/index.html"
OUTPUT_PDF="${PROJECT_ROOT}/Pi_Server_Code_Guide.pdf"
GENERATOR="${1:-${DOC_BUILD_DIR}/code-doc-generator}"

for command_name in chromium doxygen; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "문서 생성 실패: ${command_name} 명령이 필요합니다." >&2
        exit 1
    fi
done

cd "${PROJECT_ROOT}"

mkdir -p "${DOC_BUILD_DIR}"
if [[ ! -x "${GENERATOR}" ]]; then
    "${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic \
        "${PROJECT_ROOT}/tools/code_doc_generator.cpp" -o "${GENERATOR}"
fi

export PROJECT_ROOT DOC_BUILD_DIR GENERATED_GUIDE_MD
doxygen "${PROJECT_ROOT}/docs/code_guide/Doxyfile.source"
"${GENERATOR}" "${PROJECT_ROOT}" "${DOC_BUILD_DIR}/source/xml" \
    "${PROJECT_ROOT}/docs/architecture/README.md" "${GENERATED_GUIDE_MD}"
doxygen "${PROJECT_ROOT}/docs/code_guide/Doxyfile.guide"

chromium \
    --headless \
    --no-sandbox \
    --disable-gpu \
    --disable-dev-shm-usage \
    --no-pdf-header-footer \
    --print-to-pdf="${OUTPUT_PDF}" \
    "file://${GUIDE_HTML}" >/dev/null 2>&1

echo "코드 가이드 Markdown: ${GENERATED_GUIDE_MD}"
echo "코드 가이드 PDF: ${OUTPUT_PDF}"

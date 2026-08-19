#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${PROJECT_ROOT}/data/db/parking.db"

command -v sqlite3 >/dev/null 2>&1 || {
    echo "오류: sqlite3 CLI가 필요합니다. sudo apt install sqlite3" >&2
    exit 1
}

# Qt의 ROI 웹 도구로 저장해 둔 슬롯 ROI는 DB 초기화와 별개로 유지한다.
# 없으면(첫 실행 등) 비워두고, seed.sql의 기본 ROI가 대신 적용된다.
ROI_KEY_PATTERN='parking_slot_roi.%'
preserved_roi_sql=""

if [[ -f "${DB_PATH}" ]]; then
    "${PROJECT_ROOT}/tools/backup_db.sh"
    preserved_roi_sql="$(sqlite3 "${DB_PATH}" \
        "SELECT 'INSERT OR REPLACE INTO SYSTEM_SETTINGS(key, value, updated_at) VALUES(' || quote(key) || ',' || quote(value) || ',' || quote(updated_at) || ');' FROM SYSTEM_SETTINGS WHERE key LIKE '${ROI_KEY_PATTERN}';")"
    rm -- "${DB_PATH}"
fi

sqlite3 -bail "${DB_PATH}" < "${PROJECT_ROOT}/db/schema.sql"
sqlite3 -bail "${DB_PATH}" < "${PROJECT_ROOT}/db/seed.sql"

if [[ -n "${preserved_roi_sql}" ]]; then
    echo "${preserved_roi_sql}" | sqlite3 -bail "${DB_PATH}"
    roi_count="$(echo "${preserved_roi_sql}" | grep -c '^INSERT')"
    echo "기존 슬롯 ROI ${roi_count}건 복원됨"
fi

echo "DB 초기화 완료: ${DB_PATH}"
sqlite3 "${DB_PATH}" ".tables"
for table in VEHICLE PARKING_SLOT PARKING_SESSION IMAGE_LOG EVENT_LOG; do
    count="$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM ${table};")"
    printf '%-18s %s\n' "${table}" "${count}"
done

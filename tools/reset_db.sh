#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${PROJECT_ROOT}/data/db/parking.db"

command -v sqlite3 >/dev/null 2>&1 || {
    echo "오류: sqlite3 CLI가 필요합니다. sudo apt install sqlite3" >&2
    exit 1
}

if [[ -f "${DB_PATH}" ]]; then
    "${PROJECT_ROOT}/tools/backup_db.sh"
    rm -- "${DB_PATH}"
fi

sqlite3 -bail "${DB_PATH}" < "${PROJECT_ROOT}/db/schema.sql"
sqlite3 -bail "${DB_PATH}" < "${PROJECT_ROOT}/tools/db_seed.sql"

echo "DB 초기화 완료: ${DB_PATH}"
sqlite3 "${DB_PATH}" ".tables"
for table in VEHICLE PARKING_SLOT PARKING_SESSION IMAGE_LOG EVENT_LOG; do
    count="$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM ${table};")"
    printf '%-18s %s\n' "${table}" "${count}"
done

#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${1:-${PROJECT_ROOT}/data/db/parking.db}"

command -v sqlite3 >/dev/null 2>&1 || {
    echo "오류: sqlite3 CLI가 필요합니다. sudo apt install sqlite3" >&2
    exit 1
}
[[ -f "${DB_PATH}" ]] || { echo "오류: DB가 없습니다: ${DB_PATH}" >&2; exit 1; }

echo "[TABLES]"
sqlite3 "${DB_PATH}" ".tables"

echo "[ROW COUNTS]"
for table in VEHICLE PARKING_SLOT PARKING_SESSION IMAGE_LOG EVENT_LOG; do
    count="$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM ${table};")"
    printf '%-18s %s\n' "${table}" "${count}"
done

echo "[VEHICLE]"
sqlite3 -header -column "${DB_PATH}" "SELECT * FROM VEHICLE;"
echo "[PARKING_SLOT]"
sqlite3 -header -column "${DB_PATH}" "SELECT * FROM PARKING_SLOT;"
echo "[RECENT EVENT_LOG]"
sqlite3 -header -column "${DB_PATH}" \
    "SELECT * FROM EVENT_LOG ORDER BY event_id DESC LIMIT 10;"

#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${PROJECT_ROOT}/data/db/parking.db"
BACKUP_DIR="${PROJECT_ROOT}/data/db/backups"

if [[ ! -f "${DB_PATH}" ]]; then
    echo "백업할 DB가 없습니다: ${DB_PATH}"
    exit 0
fi

mkdir -p "${BACKUP_DIR}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP_PATH="${BACKUP_DIR}/parking_${TIMESTAMP}.db"

while [[ -e "${BACKUP_PATH}" ]]; do
    sleep 1
    TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
    BACKUP_PATH="${BACKUP_DIR}/parking_${TIMESTAMP}.db"
done

cp -p -- "${DB_PATH}" "${BACKUP_PATH}"
echo "DB 백업 완료: ${BACKUP_PATH}"

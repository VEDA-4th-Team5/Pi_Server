#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${PROJECT_ROOT}/data/db/parking.db"

command -v sqlite3 >/dev/null 2>&1 || {
    echo "오류: sqlite3 CLI가 필요합니다. sudo apt install sqlite3" >&2
    exit 1
}

if [[ ! -f "${DB_PATH}" ]]; then
    echo "오류: 초기화할 DB가 없습니다: ${DB_PATH}" >&2
    echo "DB 최초 생성은 db/schema.sql과 db/seed.sql을 사용하세요." >&2
    exit 1
fi

if pgrep -x pi-server >/dev/null 2>&1; then
    echo "오류: pi-server 실행 중에는 주차 상태를 초기화할 수 없습니다." >&2
    echo "서버를 종료한 뒤 다시 실행하세요." >&2
    exit 1
fi

"${PROJECT_ROOT}/tools/backup_db.sh"

# 계정, Qt에서 저장한 ROI/타이머 설정, 차량 마스터, 입구 인식 결과,
# 화재 상태는 보존한다. PARKING_SESSION을 참조하는 증거/이벤트 행은
# 삭제하지 않고 session_id만 해제해 기록 자체를 남긴다.
sqlite3 -bail "${DB_PATH}" <<'SQL'
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

DELETE FROM OCCUPANCY_EXIT_DEADLINE;
DELETE FROM PARKING_CORRELATION_BINDING;
UPDATE IMAGE_LOG SET session_id = NULL WHERE session_id IS NOT NULL;
UPDATE EVENT_LOG SET session_id = NULL WHERE session_id IS NOT NULL;
DELETE FROM PARKING_SESSION;

DELETE FROM IVA_SLOT_OBSERVATION_STATE;
DELETE FROM OCCUPANCY_COMMAND_INBOX;
DELETE FROM OCCUPANCY_SENSOR_SEQUENCE_STATE;

UPDATE PARKING_SLOT
SET status = 'VACANT',
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now');

COMMIT;
PRAGMA foreign_key_check;
SQL

echo "주차 상태/차량 세션 초기화 완료: ${DB_PATH}"
sqlite3 -header -column "${DB_PATH}" \
    "SELECT slot_id, slot_type, status FROM PARKING_SLOT ORDER BY slot_id;"
for table in PARKING_SESSION app_users SYSTEM_SETTINGS VEHICLE \
             ENTRANCE_RECOGNITION FIRE_ALARM_STATE IMAGE_LOG EVENT_LOG; do
    count="$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM ${table};")"
    printf '%-30s %s\n' "${table}" "${count}"
done

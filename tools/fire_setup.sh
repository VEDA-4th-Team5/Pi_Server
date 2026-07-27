#!/usr/bin/env bash
# 화재 알람(UART -> Qt) 테스트에 필요한 로컬 환경변수 파일을 만든다.
# 카메라/Gemini 쪽 .env.*.local (RTSP, API 키 등)은 실제 값을 몰라서
# 여기서 건드리지 않는다. 필요하면 README.md의 안내를 따라 별도로 채워야 한다.
set -euo pipefail

cd "$(dirname "$0")/.."

ENV_FILE=".env.fire.local"

if [ -f "$ENV_FILE" ]; then
    echo "already exists: $ENV_FILE (수정하려면 직접 편집하세요)"
    exit 0
fi

cat > "$ENV_FILE" <<'EOF'
FIRE_ALARM_ENABLED=true
FIRE_UART_DEVICE=/dev/serial0
FIRE_UART_BAUD=115200
FIRE_UART_REOPEN_DELAY_MS=2000
FIRE_TOPIC_PREFIX=parking/fire
FIRE_SENSOR_SLOT_MAP=FLAME01=EV01
EOF

echo "created: $ENV_FILE"
echo "사용법: set -a; source ./$ENV_FILE; set +a; ./cmake-build/pi-server"

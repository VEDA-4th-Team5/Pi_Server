#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

export PI_SERVER_ROOT="$ROOT_DIR"
export PI_SERVER_ENV_DIR="${PI_SERVER_ENV_DIR:-$ROOT_DIR}"
if [[ "$PI_SERVER_ENV_DIR" != /* ]]; then
    PI_SERVER_ENV_DIR="$ROOT_DIR/$PI_SERVER_ENV_DIR"
    export PI_SERVER_ENV_DIR
fi

# Do not `source` .env files. The repository is edited on Windows and may be
# copied to Linux with CRLF line endings; load_env_file.sh strips CR safely.
source "$ROOT_DIR/tools/load_env_file.sh"

# 최초 실행에서도 별도 준비 스크립트 없이 화재 센서 기본 설정을 만든다.

# systemd(pi-server.service)가 이 스크립트를 ExecStart로 직접 부를 때는
# INVOCATION_ID가 항상 세팅되어 있다. 그게 없는데 systemd가 이 서비스를
# 이미 관리 중이라면, 지금 사람이 셸에서 직접 이 스크립트를 실행하는
# 것이다 — 여기서 그대로 foreground로 pi-server를 띄우면 systemd가
# 관리하는 인스턴스와 포트/UART를 두고 충돌한다. 그 대신 재빌드만 하고
# systemctl로 재시작을 위임한다.
if [[ -z "${INVOCATION_ID:-}" ]] && systemctl is-enabled pi-server >/dev/null 2>&1; then
    echo "pi-server.service가 systemd로 관리되고 있습니다."
    echo "여기서 직접 실행하지 않고, 재빌드 후 systemctl로 재시작합니다."
    if [[ ! -f ./cmake-build/CMakeCache.txt ]]; then
        cmake -S . -B cmake-build
    fi
    cmake --build cmake-build --target pi-server -j2
    sudo systemctl restart pi-server
    echo "재시작 완료. 로그 확인: journalctl -u pi-server -f"
    exit 0
fi

if [[ "${1:-}" == "restart" ]]; then
    pkill -INT -x pi-server 2>/dev/null || true
    # RTSP read timeout 때문에 정상 종료에 최대 약 30초가 걸릴 수 있다.
    for _ in {1..200}; do
        pgrep -x pi-server >/dev/null || break
        sleep 0.25
    done
    if pgrep -x pi-server >/dev/null; then
        echo "pi-server did not stop within 50 seconds" >&2
        exit 1
    fi
elif pgrep -x pi-server >/dev/null; then
    echo "pi-server is already running (PID $(pgrep -x pi-server | head -1))"
    exit 0
fi

if [[ ! -f ./cmake-build/CMakeCache.txt ]]; then
    cmake -S . -B cmake-build
fi

# 소스 변경이 실행 바이너리에 빠지는 일을 막기 위해 매번 증분 빌드한다.
cmake --build cmake-build --target pi-server -j2

load_env_file "$PI_SERVER_ENV_DIR/.env.public"
load_env_file "$PI_SERVER_ENV_DIR/.env.private"

slot_config_path="${PARKING_SLOT_CONFIG:-${PARKING_SLOTS_CONFIG:-config/parking_slots.json}}"
if [[ "$slot_config_path" != /* ]]; then
    slot_config_path="$ROOT_DIR/$slot_config_path"
fi
if [[ ! -f "$slot_config_path" ]]; then
    echo "Missing parking slot configuration: $slot_config_path" >&2
    echo "Set PARKING_SLOT_CONFIG or restore config/parking_slots.json." >&2
    exit 1
fi

mkdir -p "$ROOT_DIR/data/logs" "$ROOT_DIR/data/db"

if [[ ! -f "$PI_SERVER_ENV_DIR/.env.private" ]]; then
    echo "WARNING: .env.private 없음 - Gemini 키와 카메라 주소/계정이 빈 값입니다." >&2
    echo "         README.md의 '실행' 절을 보고 팀에서 공유받은 값으로 만드세요." >&2
fi

export CAPTURE_SCHED_ENABLED="${CAPTURE_SCHED_ENABLED:-true}"
export HALL_CAPTURE_OCR_ENABLED="${HALL_CAPTURE_OCR_ENABLED:-true}"
export CAPTURE_OFFSETS_SEC="${CAPTURE_OFFSETS_SEC:-30,60}"
export SENSOR_LINK_MODE="${SENSOR_LINK_MODE:-uart}"
export SENSOR_UART_BAUD="${SENSOR_UART_BAUD:-115200}"

if [[ -z "${SENSOR_UART_DEVICE:-}" || ! -e "${SENSOR_UART_DEVICE}" ]]; then
    shopt -s nullglob
    stm32_devices=(/dev/serial/by-id/*STMicroelectronics*STM32* /dev/ttyACM*)
    shopt -u nullglob
    if (( ${#stm32_devices[@]} > 0 )); then
        export SENSOR_UART_DEVICE="${stm32_devices[0]}"
    else
        export SENSOR_UART_DEVICE="/dev/ttyACM0"
    fi
fi

mkdir -p data/logs
echo "Starting pi-server (UART: $SENSOR_UART_DEVICE @ $SENSOR_UART_BAUD)"

stdbuf -oL -eL ./cmake-build/pi-server 2>&1 \
    | stdbuf -oL tee -a data/logs/pi-server.log

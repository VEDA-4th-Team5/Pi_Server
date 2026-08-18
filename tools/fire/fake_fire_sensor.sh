#!/usr/bin/env bash
# STM32 가 아직 Pi 에 물려 있지 않은 동안, UART 수신 경로를 그대로 쓰면서
# 화재 프레임만 대신 흘려보내는 벤치용 도구다.
#
# SensorLinkManager 는 FIFO 든 /dev/ttyAMA0 든 같은 코드로 읽으므로,
# 나중에 실제 UART 를 물릴 때 Pi 서버 코드는 바뀌지 않는다.
#
# 사용법:
#   1) 터미널 A(PTY pair는 실제 termios UART와 같은 인터페이스를 제공한다):
#        tools/fire/fake_fire_sensor.sh --create-pty /tmp/fake-uart-rx /tmp/fake-uart-tx
#        FIRE_ALARM_ENABLED=true \
#        SENSOR_UART_DEVICE=/tmp/fake-uart-rx \
#        FIRE_SENSOR_SLOT_MAP='FIRE01=EV01,FIRE02=EV02' \
#          ./build/pi-server
#   2) 터미널 B:
#        tools/fire/fake_fire_sensor.sh /tmp/fake-uart-tx FIRE01 detected
#        tools/fire/fake_fire_sensor.sh /tmp/fake-uart-tx FIRE01 cleared
#        tools/fire/fake_fire_sensor.sh --loop /tmp/fake-uart-tx FIRE01
#   3) 확인:
#        mosquitto_sub -h localhost -t 'parking/fire/#' -v

set -euo pipefail

usage() {
    cat <<'USAGE'
usage:
  fake_fire_sensor.sh --create-pty <server_device> <writer_device>
  fake_fire_sensor.sh --create-fifo <device>
  fake_fire_sensor.sh <device> <sensor_id> <detected|cleared> [sequence]
  fake_fire_sensor.sh --loop <device> <sensor_id> [period_sec]
USAGE
    exit 1
}

emit() {
    local device="$1" sensor_id="$2" state="$3" sequence="$4"
    local frame="FIRE:${sensor_id}:$(printf '%s' "$state" | tr '[:lower:]' '[:upper:]'):${sequence}:$(($(date +%s) * 1000))"
    printf '%s\n' "$frame" >"$device"
    printf 'sent: %s\n' "$frame"
}

[ $# -ge 1 ] || usage

case "$1" in
    --create-pty)
        [ $# -eq 3 ] || usage
        command -v socat >/dev/null 2>&1 || {
            printf 'socat is required for PTY UART simulation\n' >&2
            exit 1
        }
        [ ! -e "$2" ] && [ ! -L "$2" ] &&
            [ ! -e "$3" ] && [ ! -L "$3" ] || {
            printf 'PTY link already exists; choose unused paths\n' >&2
            exit 1
        }
        exec socat -d -d \
            "pty,raw,echo=0,link=$2" "pty,raw,echo=0,link=$3"
        ;;
    --create-fifo)
        [ $# -eq 2 ] || usage
        printf 'warning: FIFO is not compatible with the production termios UART driver; use --create-pty\n' >&2
        rm -f "$2"
        mkfifo "$2"
        printf 'fifo created: %s\n' "$2"
        ;;
    --loop)
        [ $# -ge 3 ] || usage
        device="$2"
        sensor_id="$3"
        period="${4:-10}"
        sequence=1
        # DETECTED / CLEARED 를 번갈아 보내 상태 전이와 중복 억제를 함께 본다.
        while true; do
            emit "$device" "$sensor_id" detected "$sequence"
            sequence=$((sequence + 1))
            sleep "$period"
            emit "$device" "$sensor_id" cleared "$sequence"
            sequence=$((sequence + 1))
            sleep "$period"
        done
        ;;
    -h|--help)
        usage
        ;;
    *)
        [ $# -ge 3 ] || usage
        emit "$1" "$2" "$3" "${4:-1}"
        ;;
esac

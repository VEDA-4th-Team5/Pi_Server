#!/usr/bin/env bash
# STM32 가 아직 Pi 에 물려 있지 않은 동안, UART 수신 경로를 그대로 쓰면서
# 화재 프레임만 대신 흘려보내는 벤치용 도구다.
#
# SensorLinkManager 는 FIFO 든 /dev/ttyAMA0 든 같은 코드로 읽으므로,
# 나중에 실제 UART 를 물릴 때 Pi 서버 코드는 바뀌지 않는다.
#
# 사용법:
#   1) 터미널 A:
#        tools/fake_fire_sensor.sh --create-fifo /tmp/fake-uart
#        FIRE_ALARM_ENABLED=true \
#        FIRE_UART_DEVICE=/tmp/fake-uart \
#        FIRE_SENSOR_SLOT_MAP='FIRE01=EV01,FIRE02=EV02' \
#          ./build/pi-server
#   2) 터미널 B:
#        tools/fake_fire_sensor.sh /tmp/fake-uart FIRE01 detected
#        tools/fake_fire_sensor.sh /tmp/fake-uart FIRE01 cleared
#        tools/fake_fire_sensor.sh --loop /tmp/fake-uart FIRE01
#   3) 확인:
#        mosquitto_sub -h localhost -t 'parking/fire/#' -v

set -euo pipefail

usage() {
    cat <<'USAGE'
usage:
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
    --create-fifo)
        [ $# -eq 2 ] || usage
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

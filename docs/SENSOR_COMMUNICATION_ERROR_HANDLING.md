# 센서·통신 오류 예외처리 및 이벤트 로깅

## 구현 범위

1차 구현은 홀센서, UART, UART 기반 LoRa 입력을 대상으로 한다. 오류 복구는 기존
`SensorLinkManager`가 담당하고, `SystemEventReporter`는 발생·복구 사실을 비동기로
SQLite `EVENT_LOG`에 저장한다. MQTT/RTSP 장애의 동일 reporter 연결과 Qt 시스템 상태
토픽 발행은 후속 범위다.

```text
HallParkingService / SensorLinkManager
             ↓ report() noexcept
SystemEventReporter bounded queue (기본 100개)
             ↓ 전용 worker
EventDatabase::insertSystemEvent()
             ↓
SQLite EVENT_LOG
```

## 안전 원칙

- `report()`는 예외를 호출자에게 전달하지 않는다.
- 센서·UART worker에서 SQLite I/O를 직접 수행하지 않는다.
- reporter sink 실패를 reporter에 다시 보고하지 않아 재귀를 차단한다.
- DB 실패 시 queue의 이벤트를 기본 1초 간격으로 재시도한다.
- queue가 가득 차면 오래된 비치명 이벤트를 제거하며 최대 크기를 유지한다.
- 같은 source/code/transport/device/slot 오류는 기본 30초 동안 억제한다.
- 연결 복구 이벤트는 즉시 저장하고 해당 장치의 억제 상태를 초기화한다.
- 센서 오류 메시지에는 RTSP URL, 비밀번호, API Key 같은 비밀값을 넣지 않는다.

## 현재 이벤트 코드

| 코드 | 발생 조건 | 동작 |
|---|---|---|
| `SENSOR_MESSAGE_INVALID` | 형식 또는 필드가 잘못된 센서 메시지 | 메시지 폐기, 세션 생성 금지 |
| `SENSOR_NOT_MAPPED` | 활성 주차면에 매핑되지 않은 sensor ID | 메시지 폐기 |
| `SENSOR_SEQUENCE_REJECTED` | 중복 또는 과거 sequence | 상태 전이 차단 |
| `SENSOR_HANDLER_FAILED` | Snapshot, DB, handler 처리 실패 | 현재 이벤트 실패, 서버 유지 |
| `UART_OPEN_FAILED` | UART 장치 open/config 실패 | 재연결 대기 |
| `UART_READ_FAILED` | 연결된 UART read 실패 | 연결 해제 후 재연결 |
| `UART_WRITE_FAILED` | STM32/LoRa alert 명령 송신 실패 | 호출자에 false 반환 및 기록 |
| `UART_BUFFER_OVERFLOW` | 개행 전 입력이 4096 bytes 초과 | buffer 폐기 |
| `UART_RECOVERED` | 실패 이후 UART 재연결 | 복구 및 재시도 횟수 기록 |
| `LORA_FRAME_REJECTED` | version, length 또는 CRC 검증 실패 | 프레임 폐기 후 SOF 재동기화 |

## EVENT_LOG 저장 형식

기존 DB schema를 변경하지 않는다.

```text
event_type = UART_OPEN_FAILED
slot_id    = 확인할 수 없으면 NULL
message    = 아래 JSON
handled    = 0
```

```json
{
  "source": "UART",
  "severity": "ERROR",
  "transport": "uart-line",
  "device": "/dev/ttyUSB0",
  "message": "device unavailable",
  "retry_count": 3,
  "suppressed_count": 4,
  "dropped_count": 0,
  "recovered": false
}
```

## 빌드 및 자동 테스트

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

핵심 테스트:

- `system-event-reporter-test`
  - 동일 오류 억제와 누적 횟수
  - 복구 이벤트
  - sink 실패 재시도
  - sink 예외가 worker 밖으로 나오지 않는지 검증
- `uart-lora-driver-test`
  - PTY UART 송수신과 재연결 기반
  - LoRa CRC 오류 폐기 및 다음 frame 재동기화
- `hall-timer-integration-test`
  - 손상된 센서 메시지가 세션을 만들지 않는지 확인
  - `SENSOR_MESSAGE_INVALID`가 실제 SQLite `EVENT_LOG`에 한 번만 저장되는지 확인

특정 테스트만 실행:

```bash
ctest --test-dir build -R \
  'system-event-reporter|uart-lora-driver|hall-timer-integration' \
  --output-on-failure
```

## 실제 UART 확인

```bash
export SENSOR_LINK_MODE=uart-line
export SENSOR_UART_DEVICE=/dev/ttyUSB0
export SENSOR_UART_BAUD=115200
./build/pi-server
```

다른 터미널에서 최근 통신 이벤트를 확인한다.

```bash
sqlite3 data/db/parking.db \
  "SELECT event_id,event_type,slot_id,message,created_at FROM EVENT_LOG WHERE event_type LIKE 'UART_%' OR event_type LIKE 'SENSOR_%' OR event_type LIKE 'LORA_%' ORDER BY event_id DESC LIMIT 20;"
```

장치를 분리하면 `UART_READ_FAILED` 또는 `UART_OPEN_FAILED`, 다시 연결하면
`UART_RECOVERED`가 기록되어야 한다.

## 남은 작업

- Mosquitto 연결·발행 오류를 동일 reporter에 연결
- RTSP 채널별 `DISCONNECTED/RECOVERED` 연결
- 운영 장애만 Qt의 `parking/v1/system/state`에 retain 발행
- 오류 발생률과 queue drop 수를 health API에 노출

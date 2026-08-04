# UART / LoRa Link Protocol

기준일: 2026-07-22

## 책임 분리

- `UartDriver`: Linux `/dev/tty*`, termios raw 8N1, poll, partial read/write 담당
- `LoRaDriver`: UART byte stream 위의 frame 경계, 길이, sequence, CRC16 담당
- `SensorLinkManager`: 재연결, UART line/LoRa frame 수신 thread, 센서 handler 전달
- `HallParkingService`: 센서 ID 매핑과 실제 입·출차 업무 처리

현재 LoRa 구현은 **투명 UART LoRa 모뎀**을 전제로 한다. SX127x 같은 SPI LoRa
칩의 주파수, spreading factor, bandwidth와 무선 레지스터를 설정하는 드라이버가 아니다.
그 값은 모뎀 자체 또는 향후 칩셋 전용 드라이버에서 설정해야 한다.

## 실행 모드

`SENSOR_LINK_MODE`:

- `off`: 실제 직렬 입력을 사용하지 않음
- `uart-line`: 개행으로 끝나는 기존 센서 문자열을 UART에서 직접 수신
- `lora-frame`: 아래 binary frame의 SensorEvent payload를 수신

UART 기본값:

```text
device=/dev/ttyAMA0
baud=115200
data=8 bit
parity=none
stop=1 bit
flow-control=none
```

## UART line 시험 규약

```text
SENSOR:HALL01:OCCUPIED:1\n
SENSOR:HALL01:VACANT:2\n
```

partial read와 한 번에 여러 줄이 들어오는 경우 모두 처리한다. sequence는 센서별로
단조 증가해야 하며 중복·역순 값은 기존 `ParkingSensorSequenceGuard`가 거부한다.

## LoRa binary frame

모든 다중 byte 정수는 network byte order(big-endian)다.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | SOF `0xAA` |
| 1 | 1 | SOF `0x55` |
| 2 | 1 | Version `0x01` |
| 3 | 1 | Message type |
| 4 | 4 | Transport sequence |
| 8 | 2 | Payload length, 최대 512 |
| 10 | N | Payload |
| 10+N | 2 | CRC16-CCITT |

CRC 범위는 `Version`부터 payload 마지막 byte까지이며 초기값은 `0xFFFF`,
polynomial은 `0x1021`이다. CRC가 틀리거나 payload가 512 byte를 넘으면 frame을
폐기하고 다음 `AA 55`에서 재동기화한다.

Message type:

- `0x01`: SensorEvent
- `0x02`: AlertCommand
- `0x03`: Heartbeat

SensorEvent payload는 기존 센서 문자열이다.

```text
SENSOR:HALL01:OCCUPIED:15
```

payload 안의 sequence는 센서별 중복 방지에 사용된다. frame sequence는 무선 전송
진단 및 송수신 추적용이다. STM32 구현에서는 두 값을 동일하게 사용하는 것을 권장한다.

AlertCommand payload 예시:

```text
ALERT:EV01:ON
ALERT:EV01:OFF
HEARTBEAT
```

물리 LED/Buzzer 명령은 STM32 규약이 확정된 뒤 이 payload 종류를 확장한다.

## 설정 예시

직접 UART 시험:

```bash
export SENSOR_LINK_MODE=uart-line
export SENSOR_UART_DEVICE=/dev/ttyUSB0
export SENSOR_UART_BAUD=115200
```

LoRa frame 시험:

```bash
export SENSOR_LINK_MODE=lora-frame
export SENSOR_UART_DEVICE=/dev/ttyUSB0
export SENSOR_UART_BAUD=115200
```

## C++ 진단 도구

```bash
./build/parking-link-tool encode sensor 1 'SENSOR:HALL01:OCCUPIED:1'
./build/parking-link-tool send /dev/ttyUSB0 115200 sensor 1 \
  'SENSOR:HALL01:OCCUPIED:1'
./build/parking-link-tool listen /dev/ttyUSB0 115200
```

서버와 진단 도구가 같은 UART 장치를 동시에 열지 않도록 한다.

## 아직 확정되지 않은 항목

- 실제 LoRa 모듈 모델과 무선 설정
- Pi에서 사용할 최종 UART 장치 경로
- STM32 LED/Buzzer 명령과 응답/ACK 규약
- 화재 센서 payload
- 암호화, 송신 재시도 및 무선 ACK 정책

이 항목들은 하드웨어 규격을 받기 전까지 코드에 하드코딩하지 않는다.

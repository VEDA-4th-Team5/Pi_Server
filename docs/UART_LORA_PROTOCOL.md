# UART / LoRa Link Protocol

기준일: 2026-08-06

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

## 번호판 조명 LED 명령 (Pi -> STM32)

야간 저조도에서 번호판 OCR 성공률을 올리기 위해 Pi가 촬영 직전 LED를 켜고 촬영 직후
끈다. AlertCommand(`0x02`) payload를 아래 두 종류로 확장한다.

```text
ALERT:HALL01:LED:ON:12
ALERT:HALL01:LED:OFF:12
```

- 두 번째 필드는 센서 ID로 `SENSOR:HALL01:OCCUPIED:1` 과 같은 값을 사용한다. 주차면마다
  LED를 독립 제어하기 위해 필요하다.
- 마지막 필드는 sequence다. 하나의 촬영에서 ON 과 뒤따르는 OFF 는 같은 값을 쓰므로
  두 명령을 짝지어 추적할 수 있다. 값은 Pi 안에서 단조 증가한다.
- ACK 는 요구하지 않는다. Pi 는 ON 을 보낸 뒤 고정 정착 지연만 두고 촬영을 진행한다.

**점등 구간**: Pi 는 실제 노출 호출(카메라 스냅샷 API 의 이미지 생성 요청 또는 RTSP
프레임 획득)만 ON 과 OFF 로 감싼다. 파일 저장과 DB 기록은 조명이 필요없고 시간이
길어질 수 있어 점등 구간에서 제외한다.

**STM32 페일세이프 (필수)**: STM32 는 LED ON 을 처리할 때 자체 타이머를 걸고, Pi 의
OFF 가 오지 않아도 정해진 시간 뒤 강제로 소등해야 한다. 링크 유실이나 Pi 재시작으로
OFF 가 유실되면 LED 가 계속 켜진 채 남기 때문이다.

타이머 값은 `PLATE_LED_SETTLE_MS` 와 실측 노출 소요시간을 합친 값보다 커야 한다.
`CAMERA_SNAPSHOT_REQUEST_TIMEOUT_MS` 기본값이 30초이므로 카메라가 느리면 노출 호출이
페일세이프보다 길어질 수 있다. 이 경우 STM32 가 먼저 소등하고 Pi 의 OFF 는 이미 꺼진
LED 에 도착한다. 실제 촬영은 호출 초반에 일어나므로 인식 자체는 영향을 받지 않지만,
현장에서 노출 지연을 측정한 뒤 페일세이프 값을 확정한다(초기 권장 5초).

같은 센서에 대해 ON 이 연속으로 오면 STM32 는 타이머만 갱신하고 점등 상태를 유지한다.
점등 상태가 아닐 때 도착한 OFF 는 무시한다. 촬영 재시도로 ON/OFF 가 몇 초 안에 여러 번
반복될 수 있으므로 두 경우 모두 오류로 처리하지 않는다.

**적용 범위**: 조명은 Pi 가 직접 노출을 거는 예약 촬영 경로에만 붙는다. 촬영을 카메라가
수행하는 MQTT 촬영 요청 경로에서는 Pi 가 노출 시점을 알 수 없어 점등해도 노출과
어긋나므로 LED 명령을 보내지 않는다.

Buzzer 명령은 아직 범위 밖이며 규약이 정해지지 않았다.

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
- STM32 Buzzer 명령 규약 (LED 명령은 위 절에서 확정)
- STM32 페일세이프 타이머의 최종 값과 LED 구동 회로 규격
- 화재 센서 payload
- 암호화, 송신 재시도 및 무선 ACK 정책

이 항목들은 하드웨어 규격을 받기 전까지 코드에 하드코딩하지 않는다.

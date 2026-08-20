# UART / LoRa Link Protocol

기준일: 2026-08-13 (STM <-> Pi LoRa 연동 규격 v1.1 반영, EVDA-46/EVDA-201)

노드가 STM1/STM2 둘이며, 페이로드에 노드 ID가 들어간다. `seq`는 노드당 단일
카운터로 홀·화재가 공유한다. 하행 명령은 목적지 노드를 골라 fixed-point 주소
헤더를 붙여 보내야 한다.

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
단, 직전 값보다 `kLegacySequenceRebootDropThreshold`(1000) 넘게 줄어들면 역순이
아니라 STM32 재부팅으로 보고 새 카운터를 그대로 받아들인다(§ 노드/재부팅 처리 참고).

## LoRa binary frame

모든 다중 byte 정수는 network byte order(big-endian)다.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | SOF `0xAA` |
| 1 | 1 | SOF `0x55` |
| 2 | 1 | Version `0x01`(레거시) 또는 `0x02`(v1.1) |
| 3 | 1 | Message type |
| 4 | 4 | Transport sequence |
| 8 | 2 | Payload length, 최대 512 |
| 10 | N | Payload |
| 10+N | 2 | CRC16-CCITT |

CRC 범위는 `Version`부터 payload 마지막 byte까지이며 초기값은 `0xFFFF`,
polynomial은 `0x1021`이다. CRC가 틀리거나 payload가 512 byte를 넘으면 frame을
폐기하고 다음 `AA 55`에서 재동기화한다.

`LoRaDriver::consume()`은 상행에서 `0x01`/`0x02`를 모두 받는다("보낼 때는 좁게,
받을 때는 넓게"). `encode()`/`send()`는 계속 `0x01`을 찍는다 — STM 쪽이 하행에서
두 버전을 모두 받도록 이미 고쳐졌으므로 STM과 Pi를 같은 순간에 배포하지 않아도
된다.

Message type:

- `0x01`: SensorEvent
- `0x02`: AlertCommand
- `0x03`: Heartbeat

### SensorEvent payload

v1.0(레거시, 노드 ID 없음):

```text
SENSOR:HALL01:OCCUPIED:15
FIRE:FLAME01:DETECTED:12
```

v1.1(노드 ID·화재 energy 추가, version `0x02`에서 사용):

```text
SENSOR:<node>:<sensor>:<state>:<seq>
FIRE:<node>:<sensor>:<state>:<seq>:<energy>

SENSOR:STM1:HALL01:OCCUPIED:15
FIRE:STM1:FLAME01:DETECTED:26:42.09
```

- `<node>`는 `STM1`|`STM2`. `SensorProtocolParser`는 두 번째 필드가 이 값인지로
  두 grammar를 구분한다(out-of-band 버전 태그 없이). 센서 ID가 노드를 가로질러
  이미 전역 고유하므로(`config/parking_slots.json`) 슬롯 매핑에는 쓰이지 않고
  `SensorProtocolMessage::node` / `FireSensorMessage::node`에 진단용으로만
  담긴다.
- `<energy>`는 화재 센서 FFT의 1~20Hz 대역 에너지 합(표시용, 소수 둘째 자리)이다.
  화재 판정은 STM32가 하므로 Pi는 이 값으로 재판정하지 않는다 — `<state>`만
  근거로 삼는다. `FireSensorMessage::energy`에 담기며 도메인 로직에는 전달하지
  않는다.
- payload 안의 sequence는 **노드당 단일 카운터**다(홀 채널 2개와 화재가 공유).
  frame sequence는 무선 전송 진단 및 송수신 추적용이다. STM32 구현에서는 두 값을
  동일하게 사용하는 것을 권장한다.

AlertCommand payload 예시:

```text
ALERT:EV01:ON
ALERT:EV01:OFF
HEARTBEAT
```

## 목적지 주소 지정 (Pi -> STM, 고정점 모드)

노드가 둘이므로 하행 project frame 앞에 3 byte 목적지 헤더를 반드시 붙여야
한다. 빠뜨리면 모듈이 project frame의 앞 3 byte(`AA 55 0x`)를 주소로 오인해서
하행이 통째로 나가지 않는다.

```text
<ADDH> <ADDL> <CH>  +  project frame

STM1 로 : 00 01 1E
STM2 로 : 00 02 1E        (0x1E = 채널 30)
```

`LoRaDriver::sendTo(destination, frame)`이 이 헤더를 붙여 한 번에 write한다.
목적지는 AlertCommand payload의 센서 번호로 정해진다(§ LED 명령 표 참고):
`device::SensorLinkManager::sendAlertCommand()`가 payload를 보고 목적지를
고른 뒤 `sendTo()`를 호출하며, 알려진 센서가 아니면(HALL01~04 밖) 예전처럼
주소 없이 `send()`로 보낸다.

> 두 노드가 같은 채널을 듣는다. 목적지를 STM2로 지정해도 STM1 모듈이 전파를
> 수신할 수 있지만, STM 펌웨어가 센서 번호로 자기 담당인지 판별해 무시하므로
> 동작에는 문제가 없다. 노드가 늘면 이 필터가 유일한 방어선이므로 목적지
> 주소는 정확히 지정한다.

## 번호판 조명 LED 명령 (Pi -> STM32)

야간 저조도에서 번호판 OCR 성공률을 올리기 위해 Pi가 촬영 직전 LED를 켜고 촬영 직후
끈다. AlertCommand(`0x02`) payload를 아래 두 종류로 확장한다.

```text
ALERT:HALL01:LED:ON:12
ALERT:HALL01:LED:OFF:12
```

- 두 번째 필드는 센서 ID로 `SENSOR:HALL01:OCCUPIED:1` 과 같은 값을 사용한다. 주차면마다
  LED를 독립 제어하기 위해 필요하다. 센서 번호로 목적지 노드도 함께 정해진다:

  | 센서 | 목적지 노드 | 고정점 헤더 |
  |---|---|---|
  | `HALL01`, `HALL02` | STM1 | `00 01 1E` |
  | `HALL03`, `HALL04` | STM2 | `00 02 1E` |
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

단일 frame 인코딩·송수신 확인:

```bash
./cmake-build/parking-link-tool encode sensor 1 'SENSOR:HALL01:OCCUPIED:1'
./cmake-build/parking-link-tool send /dev/ttyUSB0 115200 sensor 1 \
  'SENSOR:HALL01:OCCUPIED:1'
./cmake-build/parking-link-tool listen /dev/ttyUSB0 115200
```

대화형 LoRa 수신·CRC/sequence 통계 및 HALL01~04 LED 명령 확인:

```bash
cmake --build cmake-build --target lora-console -j4
./cmake-build/lora-console --device /dev/serial0 --baud 115200
```

콘솔에서는 `1`~`4`로 해당 Hall LED를 토글하고, `on 2`, `off 2`, `raw <payload>`,
`s`, `q` 명령을 사용할 수 있다. HALL01~02는 STM1, HALL03~04는 STM2 목적지
주소로 전송한다. 기본 LoRa 채널은 30(`0x1E`)이다.

다른 주소 또는 채널을 직접 지정할 수 있다.

```bash
./cmake-build/lora-console --device /dev/serial0 --address 2 --channel 30
```

서버와 진단 도구가 같은 UART 장치를 동시에 열지 않도록 한다.

## 아직 확정되지 않은 항목

- 실제 LoRa 모듈 모델과 무선 설정
- Pi에서 사용할 최종 UART 장치 경로
- STM32 Buzzer 명령 규약 (LED 명령은 위 절에서 확정)
- STM32 페일세이프 타이머의 최종 값과 LED 구동 회로 규격
- 암호화, 송신 재시도 및 무선 ACK 정책
- STM2 노드 실기기 검증, 2노드 동시 운용(반이중 충돌) 검증

이 항목들은 하드웨어 규격을 받기 전까지 코드에 하드코딩하지 않는다.

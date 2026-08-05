# VEDA Smart Parking Pi Server

Hanwha Vision CCTV 기반 스마트 주차 관제 시스템의 Raspberry Pi C++ 서버입니다.
RTSP, MQTT, OpenCV, Gemini OCR, SQLite 및 Qt 조회 API를 사용합니다.

## 현재 구현

- 카메라 RTSP 영상 수신
- MQTT 및 ONVIF Metadata 이벤트 수신
- 차량·번호판 BestShot 다운로드
- IVA Snapshot과 OpenCV 전처리 이미지 저장
- Gemini HTTPS 번호판 OCR
- 로컬 DB 기반 EV / NON_EV / UNKNOWN 판별
- SQLite 주차 세션·이미지·이벤트 저장
- Qt용 HTTP/HTTPS 상태·이미지 조회 API
- 가짜 홀센서 MQTT 입력과 실제 Snapshot·세션·타이머 연결
- OCCUPIED 확정 유예시간과 T0+30초/60초 RTSP ROI 촬영·OCR scheduler
- CV5 Snapshot OpenAPI의 동일 프레임 original/enhanced 다운로드 및 OCR 연결
- 실제 UART line 및 UART 기반 LoRa CRC frame 수신 드라이버
- 프로젝트 전용 Linux Character Device `/dev/parking_alert`
- Qt용 주차 상태/위반 MQTT 이벤트 발행
- 1시간 이전 출차 시 임시 이미지와 IMAGE_LOG 정리

아직 STM32·LoRa 실물 검증, 32면 최종 모델, 화재 처리 및 영상 클립 저장은
구현되지 않았습니다.
- 센서 메시지 파서와 주차 상태 단위 테스트
- STM32 UART 화재 후보 수신 및 Qt MQTT 알림 (초안)

아직 32면 최종 모델, LoRa 및 영상 클립 저장은 구현되지 않았습니다.
STM32 실장비는 아직 연결되어 있지 않아 화재 경로는 FIFO 시뮬레이터로만
검증했습니다.

## 주요 흐름

```text
Camera
→ RTSP / MQTT / BestShot / CV Snapshot OpenAPI
→ Raspberry Pi C++ Server
→ 카메라 내부 CV 개선본 다운로드 (기본 비활성, CAP 설치 후 활성)
→ Gemini OCR
→ SQLite 및 이미지 파일 저장
→ Qt HTTP/HTTPS 조회
```

실제 UART 연결 전에는 `config/parking_slots.json`의 센서-슬롯 매핑과 다음 개발용
MQTT 메시지로 동일한 센서 업무 흐름을 검증합니다.

```text
SENSOR:HALL01:OCCUPIED:1
→ 설정된 유예시간 동안 VACANT 없이 유지되면 OCCUPIED 확정
→ EV01 최신 ROI Snapshot 저장
→ T0+30초 카메라 original/enhanced 촬영 및 첫 Gemini OCR
→ 실패 시 T0+60초 동일 방식으로 Gemini OCR 재시도
→ EV/PHEV이면 동일 SQLite session_id로 장기 점유 타이머 등록
→ 제한시간 초과 시 최신 Snapshot 추가 저장
→ parking/v1/events/EV01 및 parking/v1/state/EV01 MQTT 알림
→ Qt가 session_images_url을 HTTP로 조회
```

번호판 OCR이 EV/PHEV로 확인되면 같은 `PARKING_SESSION.session_id`를 장기 점유
타이머에 등록합니다. 별도의 세션을 다시 만들지 않으며, 서버 재시작 시 활성 세션도
남은 시간을 기준으로 복구합니다. 독립 실행형 `parking-timer` 역시 계속 빌드됩니다.

## 빌드 및 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j2
ctest --test-dir cmake-build --output-on-failure
```

필요 패키지에는 OpenCV, FFmpeg, libcurl, Mosquitto, SQLite3 및
`libcpp-httplib-dev`가 포함됩니다.

## 실행

현재 로컬 카메라·Gemini 설정과 STM32 UART 자동 탐색을 적용해 실행합니다.
`.env.fire.local`이 있으면 화재·홀센서 시험 설정도 함께 적용합니다.

```bash
./run_server.sh
```

실행 중인 서버를 종료하고 같은 설정으로 다시 시작하려면 다음을 사용합니다.

```bash
./run_server.sh restart
```

비밀정보는 다음 로컬 파일에서 관리하며 Git에 커밋하지 않습니다.

```text
.env.camera.local
.env.iva.local
.env.gemini.local
```

```bash
set -a
source ./.env.camera.local
[ -f ./.env.iva.local ] && source ./.env.iva.local
source ./.env.gemini.local
set +a

./cmake-build/pi-server
```

장기 점유 타이머의 기본 제한시간은 3600초입니다.

```bash
export PARKING_TIMER_ENABLED=true
export PARKING_OVERSTAY_THRESHOLD_SECONDS=3600
export PARKING_HALL_WORK_QUEUE_CAPACITY=100
export PARKING_OCCUPANCY_CONFIRM_MS=10000
export CAPTURE_SCHED_ENABLED=true
export HALL_CAPTURE_OCR_ENABLED=true
# 실기기 반복 시험에서만 예: export CAPTURE_OFFSETS_SEC=5,10
```

확정된 입차는 최신 RTSP FrameBuffer의 ROI를
`OCCUPANCY_START_EVIDENCE`로 한 번 저장한다. 세션이 계속 활성 상태이면 T0 기준
`overstay_threshold_seconds` 뒤에 `OVERSTAY_EVIDENCE`를 한 번 더 저장하고 같은
시점에 장기 점유 위반을 판정한다. 값은 SQLite `SYSTEM_SETTINGS`에 저장되며 Qt가
`GET/PUT /api/v1/settings/overstay-threshold`로 60~86400초 범위에서 변경한다.
변경값은 활성 세션과 이후 신규 세션 모두에 원래 입차시각 T0 기준으로 적용된다.

재시작 시 활성 세션의 원래 T0를 기준으로 아직 없는 증거 작업을 복원한다. 조기 출차는
대기 중인 Job을 즉시 제거하며, 홀센서 비동기 큐는 같은 슬롯의 최신 상태를 병합하고
서로 다른 슬롯이 설정 용량을 넘을 때 `HALL_WORK_QUEUE_OVERFLOW`를 기록한다.

`CAMERA_SNAPSHOT_API_ENABLED=true`이면 촬영 scheduler는 CV5 카메라의
`/images/generate`를 호출하고 같은 프레임의 original/enhanced JPEG를 즉시 내려받아
파일·IMAGE_LOG·Gemini OCR로 연결한다. 개선본이 제공되므로 Hall OCR에서는 Pi의
OpenCV 화질 개선을 실행하지 않는다. API가 비활성이면 기존 RTSP FrameBuffer 촬영을
유지한다. 상세 설정과 실기기 검증 절차는
[`docs/CAMERA_SNAPSHOT_API_INTEGRATION.md`](docs/CAMERA_SNAPSHOT_API_INTEGRATION.md)에 있다.

가짜 홀센서 입력:

```bash
mosquitto_pub -h localhost -t parking/sensor/hall -q 1 \
  -m 'SENSOR:HALL01:OCCUPIED:1'
mosquitto_pub -h localhost -t parking/sensor/hall -q 1 \
  -m 'SENSOR:HALL01:VACANT:2'
```

Qt MQTT 구독:

```text
parking/v1/events/+  # QoS 1, 순간 이벤트
parking/v1/state/+   # QoS 1, retain된 최신 상태
```

1시간 이전에 `VACANT`가 오면 원본·전처리 파일과 `IMAGE_LOG`를 삭제합니다.
이미 `VIOLATION`이 된 세션의 입차/위반 증거는 출차 후에도 보존합니다.

실제 UART 또는 투명 UART LoRa 모뎀을 사용하려면 다음 중 하나를 선택합니다.

```bash
export SENSOR_LINK_MODE=uart-line   # 개행 SENSOR 문자열
# export SENSOR_LINK_MODE=lora-frame # CRC16 binary frame
export SENSOR_UART_DEVICE=/dev/ttyUSB0
export SENSOR_UART_BAUD=115200
```

상세 frame 규격과 C++ 진단 도구는
[`docs/UART_LORA_PROTOCOL.md`](docs/UART_LORA_PROTOCOL.md)를 참고하십시오.

주차 알림용 독립 문자 디바이스는 `parking_alert.ko`를 적재해 생성합니다.

```bash
cmake --build build --target parking-alert-module
sudo insmod driver/parking_alert/parking_alert.ko
sudo ./build/parking-alert-ctl status
```

ABI, 권한 설정, `read/write/ioctl/poll` 검증 방법은
[`docs/PARKING_ALERT_DRIVER.md`](docs/PARKING_ALERT_DRIVER.md)를 참고하십시오.
### 화재 알림 (STM32 UART)

기본값은 비활성이며, 다음 환경변수로 켭니다.

| 변수 | 기본값 | 설명 |
|---|---|---|
| `FIRE_ALARM_ENABLED` | `false` | 화재 경로 전체 on/off |
| `FIRE_UART_DEVICE` | `/dev/ttyAMA0` | STM32 UART 장치. 테스트 시 FIFO 경로 |
| `FIRE_UART_BAUD` | `115200` | 9600/19200/38400/57600/115200 |
| `FIRE_TOPIC_PREFIX` | `parking/fire` | 화재 최신 상태 토픽 접두사 |
| `FIRE_SENSOR_CHANNEL_MAP` | (없음) | `FLAME01=ch01,FLAME02=ch02,...` |

STM32가 아직 연결되지 않은 동안에는 FIFO로 같은 수신 경로를 검증할 수 있습니다.

```bash
tools/fake_fire_sensor.sh --create-fifo /tmp/fake-uart

FIRE_ALARM_ENABLED=true \
FIRE_UART_DEVICE=/tmp/fake-uart \
FIRE_SENSOR_CHANNEL_MAP='FLAME01=ch01,FLAME02=ch02,FLAME03=ch03,FLAME04=ch04' \
  ./cmake-build/pi-server

# 다른 터미널에서
tools/fake_fire_sensor.sh /tmp/fake-uart FLAME01 detected
mosquitto_sub -h localhost -t 'parking/fire/#' -v
```

토픽과 payload 규약은 `docs/MQTT_PROTOCOL_PROPOSAL.md`에 있습니다.

## 데이터 저장

```text
data/
├── bestshots/
│   ├── vehicle/
│   └── plate/
│       └── enhanced/
├── snapshots/
│   └── ch1/EV01~EV04/
└── db/
    └── parking.db
```

`snapshots/ch1/EV01~EV04` 아래에는 슬롯마다 `scene/`, `enhanced/`가 생성됩니다.

SQLite 주요 테이블:

- `VEHICLE`
- `PARKING_SLOT`
- `PARKING_SESSION`
- `IMAGE_LOG`
- `EVENT_LOG`

## Qt 조회 API

기본 주소:

```text
http://<PI_IP>:8080
```

| Method | 경로 | 설명 |
|---|---|---|
| GET | `/api/v1/health` | 서버 상태 |
| GET | `/api/v1/parking-slots` | 전체 주차면 |
| GET | `/api/v1/parking-slots/{slot_id}` | 특정 주차면 |
| GET | `/api/v1/parking-sessions/active` | 활성 세션 |
| GET | `/api/v1/parking-sessions/{id}/images` | 세션 이미지 목록 |
| GET | `/api/v1/images/{id}/original` | 원본 이미지 |
| GET | `/api/v1/images/{id}/enhanced` | 전처리 이미지 |

Qt는 이미지 목록에서 받은 상대 URL에 Pi 서버 주소를 붙여 사진을 요청합니다.

## 현재 제한

- DB는 최종 목표인 4채널 32면 구조가 아닙니다.
- BestShot 저장 경로는 아직 채널별 P1~P4로 분리되지 않았습니다.
- UART/LoRa 소프트웨어 계층은 구현됐지만 실제 STM32·LoRa 장비 검증은 남아 있습니다.
- 알람 ACK와 STM32 부저/LED 출력은 아직 없습니다.
- `/dev/parking_alert`는 구현됐지만 타이머 위반 callback과 32면 bit 매핑은 아직 연결 전입니다.
- API 사용자 인증은 아직 없습니다.

상세 문서:

- [`docs/CAMERA_MQTT_CAPTURE_PROTOCOL.md`](docs/CAMERA_MQTT_CAPTURE_PROTOCOL.md): 카메라 MQTT 촬영 요청 목표 규약과 ROI 처리
- [`docs/CAMERA_SNAPSHOT_API_INTEGRATION.md`](docs/CAMERA_SNAPSHOT_API_INTEGRATION.md): CV5 카메라 내부 화질 개선 이미지 연동
- [`docs/GEMINI_OCR_GUIDE.md`](docs/GEMINI_OCR_GUIDE.md): OpenCV 전처리, Gemini HTTPS OCR, DB 반영과 수동 테스트
- [`docs/UART_LORA_PROTOCOL.md`](docs/UART_LORA_PROTOCOL.md): STM32 UART 및 LoRa frame 규약
- [`docs/PARKING_ALERT_DRIVER.md`](docs/PARKING_ALERT_DRIVER.md): 전용 Linux Character Device 빌드·ABI·검증
- [`docs/DEVELOPER_WALKTHROUGH.md`](docs/DEVELOPER_WALKTHROUGH.md): 전체 코드 진입점과 흐름

## 코드 이해 문서

코드와 함께 diff·검색하기 쉬운 기준 문서는
[`docs/generated/PI_SERVER_CODE_GUIDE.md`](docs/generated/PI_SERVER_CODE_GUIDE.md)입니다.
이 Markdown에는 현재 아키텍처와 Doxygen에서 자동 추출한 C/C++ 파일별 함수 목록이 들어갑니다.
배포·열람용 결과는 [`Pi_Server_Code_Guide.pdf`](Pi_Server_Code_Guide.pdf)입니다.

코드 또는 Doxygen 주석을 수정한 뒤 두 문서를 함께 다시 생성합니다.

```bash
cmake -S . -B cmake-build
cmake --build cmake-build --target docs -j2
```

중간 Doxygen XML·HTML·JavaScript는 추적하지 않는 `build/docs/`에만 생성됩니다.
Markdown과 PDF가 최종 결과물이므로 대량 생성 파일을 Git에 추가할 필요가 없습니다.

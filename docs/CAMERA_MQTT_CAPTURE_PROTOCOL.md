# Camera MQTT Capture Protocol

기준일: 2026-07-26

> 이 문서가 정의한 목표 규약 중 촬영 트리거는 이후 Camera Snapshot API
> (`/images/generate`)로 구현이 확정됐다. 실제 구현·설정은
> [`docs/CAMERA_SNAPSHOT_API_INTEGRATION.md`](CAMERA_SNAPSHOT_API_INTEGRATION.md)를
> 참고한다. 아래 내용은 draft 규약과 미구현 항목을 추적하는 기록으로 남긴다.

## 1. 문서 상태

이 문서는 홀센서 차량 감지 후 Raspberry Pi가 MQTT로 카메라 촬영을 요청하고,
수신한 사진을 슬롯 ROI 및 번호판 OCR에 사용하는 **목표 규약**을 정의한다.

아래 항목은 현재 구현과 목표 구현을 구분한다.

| 기능 | 상태 |
|---|---|
| Camera → Pi MQTT 이벤트 | 구현 및 실기기 확인 |
| Pi의 RTSP 최신 프레임 유지 | 구현 및 실기기 확인 |
| Pi에서 정규화 ROI crop | 구현 |
| Pi → Camera MQTT 촬영 요청 | draft topic/payload 발행 구현, 카메라 기능 확인 필요 |
| Camera 촬영 응답/URL 수신 | 미구현, 응답 방식 확인 필요 |
| HTTP/HTTPS Snapshot 다운로드 | BestShot은 구현, 요청형 Snapshot은 미구현 |
| 30초·60초 촬영 scheduler | 구현: steady clock 예약·조기 출차 취소·발행 실패 재시도 |
| 30초·60초 RTSP ROI 저장·OCR | 구현: 실제 SQLite session_id로 IMAGE_LOG·Gemini·타이머 연결 |

현재 `saveIvaAreaSnapshot()`은 카메라에 좌표나 촬영 명령을 보내지 않는다. Pi가
연속 수신한 RTSP 최신 프레임을 메모리에서 복사한 뒤 OpenCV로 crop한다.

현재 scheduler는 `parking/capture/{slot_id}`에 `capture_request_draft_v0` JSON을
QoS 1로 발행한다. 성공 로그는 Mosquitto가 발행 요청을 접수했다는 뜻이며, 카메라
촬영 완료나 이미지 수신 성공을 뜻하지 않는다. 응답 correlation은 EVDA-138 범위다.

다만 현재 운영 코드는 MQTT 응답을 기다리지 않고, 기존 RTSP FrameBuffer의
최신 프레임을 같은 30초·60초 시점에 ROI crop한다. `HallCaptureExecutor` →
`HallCaptureCoordinator` → `OcrWorker` 흐름이 실제 파일 저장, IMAGE_LOG,
Gemini OCR을 하나의 SQLite `session_id`로 연결한다.

## 2. 목표 흐름

```text
Hall Sensor OCCUPIED
→ STM32
→ UART 또는 LoRa
→ Pi HallParkingService
→ PARKING_SESSION 생성 및 T0 기록
→ T0+30초 Camera MQTT 촬영 요청
→ T0+60초 Camera MQTT 촬영 요청
→ Camera 응답의 Snapshot URL 확인
→ Pi가 HTTPS로 JPEG 다운로드
→ 슬롯 ROI crop
→ 번호판 후보 crop
→ OpenCV 전처리
→ Gemini OCR
→ Local VEHICLE DB 조회
→ IMAGE_LOG / EVENT_LOG 저장
```

카메라가 MQTT 명령을 직접 촬영 trigger로 사용할 수 없다면 다음 대체 흐름을 사용한다.

```text
Pi 내부 MQTT capture request
→ Pi CameraCaptureService가 구독
→ Hanwha HTTP Snapshot API 호출
→ JPEG 다운로드
```

## 3. 슬롯·카메라·ROI 매핑

센서 ID를 `config/parking_slots.json`의 슬롯 ID로 변환하고, `AppConfig::iva_areas`에서
카메라 채널과 ROI를 찾는다.

```text
HALL01 → EV01 → cam01/ch01 → EV01 normalized ROI
```

ROI는 해상도에 독립적인 `0.0~1.0` 정규화 좌표다.

```text
pixel_x      = roi_x      × image_width
pixel_y      = roi_y      × image_height
pixel_width  = roi_width  × image_width
pixel_height = roi_height × image_height
```

관련 설정:

```text
IVA_EV01_CHANNEL_ID
IVA_EV01_ROI_X
IVA_EV01_ROI_Y
IVA_EV01_ROI_WIDTH
IVA_EV01_ROI_HEIGHT
```

관련 코드:

- `include/snapshot/SnapshotStorage.hpp`
- `src/snapshot/SnapshotStorage.cpp`
- `src/app/AppConfig.cpp`
- `config/parking_slots.json`

## 4. 촬영 일정

| 기준 시각 | 동작 |
|---|---|
| T0 | OCCUPIED 세션 및 촬영 예약 생성 |
| T0+30초 | `HALL_OCCUPIED_30S` 촬영 요청 |
| T0+60초 | `HALL_OCCUPIED_60S` 촬영 요청 |
| T0+장기점유 기준시간 | 차량이 남아 있으면 `OVERTIME_VIOLATION` 촬영 요청 및 위반 처리 |
| 장기점유 기준시간 이전 VACANT | 대기 요청·OCR 취소, 세션 사진 및 IMAGE_LOG 삭제 |

기준시간은 `SYSTEM_SETTINGS.overstay_threshold_seconds`에서 조회하며 기본값은 3600초다.
`점유시간 < 기준시간`은 조기 출차, `점유시간 >= 기준시간`은 위반으로 처리한다.

## 5. MQTT 요청 규약 제안

> 아래 topic과 payload는 카메라에서 지원이 확인된 값이 아니라 프로젝트 제안이다.

Topic:

```text
parking/camera/{camera_id}/capture/request
```

QoS/retain:

```text
QoS 1
retain false
```

Payload:

```json
{
  "schema_version": 1,
  "request_id": "EV01-20260722T120030Z-1",
  "command": "CAPTURE_SNAPSHOT",
  "camera_id": "cam01",
  "channel_id": "ch01",
  "slot_id": "EV01",
  "session_id": 101,
  "reason": "HALL_OCCUPIED_30S",
  "requested_at": "2026-07-22T12:00:30Z"
}
```

`request_id`는 중복 요청과 늦게 도착한 응답을 구분하는 idempotency key다.

## 6. MQTT 응답 규약 제안

Topic:

```text
parking/camera/{camera_id}/capture/response
```

성공:

```json
{
  "schema_version": 1,
  "request_id": "EV01-20260722T120030Z-1",
  "status": "SUCCESS",
  "camera_id": "cam01",
  "channel_id": "ch01",
  "snapshot_url": "https://camera-host/snapshot/path",
  "captured_at": "2026-07-22T12:00:31Z"
}
```

실패:

```json
{
  "schema_version": 1,
  "request_id": "EV01-20260722T120030Z-1",
  "status": "FAILED",
  "error": "SNAPSHOT_TIMEOUT"
}
```

MQTT payload에 JPEG binary를 직접 넣지 않는다. MQTT는 명령·상태·URL을 전달하고
실제 이미지는 HTTPS로 받는 방식을 우선한다.

## 7. Pi 이미지 처리

```text
Camera full Snapshot
→ MIME/크기/해상도 검증
→ slot normalized ROI crop
→ 번호판 후보 검출 및 원근 보정
→ Resize
→ Bilateral Filter
→ CLAHE
→ Unsharp Mask
→ Gemini OCR
```

번호판 후보는 메모리에서 처리하며 `plate_candidates` 디렉터리를 다시 만들지 않는다.

권장 저장 구조:

```text
data/snapshots/ch1/EV01/
├── occupied_30s/
│   ├── original/..._HALL_30S_original.jpg
│   └── enhanced/..._HALL_30S_enhanced.jpg
└── occupied_60s/
    ├── original/..._HALL_60S_original.jpg
    └── enhanced/..._HALL_60S_enhanced.jpg
```

`IMAGE_LOG.enhancement_type` 제안:

```text
HALL_30S
HALL_60S
TIMER_VIOLATION
```

## 8. OCR 및 재시도 정책

- 30초 사진으로 첫 OCR을 수행한다.
- 30초 OCR이 성공하면 60초 사진은 증거로만 저장한다.
- 30초 OCR이 실패하면 60초 사진으로 OCR을 재시도한다.
- 두 번 실패하면 `ocr_status=FAILED`, `ev_status=UNKNOWN`으로 남긴다.
- OCR 실패를 `NON_EV`로 처리하지 않는다.

촬영 요청은 3초 내 응답이 없으면 2초 간격으로 최대 2회 재시도하는 안을 권장한다.
촬영 실패가 세션과 1시간 타이머를 중단시키면 안 된다.

## 9. 조기 출차 및 늦은 응답

- VACANT 발생 시 아직 발행하지 않은 30초·60초 예약을 취소한다.
- 진행 중인 OCR은 `OcrWorker::cancelSession()`으로 결과 반영을 차단한다.
- 1시간 이전 종료 세션의 원본·개선 이미지와 `IMAGE_LOG`를 삭제한다.
- 출차 후 도착한 `request_id` 응답은 세션 상태를 확인한 뒤 다운로드하지 않는다.
- 이미 임시 다운로드한 파일은 즉시 삭제한다.
- VIOLATION 세션의 입차·위반 증거는 보존한다.

## 10. 구현 전 카메라 확인사항

1. PNO-A9081R이 MQTT topic을 구독할 수 있는가?
2. MQTT subscription을 Event Rule trigger로 사용할 수 있는가?
3. Event Rule에서 Snapshot 생성이 가능한가?
4. 생성한 Snapshot을 URL, HTTP push, FTP 중 어떤 방식으로 제공하는가?
5. 요청과 결과를 `request_id`로 연결할 수 있는가?
6. 채널별 Snapshot API endpoint는 무엇인가?
7. Snapshot API 인증은 Digest, Basic 또는 Session 중 무엇인가?
8. 실제 URL, 계정, 비밀번호는 환경변수로 관리 가능한가?

이 항목이 확인되기 전에는 카메라가 지원하지 않는 MQTT 명령을 코드에 사실처럼
하드코딩하지 않는다.

## 11. 완료 조건

- OCCUPIED 중복 이벤트가 촬영 예약을 중복 생성하지 않는다.
- 올바른 센서의 슬롯·채널·ROI로 요청한다.
- 30초와 60초 요청이 각각 한 번만 성공 처리된다.
- 잘못된/중복/늦은 `request_id` 응답을 거부한다.
- 이미지 다운로드·crop·전처리·OCR·DB 저장이 하나의 session_id로 연결된다.
- 조기 출차 이미지와 DB 기록이 정리된다.
- 카메라 또는 MQTT 장애가 주차 상태와 타이머를 중단하지 않는다.
- 비밀정보가 topic, payload, 로그, Git에 포함되지 않는다.

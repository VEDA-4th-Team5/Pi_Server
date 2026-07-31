# CV Snapshot API Pi Server 연동

기준일: 2026-07-28

## 목적

CV5의 `cv_snapshot_api`가 한 번 캡처한 동일 프레임에서 생성한 `original`과
`enhanced` JPEG를 Raspberry Pi가 내려받아 기존 홀센서 30/60초 OCR 흐름에 연결한다.
Pi는 카메라 개선본이 정상 제공되면 OpenCV 화질 개선을 다시 수행하지 않는다.

## 런타임 흐름

```text
Hall SessionStarted
→ CaptureSchedulerRuntime (T0+30초 / T0+60초)
→ HallCaptureExecutor
→ CameraSnapshotApiClient
   ├─ POST OPEN_API_BASE/startserver          (초기화 시)
   ├─ GET  OPEN_API_BASE/channels             (초기화 시)
   ├─ GET  OPEN_API_BASE/filters              (초기화 시)
   ├─ POST OPEN_API_BASE/images/generate      (촬영 시)
   └─ GET  IMAGE_BASE + results[].image_path  (즉시)
→ SnapshotStorage에 original/enhanced 파일 저장
→ HallCaptureCoordinator / IMAGE_LOG
→ OcrWorker::enqueueHallCapture(original, enhanced)
→ Gemini HTTPS OCR
→ VEHICLE EV/PHEV 조회 및 동일 PARKING_SESSION.session_id 갱신
```

## 설정

```bash
export CAPTURE_SCHED_ENABLED=true
export HALL_CAPTURE_OCR_ENABLED=true
export CAMERA_SNAPSHOT_API_ENABLED=true
export CAMERA_OPEN_API_BASE='http://CAMERA_IP/opensdk/APP_ID'
export CAMERA_IMAGE_BASE='http://CAMERA_IP:8080'
export CAMERA_API_USERNAME='CAMERA_USER'
export CAMERA_API_PASSWORD='LOCAL_SECRET'
export CAMERA_IMAGE_SERVER_PORT=8080
export CAMERA_SNAPSHOT_API_RTSP_FALLBACK=false

# CV API는 0-based channel을 사용한다.
export IVA_EV01_SNAPSHOT_API_CHANNEL=0
export IVA_EV02_SNAPSHOT_API_CHANNEL=0
export IVA_EV03_SNAPSHOT_API_CHANNEL=0
export IVA_EV04_SNAPSHOT_API_CHANNEL=0
```

실기기 반복 시험에서는 다음처럼 시간을 줄일 수 있다.

```bash
export PARKING_OCCUPANCY_CONFIRM_MS=0
export CAPTURE_OFFSETS_SEC=5,10
```

## 저장 위치

```text
data/snapshots/ch1/EV01/scene/
├─ session_<id>_slot_EV01_HALL_30S_CAMERA_API_<time>_original.jpg
└─ enhanced/
   └─ session_<id>_slot_EV01_HALL_30S_CAMERA_API_<time>_enhanced.jpg
```

두 경로는 같은 `IMAGE_LOG` 행의 `original_image_path`와
`enhanced_image_path`에 기록된다. 파일 저장 후 DB 저장이 실패하면 실행기가 두 파일을
삭제한다.

## 오류와 재시도

- `PROCESSING_BUSY`, `SNAPSHOT_FAILED`, `IMAGE_SERVER_NOT_STARTED`만 제한 재시도한다.
- JPEG는 HTTP 200, `Content-Type: image/jpeg`, `FF D8 FF` magic, 응답 byte 길이를 검증한다.
- JPEG GET 404는 만료된 run으로 보고 같은 URL을 계속 사용하지 않는다.
- 카메라가 최근 run 8개만 보관하므로 generate 응답 직후 두 JPEG를 모두 받는다.
- API 실패 시 전체 서버는 종료하지 않고 CaptureScheduler의 기존 재시도 정책으로 넘긴다.
- `CAMERA_SNAPSHOT_API_RTSP_FALLBACK=true`일 때만 기존 RTSP 촬영으로 fallback한다.

## 현재 제한

- 제공된 CV API에는 ROI 입력이 없으므로 카메라에서 받은 결과는 채널 전체 프레임이다.
- API 모드에서는 Pi가 화질 개선이나 ROI crop을 하지 않고 전체 original/enhanced를 Gemini에 전달한다.
- 현재 실기기는 OpenAPI에 HTTP Digest 인증을 요구하며 계정은 `.env.camera.local`에서만 읽는다.
- 기본값은 안전을 위해 `CAMERA_SNAPSHOT_API_ENABLED=false`다. CAP 검증 후 운영 환경에서 활성화한다.

## 2026-07-28 실기기 API 검증

- `cv_snapshot_api`의 `/channels`와 `/filters`를 Digest 인증으로 조회했다.
- 채널은 0~3까지 총 4개이며 현재 시험은 채널 0으로 수행했다.
- `/startserver`는 HTTP 202, `/images/generate`는 HTTP 200을 반환했다.
- 같은 `run_id`의 original/enhanced를 즉시 내려받아 HTTP 200, JPEG Content-Type,
  `FF D8 FF` magic, 응답 byte 길이 일치를 확인했다.
- 시험 프레임은 2592x1520이며 환경은 `backlight`, 자동 필터는
  `backlight_combined`로 판정됐다.
- 아직 남은 실기기 검증은 실제 번호판을 비춘 상태의 채널/슬롯 매핑, Gemini OCR 정확도,
  홀센서 30/60초 전체 런타임과 DB 저장이다.

## 테스트

하드웨어 없이 mock HTTP 서버로 discovery, `PROCESSING_BUSY` 재시도, 동일 run의 JPEG
다운로드, JPEG 검증과 잘못된 채널 거부를 확인한다.

```bash
cmake -S . -B build
cmake --build build -j2 --target camera-snapshot-api-client-test pi-server
ctest --test-dir build -R camera-snapshot-api-client-test --output-on-failure
```

실기기에서는 CAP 설치 후 먼저 다음 순서로 확인한다.

```text
POST /startserver
→ GET /channels
→ GET /filters
→ POST /images/generate
→ 응답의 original/enhanced image_path 즉시 GET
→ Pi 저장 파일과 IMAGE_LOG 확인
→ Gemini OCR 및 PARKING_SESSION 번호판 확인
```

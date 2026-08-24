# CH2 입구 BestShot 처리 구조

## 범위

주차장 입구에서 발생한 Hanwha Vision Plate BestShot을 수집하고 동일 객체의
반복 이벤트가 EV 아이콘 판정과 Gemini OCR을 중복 실행하지 않도록 한다. 입구 객체는 주차면의
`PARKING_SESSION`과 별도 수명을 가지며 `ENTRANCE_RECOGNITION`에 기록한다.

## 입력 구성

- 카메라에서는 입구 영역의 BestShot 규칙만 활성화한다.
- 별도 CH2 RTSP 주소를 만들지 않고 기존 RTSP Metadata 트랙을 재사용한다.
- 서버는 카메라의 실제 CH2 Metadata에서 받은 BestShot을 `ch02`로 기록한다.
- CH1 IVA 점유 이벤트는 기존 MQTT 경로를 그대로 사용한다.

```text
RTSP Metadata
  -> BestShotReceiver
  -> EntranceBestShotCoordinator
  -> EntranceVehicleService
       -> ImageRef JPEG 다운로드
       -> EntranceEvWorker / Python EV 아이콘 판정
       -> OcrWorker / Gemini 번호판 OCR
       -> VEHICLE UPSERT + ENTRANCE_RECOGNITION 연결
```

## 중복 및 상태 정책

1차 중복 키는 `camera_id|channel_id|object_id`이다. 카메라가 같은 차량에 새
`object_id`를 부여하는 경우에는 Plate JPEG의 OpenCV pHash를 짧은 시간 동안
비교해 EV 판정과 Gemini OCR 전에 2차로 차단한다. 상태는 다음 순서로 전이한다.

```text
EV_QUEUED -> EV_PROCESSING -> OCR_QUEUED -> OCR_PROCESSING -> COMPLETED
       \              \              \               \-> FAILED
        \--------------\--------------\-----------------> FAILED
```

- Vehicle BestShot은 소비만 하며 DB 행이나 Plate 대기 상태를 만들지 않는다.
- Plate BestShot 하나가 도착하면 즉시 Plate JPEG를 다운로드한다.
- 같은 Plate JPEG로 EV 아이콘 판정을 먼저 한 번 실행한다.
- 아이콘 판정이 0 또는 1로 끝난 경우에만 Gemini OCR을 한 번 실행한다.
- 첫 Plate가 큐 소유권을 얻은 뒤 같은 키는 추가 등록 없이 중복 수만 누적한다.
- 같은 카메라·채널에서 pHash 거리가 설정 임계값 이하인 이미지는 대표 입구
  이벤트의 `duplicate_count`에 누적하고 별도 DB 행과 OCR 작업을 만들지 않는다.
- 완료 및 실패 객체는 TTL 이후 제거해 ObjectId 재사용을 허용한다.
- TTL 동안 처리 객체 수는 `ENTRANCE_PENDING_CAPACITY`로 제한한다.
- pHash 기록도 별도 TTL과 최대 용량을 적용해 메모리가 계속 증가하지 않게 한다.

Vehicle과 Plate의 ObjectId 결합은 하지 않는다. Plate 객체만 처리하므로 Vehicle
ObjectId가 다르거나 Vehicle 이벤트가 오지 않아도 입구 판정에는 영향이 없다.
pHash는 동일·근접 이미지 중복만 제거한다. 같은 차량이라도 카메라가 크게 다른
Plate 이미지를 만들면 별도 객체로 처리될 수 있으며, 임계값을 과도하게 높이면
연속 진입한 다른 차량을 합칠 수 있으므로 실제 CH2 표본으로 조정한다.

## 저장 경로

```text
data/entrance/.work/ch02/<camera>_<object>_<received_epoch_ms>/
├── plate.jpg
└── ev_analysis/
    └── worker 임시 산출물
```

파일명에는 번호판을 넣지 않는다. EV 아이콘 판정과 OCR이 모두 성공하면 DB 결과를
확정한 뒤 위 작업 디렉터리를 삭제한다. `ENTRANCE_RECOGNITION`의 번호판, OCR 신뢰도,
`resolved_is_ev`, `vehicle_id`는 남고 파일 경로만 `NULL`, `artifact_state=DELETED`가
된다. 실패 산출물은 기본 24시간 보존한 뒤 같은 방식으로 정리한다. 주차면의
`data/snapshots` 증거 이미지와 `IMAGE_LOG`는 이 정리 대상이 아니다.

## 설정

```text
ENTRANCE_ENABLED=true
ENTRANCE_CAMERA_ID=cam01
ENTRANCE_SOURCE_CHANNEL_ID=ch02
ENTRANCE_CHANNEL_ID=ch02
ENTRANCE_OUTPUT_ROOT=data/entrance
ENTRANCE_OBJECT_TTL_SECONDS=60
ENTRANCE_PENDING_CAPACITY=64
ENTRANCE_IMAGE_DEDUP_WINDOW_SECONDS=20
ENTRANCE_IMAGE_DEDUP_PHASH_THRESHOLD=10
ENTRANCE_DELETE_ARTIFACTS_ON_SUCCESS=true
ENTRANCE_FAILURE_RETENTION_HOURS=24
ENTRANCE_PLATE_MATCH_WINDOW_MINUTES=30
ENTRANCE_PLATE_MATCH_MIN_CONFIDENCE=0.85
ENTRANCE_EV_ANALYSIS_ENABLED=true
ENTRANCE_EV_PYTHON=/usr/bin/python3
ENTRANCE_EV_WORKER_SCRIPT=tools/cv/low_quality_presence_v1/runtime/pi_worker.py
ENTRANCE_EV_MODEL_BUNDLE=tools/cv/low_quality_presence_v1/model_bundle
ENTRANCE_EV_TEMPLATE_CACHE=tools/cv/low_quality_presence_v1/model_bundle/runtime_template_cache.json
ENTRANCE_EV_THRESHOLDS=tools/cv/low_quality_presence_v1/model_bundle/default_thresholds.json
ENTRANCE_EV_TIMEOUT_MS=5000
ENTRANCE_EV_QUEUE_CAPACITY=16
ENTRANCE_EV_OPENCV_THREADS=1
```

Python runtime 의존성은 `python3-opencv`, `python3-numpy`이다. 프로세스는
이미지마다 새로 실행하지 않고 Pi Server가 한 개를 유지하며 JSON Lines로 통신한다.
시작할 때 모델·threshold·템플릿 계약을 검증하고, 검증된 아티팩트는 프로세스
수명 동안 메모리에서 재사용한다.
서버 시작 시 worker가 준비되지 않으면 입구 기능을 잘못된 상태로 실행하지 않고
서버 시작을 실패 처리한다. 실행 중 timeout, 잘못된 JSON 또는 자식 프로세스 종료는
해당 입구 객체를 `FAILED`로 기록하며 서버의 다른 기능은 계속 실행한다.

## EV 판정과 DB 원칙

`VEHICLE.is_ev`는 입구 Plate 이미지의 아이콘 판정값이며 1=EV, 0=NON_EV다.
기존 PHEV 값은 동일한 충전 차량 정책을 보존하기 위해 EV로 흡수한다. 아이콘
판정값은 감사 목적으로 `ENTRANCE_RECOGNITION.vision_is_ev`에도 저장한다.

```text
EV_CANDIDATE     -> vision_is_ev=1
NON_EV_CANDIDATE -> vision_is_ev=0
REVIEW           -> vision_is_ev=NULL
```

`EV_CANDIDATE` 또는 `NON_EV_CANDIDATE`가 나온 뒤 Gemini OCR이 번호판을 인식하면
같은 트랜잭션에서 `VEHICLE(plate_number,is_ev)`를 생성하거나 갱신하고,
`ENTRANCE_RECOGNITION.vehicle_id`를 연결한다. `REVIEW`, worker 오류 또는 OCR 실패는
차량 행을 만들지 않는다.

## 주차면 OCR 연결

주차면 OCR 문자열은 `PARKING_SESSION.parking_ocr_plate`와 `IMAGE_LOG.ocr_result`에
원문 그대로 남긴다. 서버는 주차 세션 시작 전 설정 시간 안에 완료된 입구 이벤트 중
신뢰도 기준을 충족하고 아직 다른 세션에 연결되지 않은 후보를 찾는다.

- 완전 일치는 `ENTRANCE_EXACT`다.
- 마지막 네 자리 숫자가 같고 전체에서 정확히 한 글자만 다른 경우만
  `ENTRANCE_FUZZY`로 허용한다.
- 동점 후보가 모호하거나 후보가 없으면 `UNRESOLVED`로 두어 자동 위반을 만들지 않는다.
- 연결되면 `PARKING_SESSION.plate_number`에는 입구 기준 번호판을 저장하고,
  `entrance_event_id`, `plate_match_score`, `plate_resolution_source`를 함께 남긴다.
- 한 입구 이벤트는 한 주차 세션에만 연결한다.

연결된 기준 번호판으로 `VEHICLE.is_ev`를 조회한다. EV는 주차장 입구 통과 시각이
아니라 실제 `PARKING_SESSION.entry_time`부터 장기점유 시간을 계산하며, NON_EV는
EV 전용면에서 즉시 위반 처리한다. 미매칭 차량은 `UNKNOWN`으로 남긴다.

현재 Python bundle의 원래 계약은 아이콘 존재 기반 사전 판별이다. 운영 정책상 이
결과를 `is_ev`로 직접 사용하도록 승인된 상태이므로, 실제 CH2 Plate 이미지로
오탐·미탐 정확도를 별도 검증해야 한다.

실제 Python 런타임까지 포함한 C++ worker smoke는 다음 명령으로 실행한다.

```bash
cmake --build cmake-build --target entrance-ev-worker-test -j4
./cmake-build/entrance-ev-worker-test --real-worker "$PWD"
```

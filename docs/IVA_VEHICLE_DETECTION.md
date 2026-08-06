# WiseAI IVA 차량 탐지 연동

기준 기능: EVDA-192

## 역할 분리

카메라의 WiseAI가 영상과 IVA Area를 분석하고, Raspberry Pi는 카메라가 발행한
ONVIF MQTT 이벤트를 주차면 이벤트로 변환한다. Qt의 실시간 RTSP 표시는 이 흐름과
독립적이다.

```text
Hanwha WiseAI IVA Area
→ Camera ONVIF MQTT
→ Mosquitto
→ CameraEventParser
→ IvaEventResolver
→ ParkingTriggerCoordinator
→ Snapshot / DB / OCR / Qt MQTT
```

## 2026-08-05 배치 기준

현재 목표 배치는 한 카메라 채널의 고정 화면 안에 전기차 주차면 네 개가 있는
구조다. 카메라 WiseAI에는 주차면과 같은 이름의 IVA Rule을 만든다.

| 서버 채널 | Video source token | IVA Rule | 전역 slot_id | Snapshot API channel |
|---|---|---|---|---:|
| `ch01` | `vs-0` | `EV01` | `EV01` | 0 |
| `ch01` | `vs-0` | `EV02` | `EV02` | 0 |
| `ch01` | `vs-0` | `EV03` | `EV03` | 0 |
| `ch01` | `vs-0` | `EV04` | `EV04` | 0 |

네 슬롯은 같은 전체 프레임을 공유하지만 ROI가 서로 다르다. 실제 좌표는 카메라
설치 후 캡처한 기준 프레임에서 측정하며 문서가 임의 값을 확정하지 않는다.

향후 여러 카메라 채널에서 `EV01~EV04` Rule 이름을 반복한다면 DB의 `slot_id`는
전역적으로 유일해야 하므로 `CH02_EV01` 같은 전역 ID를 사용하거나 별도 매핑을
정해야 한다. 현재 표는 `ch01` 한 채널 기준이다.

## 슬롯 매핑 계약

IVA 이벤트는 `config/parking_slots.json`의 다음 세 값을 모두 만족해야 한다.

```text
camera_id + video_source_token + rule_name
```

예시는 다음과 같다.

```json
{
  "camera_id": "cam01",
  "video_source_token": "vs-0",
  "rule_name": "EV01"
}
```

카메라 topic의 `VideoSourceToken-0`과 `vs-0`은 모두 내부에서 `vs-0`으로
정규화한다. 토큰 또는 Rule 이름이 없거나, 같은 조합이 여러 슬롯에 매핑되면
기본 CH1을 추측하지 않고 이벤트를 거부한다.

슬롯별 ROI와 서버 채널은 `AppConfig::iva_areas`에서 가져온다. 운영 전에
`.env.iva.local` 또는 환경변수로 `IVA_EV01_ROI_X` 등의 정규화 좌표를 설정해야
한다. 설정하지 않으면 기존 호환 동작으로 전체 프레임 `(0, 0, 1, 1)`을 사용한다.

좌표는 이미지 디렉터리마다 별도 파일로 복제하지 않고 슬롯 설정 한 곳에서 관리한다.
장기적으로는 `config/parking_slots.json`의 각 `camera_bindings`에 다음 정보를 함께
두는 것을 목표로 한다. 아래는 형식 예시이며 숫자는 실제 설치 좌표가 아니다.

```json
{
  "camera_id": "cam01",
  "video_source_token": "vs-0",
  "rule_name": "EV01",
  "snapshot_api_channel": 0,
  "roi": {
    "x": "실측 정규화 좌표",
    "y": "실측 정규화 좌표",
    "width": "실측 정규화 폭",
    "height": "실측 정규화 높이"
  }
}
```

ROI는 입력 해상도와 무관한 0.0~1.0 정규화 좌표를 사용한다. 신규 세션 이미지는
실제 SQLite `session_id` 아래에 저장한다. 기존 `scene/` 파일은 자동 이동하지 않는다.

```text
data/snapshots/ch1/
└─ EV01/
   └─ session_27/
      ├─ occupancy_start/
      ├─ hall_30s/
      ├─ hall_60s/
      └─ overstay/
```

각 촬영 단계의 original/enhanced 파일은 같은 단계 디렉터리에 둔다. 세션에
연결되지 않은 IVA 진단 이미지만 `EV01/events/iva/`에 격리한다.

## Hall 센서 없이 운영하는 상태 머신

점유 판정 주체는 한 실행에서 하나만 선택한다. 기본값은 기존 호환을 위한 `HALL`이며,
카메라 ENTER/EXIT로 세션을 관리할 때만 `CAMERA_IVA`를 선택한다.

```bash
export PARKING_OCCUPANCY_SOURCE=CAMERA_IVA
export CAMERA_IVA_EXIT_CONFIRM_MS=10000
```

```text
IVA ENTER active=true
→ 대기 중인 동일 슬롯 EXIT 취소
→ ACTIVE 세션이 없으면 PARKING_SESSION 생성

동일 active 반복
→ 기존 session 유지, 중복 촬영/세션 생성 금지

IVA EXIT active=false
→ 기본 10초 출차 확인 예약
→ 확인 중 ENTER가 오면 취소
→ 확인 만료 후 동일 슬롯 session 종료

BestShot/Plate 이벤트
→ 현재 ACTIVE session에 이미지와 OCR 결과만 attach
```

`CAMERA_IVA_EXIT_CONFIRM_MS`는 1000~60000ms 범위이며 기본값은 10000ms다.
반복 EXIT는 최초 deadline을 뒤로 미루지 않는다. 확인이 끝난 출차는 Hall VACANT와
동일한 정리 정책을 사용한다. `violation_at IS NULL`이면 예약/OCR/이미지/IMAGE_LOG를
정리하고, `violation_at IS NOT NULL`이면 위반 증거를 보존한다.

## 카메라 Publication 계약

```text
ENTER topic: cam01/onvif-ej/iva/vs-0/EV01/enter
EXIT topic:  cam01/onvif-ej/iva/vs-0/EV01/exit
QoS: 1
Retain: false
Default topic prefix: false
```

```json
{"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"EV01","slot_id":"EV01","event_type":"IVA_AREA","action":"ENTER","active":true}
```

```json
{"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"EV01","slot_id":"EV01","event_type":"IVA_AREA","action":"EXIT","active":false}
```

Pi는 JSON 필드 타입과 topic/payload/config의 카메라·토큰·슬롯·동작 일치 여부를
검사한다. 불일치 이벤트는 기본 CH1로 추측하지 않고 거부한다.

## Snapshot API 기반 현재 촬영 흐름

```text
IVA MQTT active 수신
→ (camera_id, token, rule_name)으로 EV01~EV04 결정
→ bounded capture queue에 작업 등록 후 MQTT callback 즉시 반환
→ CameraSnapshotApiClient /images/generate 호출
→ 해당 API channel의 original/enhanced JPEG 즉시 다운로드
→ data/snapshots/ch1/EVxx/session_<id>/<stage> 저장
→ IMAGE_LOG / OCR / Qt 이벤트 연결
```

MQTT는 촬영 신호와 식별자만 전달하고 JPEG는 Snapshot HTTP API로 내려받는다. Pi는
카메라 개선본에 CLAHE/Sharpen을 다시 적용하지 않는다. 실제 슬롯 좌표가 아직 확정되지
않아 현재는 전체 프레임을 저장하며 crop은 구현하지 않았다. 이 방식으로
Qt의 RTSP 스트리밍과 Pi의 이벤트 촬영을 분리하고 Pi의 연속 영상 디코딩을 제거한다.

Snapshot API가 반환하는 것은 호출 시점의 현재 프레임이다. IVA가 감지된 바로 그
프레임이나 특정 `object_id`의 번호판 BestShot이 반드시 필요한 경우에는 카메라가
제공하는 event snapshot/BestShot URL을 우선 사용하고, Snapshot API는 fallback으로
사용해야 한다.

## 현재 처리 정책

- `HALL` 모드에서는 IVA ENTER/INTRUSION을 촬영 후보로만 처리한다.
- `CAMERA_IVA` 모드에서는 ENTER가 세션을 만들고 EXIT가 확인 후 세션을 닫는다.
- 선택되지 않은 점유 입력은 무시하여 Hall과 IVA가 동시에 세션을 만들지 않는다.
- 같은 슬롯의 짧은 반복 이벤트는 `IVA_DUPLICATE_SUPPRESSION_MS` 동안 억제한다.
- 유효한 IVA 이벤트는 BestShot과 연결할 pending slot을 만든다.
- 일반 Motion/ObjectDetection 이벤트는 IVA Snapshot 중복을 막기 위해 저장하지 않는다.

## 현재 제한

Camera Snapshot API가 활성화되면 시작·30초·60초·장기점유 증거는 카메라의 전체
original/enhanced JPEG를 사용한다. fallback이 꺼져 있으면 Pi의 RTSP 수신과 최초
프레임 대기도 생략한다. 카메라 웹 설정의 Publication 값은 `parking_slots.json`의
camera/token/rule과 일치해야 한다.

## 빌드와 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build --target camera-iva-event-test \
  camera-iva-occupancy-integration-test pi-server -j4
ctest --test-dir cmake-build \
  -R 'camera-iva-event-test|camera-iva-occupancy-integration-test' \
  --output-on-failure
```

테스트는 다음을 검증한다.

- `VideoSourceToken-0`과 `vs-0` 정규화
- 같은 Rule 이름을 가진 서로 다른 채널의 슬롯 분리
- 손상된 토큰의 CH1 오매핑 방지
- Rule 이름이 없는 이벤트 거부
- 비활성 이벤트 판별
- 모호한 camera/token/rule 설정 거부
- EXIT 확인 전 세션 유지 및 ENTER 재수신 취소
- 조기 EXIT의 이미지·IMAGE_LOG 삭제
- `violation_at`이 있는 EXIT의 증거 보존
- `session_id` 기반 이미지 디렉터리

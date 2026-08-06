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

ROI는 입력 해상도와 무관한 0.0~1.0 정규화 좌표를 사용한다. 결과 이미지는 좌표
설정과 분리해 다음처럼 저장한다.

```text
data/snapshots/ch1/
├─ EV01/scene/iva_<event_id>_original.jpg
├─ EV02/scene/iva_<event_id>_original.jpg
├─ EV03/scene/iva_<event_id>_original.jpg
└─ EV04/scene/iva_<event_id>_original.jpg
```

## Hall 센서 없이 운영하는 목표 상태 머신

`IVA Intrusion`은 경계 진입 순간의 pulse일 수 있으므로 이것만으로는 차량이 계속
주차 중인지, 출차했는지를 확정할 수 없다. Hall 센서를 사용하지 않으려면 카메라에서
영역 점유 상태를 나타내는 `ObjectsInside` 또는 동등한 active/clear 이벤트를 함께
발행해야 한다.

```text
IVA ENTER/INTRUSION active
→ 차량 후보 생성 및 Snapshot API 촬영 예약

IVA ObjectsInside=true가 확인 시간 이상 유지
→ camera-only PARKING_SESSION ACTIVE 생성

동일 active 반복
→ 기존 session 유지, 중복 촬영/세션 생성 금지

IVA ObjectsInside=false가 출차 확인 시간 이상 유지
→ session 종료

BestShot/Plate 이벤트
→ 현재 ACTIVE session에 이미지와 OCR 결과만 attach
```

권장 확인 시간은 실제 카메라 이벤트 흔들림을 측정한 뒤 설정한다. 카메라가 명시적인
clear/empty 이벤트를 제공하지 않는다면 Hall 센서 없이 정확한 출차 판정은 보장할 수
없으며, 이 경우 IVA는 촬영 트리거로만 사용해야 한다.

## Snapshot API 기반 목표 촬영 흐름

```text
IVA MQTT active 수신
→ (camera_id, token, rule_name)으로 EV01~EV04 결정
→ bounded capture queue에 작업 등록 후 MQTT callback 즉시 반환
→ CameraSnapshotApiClient /images/generate 호출
→ 해당 API channel의 original/enhanced JPEG 즉시 다운로드
→ Pi에서 해당 슬롯의 고정 ROI만 crop
→ data/snapshots/ch1/EVxx/scene 저장
→ IMAGE_LOG / OCR / Qt 이벤트 연결
```

MQTT는 촬영 신호와 식별자만 전달하고 JPEG는 Snapshot HTTP API로 내려받는다. Pi는
카메라 개선본에 CLAHE/Sharpen을 다시 적용하지 않고 crop만 수행한다. 이 방식으로
Qt의 RTSP 스트리밍과 Pi의 이벤트 촬영을 분리하고 Pi의 연속 영상 디코딩을 제거한다.

Snapshot API가 반환하는 것은 호출 시점의 현재 프레임이다. IVA가 감지된 바로 그
프레임이나 특정 `object_id`의 번호판 BestShot이 반드시 필요한 경우에는 카메라가
제공하는 event snapshot/BestShot URL을 우선 사용하고, Snapshot API는 fallback으로
사용해야 한다.

## 현재 처리 정책

- IVA ENTER/INTRUSION/OCCUPIED의 활성 이벤트만 차량 탐지 후보로 처리한다.
- `active=false` 이벤트는 입차 Snapshot을 만들지 않는다.
- 같은 슬롯의 짧은 반복 이벤트는 `IVA_DUPLICATE_SUPPRESSION_MS` 동안 억제한다.
- 유효한 IVA 이벤트는 BestShot과 연결할 pending slot을 만든다.
- 일반 Motion/ObjectDetection 이벤트는 IVA Snapshot 중복을 막기 위해 저장하지 않는다.

## 현재 제한

현재 IVA Snapshot은 Pi가 유지하는 RTSP 최신 프레임을 사용한다. 카메라 Snapshot
API 기반 요청형 촬영과 Pi의 연속 RTSP 제거는 후속 작업이다. 또한 카메라 웹 설정의
실제 Rule 이름이 `parking_slots.json`의 `rule_name`과 정확히 일치해야 한다.

카메라 단독 점유 세션 상태 머신과 ROI를 포함한 `parking_slots.json` schema도 아직
목표 설계이며 현재 런타임은 Hall 세션 및 `AppConfig::iva_areas`를 사용한다.

## 빌드와 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build --target camera-iva-event-test pi-server -j4
ctest --test-dir cmake-build -R camera-iva-event-test --output-on-failure
```

테스트는 다음을 검증한다.

- `VideoSourceToken-0`과 `vs-0` 정규화
- 같은 Rule 이름을 가진 서로 다른 채널의 슬롯 분리
- 손상된 토큰의 CH1 오매핑 방지
- Rule 이름이 없는 이벤트 거부
- 비활성 이벤트 판별
- 모호한 camera/token/rule 설정 거부

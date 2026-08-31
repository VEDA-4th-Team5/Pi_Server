# WiseAI IVA 차량 탐지 연동

기준 기능: EVDA-192, EVDA-238(CH3 확장 및 ONVIF PullPoint 입력)

## 역할 분리

카메라의 WiseAI가 영상과 IVA Area를 분석하고, Raspberry Pi는 카메라가 발행한
이벤트를 주차면 이벤트로 변환한다. Qt의 실시간 RTSP 표시는 이 흐름과
독립적이다. 이벤트 전송 경로는 `CAMERA_IVA_EVENT_SOURCE`로 선택하며
아래 두 경로 모두 같은 `IvaEventResolver`로 수렴한다.

```text
Hanwha WiseAI IVA Area
→ Camera ONVIF MQTT (CAMERA_IVA_EVENT_SOURCE=MQTT, 코드 기본값)
→ Mosquitto
→ CameraEventParser
→ IvaEventResolver
→ ParkingTriggerCoordinator
→ Snapshot / DB / OCR / Qt MQTT
```

```text
Hanwha WiseAI IVA Area
→ Camera ONVIF PullPoint long-poll (CAMERA_IVA_EVENT_SOURCE=ONVIF, 운영값)
→ OnvifIvaEventSource
→ OnvifIvaEventAdapter
→ IvaEventResolver
→ HallParkingService::handleCameraIvaSignal
→ Snapshot / DB / OCR / Qt MQTT
```

두 경로는 동시에 활성화되지 않는다. `ONVIF`를 선택하면 `MqttEventBridge`는
자신의 IVA 처리 경로를 건너뛴다(`src/mqtt/MqttEventBridge.cpp:383`). `OnvifIvaEventSource`는
카메라 ONVIF 이벤트 서비스를 PullPoint로 구독하고, 매핑 실패 시 아래 형식으로
경고를 남긴다(`src/main.cpp`의 콜백 로그).

```text
[WARN] ONVIF IVA event rejected: <IvaEventResolver 원인> token=<video_source_token> rule=<rule_name> action=<Intrusion|Exit>
```

원인 문자열이 `camera/token/rule mapping not found`이면 카메라가 실제로 발행하는
`video_source_token`/`rule_name` 조합이 `config/parking_slots.json`의
`camera_bindings`와 다르다는 뜻이다. 카메라 IVA Area 설정 화면에서 실제 값을
확인하고 슬롯 설정을 맞춘다.

## CH1·CH3 슬롯 매핑 (EVDA-192 / EVDA-238)

두 카메라 채널이 각각 고정 화면 안에서 WiseAI 기본 영역 `name1`~`name8`를
EV 주차면과 1:1로 매핑한다. CH1과 CH3는 `video_source_token`이 다르므로
Rule 이름이 채널마다 `name1`부터 반복돼도 슬롯 매핑은 모호해지지 않는다 —
`camera_id + video_source_token + rule_name` 세 값이 모두 일치해야 하기 때문이다.

| 서버 채널 | Video source token | IVA Rule | 전역 slot_id | Snapshot API channel |
|---|---|---|---|---:|
| `ch01` | `vs-0` | `name1` | `EV01` | 0 |
| `ch01` | `vs-0` | `name2` | `EV02` | 0 |
| `ch01` | `vs-0` | `name3` | `EV03` | 0 |
| `ch01` | `vs-0` | `name4` | `EV04` | 0 |
| `ch03` | `vs-2` | `name5` | `EV05` | 2 |
| `ch03` | `vs-2` | `name6` | `EV06` | 2 |
| `ch03` | `vs-2` | `name7` | `EV07` | 2 |
| `ch03` | `vs-2` | `name8` | `EV08` | 2 |

여덟 슬롯은 각각 다른 ROI를 사용한다. 실제 좌표는 카메라
설치 후 캡처한 기준 프레임에서 측정하며 문서가 임의 값을 확정하지 않는다.
실제 배치의 Rule 이름·채널·slot_id 대응은 배치마다 달라질 수 있으므로 이 표는
develop 기준 기본 배선이고, 실제 값은 `config/parking_slots.json`이 최종
근거다.

향후 세 번째 카메라 채널을 추가해 `name1~name4` Rule 이름을 다시 반복한다면
DB의 `slot_id`는 전역적으로 유일해야 하므로 `CH0x_EV01` 같은 전역 ID를 사용하거나
별도 매핑을 정해야 한다.

## 슬롯 매핑 계약

IVA 이벤트는 `config/parking_slots.json`의 다음 세 값을 모두 만족해야 한다.

```text
camera_id + video_source_token + rule_name
```

예시는 다음과 같다. EV01은 통합 IVA 영역 `name1` 하나에 바인딩한다.

```json
{
  "camera_id": "cam01",
  "video_source_token": "vs-0",
  "rule_name": "name1"
}
```

카메라 topic의 `VideoSourceToken-0`과 `vs-0`은 모두 내부에서 `vs-0`으로
정규화한다. 토큰 또는 Rule 이름이 없거나, 같은 조합이 여러 슬롯에 매핑되면
기본 CH1을 추측하지 않고 이벤트를 거부한다.

슬롯별 ROI와 서버 채널은 `AppConfig::iva_areas`에서 가져온다. 운영 전에
`.env.public` 또는 환경변수로 `IVA_EV01_ROI_X` 등의 정규화 좌표를 설정해야
한다. 또는 Qt가 REST API로 좌표를 SQLite에 저장할 수 있다. 두 위치 모두에
좌표가 없으면 전체 프레임을 임의로 사용하지 않고 해당 슬롯 촬영/OCR을 건너뛴다.

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

ROI는 입력 해상도와 무관한 0.0~1.0 정규화 좌표를 사용한다. 이미지는
슬롯별 고정 촬영 단계 디렉터리에 저장하고, 파일명과 `IMAGE_LOG.session_id`로
세션을 구분한다.

```text
data/snapshots/
├─ ch1/EV01~EV04/
│  ├─ occupancy_start/{original,enhanced}/
│  ├─ occupied_30s/{original,enhanced}/
│  ├─ occupied_60s/{original,enhanced}/
│  └─ overstay/{original,enhanced}/
└─ ch3/EV05~EV08/  (구조 동일)
```

각 촬영 단계의 original/enhanced 파일은 각각 `original/`과 `enhanced/`
하위 디렉터리에 둔다. 세션에
연결되지 않은 IVA 진단 이미지만 `EVxx/events/iva/`에 격리한다.

## Hall 센서 없이 운영하는 상태 머신

점유 판정 주체는 한 실행에서 하나만 선택한다. 기본값은 기존 호환을 위한 `HALL`이며,
카메라 네이티브 IvaArea 상태로 세션을 관리할 때만 `CAMERA_IVA`를 선택한다.

```bash
export PARKING_OCCUPANCY_SOURCE=CAMERA_IVA
export CAMERA_IVA_EXIT_CONFIRM_MS=10000
```

```text
name1 Action=Intrusion
→ 대기 중인 동일 슬롯 EXIT 취소
→ ACTIVE 세션이 없으면 PARKING_SESSION 생성

동일 Intrusion 반복
→ 기존 session 유지, 중복 촬영/세션 생성 금지

name1 Action=Exit
→ 기본 10초 출차 확인 예약
→ 확인 중 INTRUSION이 오면 취소
→ 확인 만료 후 동일 슬롯 session 종료

smart-parking-iva-v1 고정 Publication Action=EXIT
→ 실제 WiseAI 분석 결과가 아니므로 진단 로그만 남기고 무시
→ 세션 종료·사진 삭제 금지

name1 Action=Enter
→ 경계 통과 진단 이벤트로만 기록하고 점유·촬영에는 사용하지 않음

BestShot/Plate 이벤트
→ 현재 ACTIVE session에 이미지와 OCR 결과만 attach
```

`CAMERA_IVA_EXIT_CONFIRM_MS`는 1000~60000ms 범위이며 기본값은 10000ms다.
반복 EXIT는 최초 deadline을 뒤로 미루지 않는다. 확인이 끝난 출차는 Hall VACANT와
동일한 정리 정책을 사용한다. `violation_at IS NULL`이면 예약/OCR/이미지/IMAGE_LOG를
정리하고, `violation_at IS NOT NULL`이면 위반 증거를 보존한다.

## IVA와 Hall을 함께 사용하는 운영 상태 머신

현재 운영 프로필은 두 센서를 보완 입력으로 사용하는 다음 설정이다.

```bash
export PARKING_OCCUPANCY_SOURCE=HYBRID_OR
export PARKING_OCCUPANCY_CONFIRM_MS=5000
export CAMERA_IVA_EXIT_CONFIRM_MS=10000
```

입차는 IVA INTRUSION 또는 5초 유지된 Hall OCCUPIED 중 먼저 확정된 입력으로
세션 하나만 만든다. 나중에 도착한 센서는 새 세션을 만들지 않고 기존 세션의
`iva_confirmed` 또는 `hall_confirmed` 상태만 보강한다.

출차는 해당 세션을 실제로 확인한 센서만 판단에 참여한다. IVA만 확인한 세션의
Hall VACANT는 무시하고, Hall만 확인한 세션은 IVA EXIT을 기다리지 않는다. 두
센서가 모두 확인한 세션은 `iva_occupied=0`과 `hall_occupied=0`이 모두 확인된
뒤 종료한다. 네 상태는 `PARKING_SESSION`에 저장되므로 재시작 후에도 동일한
출차 정책을 유지한다.

## 카메라 MQTT 계약

`CAMERA_IVA_EVENT_SOURCE=MQTT`일 때의 계약이다. 운영 기본값인 `ONVIF`에서는
아래 topic 구조 대신 ONVIF PullPoint 응답의 같은 필드(`VideoSourceToken`,
`RuleName`, `Action`, `UtcTime`, `ObjectId`)를 직접 읽으며, 이후 슬롯 매핑·판정
로직은 동일하다. 운영 점유 판정은 카메라가 자동 발행하는 네이티브 IvaArea 상태를 사용한다.

```text
.../onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0/name1
.../onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0/name4
```

CH1(`vs-0`) 예시이며 CH3는 같은 구조에서 토큰만 `vs-2`, Rule 이름은
`name5`~`name8`이다(§CH1·CH3 슬롯 매핑 참고).

`name1`~`name4`는 각각 `EV01`~`EV04`다(CH3의 `name5`~`name8`은 `EV05`~`EV08`).
`Data.Action=Intrusion`만 대응 슬롯의
OCCUPIED로 전달하고 `Data.Action=Exit`는 VACANT 후보로 전달한다. WiseAI의
`Data.State=true`는 액션 발생 상태이므로 EXIT payload에서도 점유 true로 해석하지
않는다. `ObjectId`는 EXIT와 후속 INTRUSION의 상관관계 로그에 보존한다.

아래 고정 Publication은 카메라 설정 진단용으로만 보존하며 운영 점유 입력으로
사용하지 않는다. 같은 Intrusion Event Rule에 연결하면 ENTER와 EXIT가 동시에
발행될 수 있기 때문이다.

```text
INTRUSION topic: cam01/onvif-ej/iva/vs-0/EV01/intrusion
EXIT topic:      cam01/onvif-ej/iva/vs-0/EV01/exit
QoS: 1
Retain: false
Default topic prefix: false
```

```json
{"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"name1","slot_id":"EV01","event_type":"IVA_AREA","action":"INTRUSION","active":true}
```

```json
{"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"name1","slot_id":"EV01","event_type":"IVA_AREA","action":"EXIT","active":false}
```

커스텀 JSON 파서는 이전 프로토콜 호환을 위해 유지한다. 네이티브 운영 경로는
`camera_id + video_source_token + rule_name`과 슬롯별 OR 집계 결과를 사용한다.
커스텀 INTRUSION은 보조 입차 입력으로 허용하지만 고정 커스텀 EXIT에는 출차
권한을 부여하지 않는다. 출차는 Raw WiseAI `Data.Action=Exit`만 인정한다.

## Snapshot API 기반 현재 촬영 흐름

```text
IVA active 수신 (MQTT 또는 ONVIF PullPoint)
→ (camera_id, token, rule_name)으로 EV01~EV08 결정 (CH1: EV01~04, CH3: EV05~08)
→ bounded capture queue에 작업 등록 후 콜백 즉시 반환
→ CameraSnapshotApiClient /images/generate 호출
→ 해당 API channel의 original/enhanced JPEG 즉시 다운로드
→ data/snapshots/ch{1,3}/EVxx/<stage> 저장
→ 카메라 enhanced ROI를 Gemini OCR 우선 입력으로 사용
→ IMAGE_LOG / OCR / Qt 이벤트 연결
```

MQTT는 촬영 신호와 식별자만 전달하고 JPEG는 Snapshot HTTP API로 내려받는다. Pi는
카메라 개선본에 CLAHE/Sharpen을 다시 적용하지 않고 슬롯 ROI crop만 수행한다.
홀 촬영 OCR은 카메라 `enhanced` 파일을 우선 사용하고 파일이 누락·손상된
경우에만 `original`로 fallback한다. 이 방식으로
Qt의 RTSP 스트리밍과 Pi의 이벤트 촬영을 분리하고 Pi의 연속 영상 디코딩을 제거한다.

Snapshot API가 반환하는 것은 호출 시점의 현재 프레임이다. IVA가 감지된 바로 그
프레임이나 특정 `object_id`의 번호판 BestShot이 반드시 필요한 경우에는 카메라가
제공하는 event snapshot/BestShot URL을 우선 사용하고, Snapshot API는 fallback으로
사용해야 한다.

## 현재 처리 정책

- `HALL` 모드에서도 IVA INTRUSION만 촬영 후보로 처리하고 ENTER는 무시한다.
- `CAMERA_IVA` 모드에서는 INTRUSION 영역 OR 결과가 true면 세션을 만들고 모든
  영역에서 EXIT가 확인되면 출차 확인 후 세션을 닫는다.
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
- `name1 → EV01`, `name2 → EV02`, `name3 → EV03`, `name4 → EV04`
  1:1 점유 매핑
- ENTER 무시 및 INTRUSION 전용 세션 시작
- WiseAI JSON `ObjectId`, `UtcTime`, `Action` 구조 파싱
- EXIT 확인 전 세션 유지 및 INTRUSION 재수신 취소
- 조기 EXIT의 이미지·IMAGE_LOG 삭제
- `violation_at`이 있는 EXIT의 증거 보존
- `session_id` 기반 이미지 디렉터리

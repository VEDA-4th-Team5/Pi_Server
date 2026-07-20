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
- 센서 메시지 파서와 주차 상태 단위 테스트

아직 실제 STM32 UART 연결, 32면 최종 모델, 화재·LoRa 및 영상 클립 저장은
구현되지 않았습니다.

## 주요 흐름

```text
Camera
→ RTSP / MQTT / BestShot
→ Raspberry Pi C++ Server
→ OpenCV 전처리
→ Gemini OCR
→ SQLite 및 이미지 파일 저장
→ Qt HTTP/HTTPS 조회
```

## 빌드 및 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j2
ctest --test-dir cmake-build --output-on-failure
```

필요 패키지에는 OpenCV, FFmpeg, libcurl, Mosquitto, SQLite3 및
`libcpp-httplib-dev`가 포함됩니다.

## 실행

비밀정보는 다음 로컬 파일에서 관리하며 Git에 커밋하지 않습니다.

```text
.env.camera.local
.env.iva.local
.env.gemini.local
```

```bash
set -a
source ./.env.camera.local
source ./.env.iva.local
source ./.env.gemini.local
set +a

./cmake-build/pi-server
```

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
- 슬롯 하나에 여러 `ACTIVE` 세션이 생기는 문제를 보완해야 합니다.
- 센서 코드는 실제 UART 및 `main.cpp`와 연결되지 않았습니다.
- API 사용자 인증은 아직 없습니다.

자세한 내용은 [`docs/`](docs/)를 참고하십시오.

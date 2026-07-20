5. 디렉터리 구조
.
├── AGENTS.md
├── backups
│   └── modular_backup_20260709_180427.tar.gz
├── CMakeLists.txt
├── data
│   ├── db
│   │   ├── parking.db
│   │   └── backups/
│   └── snapshots
│       └── *.jpg
├── include
│   ├── app
│   │   └── AppConfig.hpp
│   ├── camera
│   │   ├── CameraChannel.hpp
│   │   └── RtspStreamReceiver.hpp
│   ├── database
│   │   └── EventDatabase.hpp
│   ├── event
│   │   ├── CameraEvent.hpp
│   │   ├── CameraEventParser.hpp
│   │   └── EventPayloadBuilder.hpp
│   ├── mqtt
│   │   └── MqttEventBridge.hpp
│   ├── snapshot
│   │   └── SnapshotStorage.hpp
│   └── util
│       ├── Logger.hpp
│       ├── StringUtil.hpp
│       ├── TimeUtil.hpp
│       └── UrlMasker.hpp
├── cmake-build
├── README.md
└── src
    ├── app
    │   └── AppConfig.cpp
    ├── camera
    │   └── RtspStreamReceiver.cpp
    ├── database
    │   └── EventDatabase.cpp
    ├── event
    │   ├── CameraEventParser.cpp
    │   └── EventPayloadBuilder.cpp
    ├── main.cpp
    ├── mqtt
    │   └── MqttEventBridge.cpp
    ├── snapshot
    │   └── SnapshotStorage.cpp
    └── util
        ├── Logger.cpp
        ├── StringUtil.cpp
        ├── TimeUtil.cpp
        └── UrlMasker.cpp
6. 주요 모듈 설명
6.1 main.cpp

Pi 서버의 진입점이다.
직접 RTSP 프레임을 읽거나 MQTT payload를 파싱하기보다는, 각 모듈을 초기화하고 실행 순서를 관리한다.

주요 역할은 다음과 같다.

OpenCV FFmpeg RTSP 옵션 설정
SIGINT, SIGTERM 종료 핸들러 등록
환경변수 기반 설정 로드
카메라 채널 객체 생성
SQLite DB 연결
RTSP 수신기 실행
초기 프레임 수신 확인
MQTT 이벤트 브릿지 실행
서버 대기 루프 유지
종료 시 MQTT, RTSP, DB 정리
6.2 AppConfig

위치:

include/app/AppConfig.hpp
src/app/AppConfig.cpp

환경변수에서 서버 설정을 읽어오는 모듈이다.

예상 관리 항목은 다음과 같다.

카메라 ID
RTSP URL 목록
MQTT Broker 주소
MQTT Port
MQTT Subscribe Topic
Snapshot 저장 경로
SQLite DB 경로
Preview frame 크기
RTSP 재시도 설정
초기 프레임 대기 시간
빈 프레임 처리 설정
6.3 CameraChannel

위치:

include/camera/CameraChannel.hpp

각 RTSP 채널의 상태를 관리하는 구조체 또는 클래스이다.

하나의 채널은 대략 다음 정보를 가진다.

camera_id
channel_id
rtsp_url
최신 원본 프레임
preview frame
프레임 수신 상태
mutex 등 동시성 제어 요소
6.4 RtspStreamReceiver

위치:

include/camera/RtspStreamReceiver.hpp
src/camera/RtspStreamReceiver.cpp

RTSP 스트림을 실제로 수신하는 모듈이다.

주요 역할은 다음과 같다.

각 채널의 RTSP URL 열기
OpenCV cv::VideoCapture 기반 프레임 수신
원본 프레임 저장
preview frame 생성
빈 프레임 발생 시 대기
연속 read 실패 감지
RTSP 재연결 처리
서버 종료 플래그 감지
초기 프레임 수신 여부 확인

현재 main.cpp에서는 다음과 같이 사용된다.

camera::RtspStreamReceiver rtsp_receiver(
    channels,
    config.preview_width,
    config.preview_height,
    config.rtsp_retry_delay_ms,
    config.empty_frame_delay_ms,
    config.max_consecutive_read_failures,
    g_running
);
6.5 SnapshotStorage

위치:

include/snapshot/SnapshotStorage.hpp
src/snapshot/SnapshotStorage.cpp

이벤트 발생 시 스냅샷 이미지를 저장하는 모듈이다.

현재 서버 로그 기준으로 스냅샷 저장 정책은 다음과 같다.

snapshot mode: ALL CHANNELS FULL-SIZE original frame

즉, preview frame이 아니라 각 채널의 원본 프레임을 저장한다.

저장 파일 예시는 다음과 같다.

data/snapshots/cam01_ch01_FULL_1280x720_20260709_180619_361.jpg
data/snapshots/cam01_ch02_FULL_1280x720_20260709_180619_414.jpg
data/snapshots/cam01_ch03_FULL_1280x720_20260709_180619_491.jpg
data/snapshots/cam01_ch04_FULL_2592x1520_20260709_180619_585.jpg

파일명에는 다음 정보가 포함된다.

camera_id + channel_id + frame_type + resolution + timestamp
6.6 MqttEventBridge

위치:

include/mqtt/MqttEventBridge.hpp
src/mqtt/MqttEventBridge.cpp

MQTT 이벤트와 내부 서버 기능을 연결하는 브릿지 모듈이다.

주요 역할은 다음과 같다.

MQTT Broker 연결
지정된 Topic Subscribe
MQTT payload 수신
이벤트 payload 파싱
이벤트에 해당하는 채널 탐색
SnapshotStorage 호출
EventDatabase 호출
이벤트 결과 로그 출력

전체 구조상 MqttEventBridge는 다음 기능을 연결한다.

MQTT Event
   ↓
CameraEventParser
   ↓
SnapshotStorage
   ↓
EventDatabase
6.7 CameraEventParser

위치:

include/event/CameraEventParser.hpp
src/event/CameraEventParser.cpp

MQTT로 수신된 payload를 내부 이벤트 구조로 변환하는 모듈이다.

담당 역할은 다음과 같다.

MQTT payload 문자열 파싱
camera_id 추출
channel_id 추출
event_type 추출
timestamp 추출
원본 payload 보존
잘못된 payload 처리
6.8 CameraEvent

위치:

include/event/CameraEvent.hpp

카메라 이벤트 정보를 담는 데이터 구조이다.

예상 필드는 다음과 같다.

camera_id
channel_id
event_type
event_time
topic
raw_payload
snapshot_path
6.9 EventPayloadBuilder

위치:

include/event/EventPayloadBuilder.hpp
src/event/EventPayloadBuilder.cpp

이벤트 정보를 다른 시스템으로 전달하기 위한 payload 생성 모듈이다.

향후 Qt 클라이언트, 외부 서버, STM32 연동, REST API 연동 등에 사용할 수 있다.

6.10 EventDatabase

위치:

include/database/EventDatabase.hpp
src/database/EventDatabase.cpp

SQLite 데이터베이스를 관리하는 모듈이다.

주요 역할은 다음과 같다.

DB 파일 열기
이벤트 테이블 생성
이벤트 로그 insert
snapshot 경로 저장
DB 연결 종료

DB 파일 기본 위치는 다음과 같다.

data/db/parking.db
6.11 util

위치:

include/util
src/util

공통 유틸리티 모듈이다.

파일	역할
Logger	로그 출력
StringUtil	문자열 처리
TimeUtil	timestamp 생성
UrlMasker	RTSP URL 내 계정/비밀번호 마스킹

UrlMasker는 제출 및 로그 출력 시 RTSP URL에 포함된 비밀번호가 그대로 노출되지 않도록 하기 위한 유틸리티이다.

7. 빌드 환경
7.1 권장 실행 환경
Raspberry Pi OS
Linux
C++17 이상
OpenCV
FFmpeg
Mosquitto MQTT
SQLite3
7.2 필요 패키지 설치

Ubuntu 또는 Raspberry Pi OS 기준:

sudo apt update

sudo apt install -y \
  build-essential \
  pkg-config \
  cmake \
  ffmpeg \
  libopencv-dev \
  mosquitto \
  mosquitto-clients \
  libmosquitto-dev \
  sqlite3 \
  libsqlite3-dev

패키지 역할은 다음과 같다.

패키지	역할
build-essential	g++, make 등 C/C++ 빌드 도구
pkg-config	OpenCV, Mosquitto 등 라이브러리 컴파일 옵션 탐색
cmake	C/C++ 빌드 도구
ffmpeg	RTSP 스트림 처리 기반 도구
libopencv-dev	OpenCV 개발 라이브러리
mosquitto	MQTT Broker
mosquitto-clients	mosquitto_pub, mosquitto_sub 테스트 도구
libmosquitto-dev	C/C++ MQTT 클라이언트 개발 라이브러리
sqlite3	SQLite CLI
libsqlite3-dev	C/C++ SQLite 개발 라이브러리
8. 빌드 방법

프로젝트 루트에서 다음 명령을 실행한다.

cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j2

빌드가 성공하면 실행 파일이 생성된다.

./cmake-build/pi-server

DB smoke test 실행 파일도 `cmake-build/db-manager-test`에 생성된다. 이 테스트는
DB smoke test는 지정한 테스트 DB를 변경하므로 운영 DB를 대상으로 실행하지 않는다.
9. 실행 전 환경변수 설정

이 서버는 코드 안에 카메라 주소, MQTT 주소, DB 경로를 직접 넣지 않고 환경변수에서 설정을 읽는다.

9.1 기본 환경변수 예시
export CAMERA_ID='cam01'

export CAMERA_RTSP_CH1='rtsp://USER:PASSWORD@CAMERA_IP:554/0/profile2/media.smp'
export CAMERA_RTSP_CH2='rtsp://USER:PASSWORD@CAMERA_IP:554/1/profile2/media.smp'
export CAMERA_RTSP_CH3='rtsp://USER:PASSWORD@CAMERA_IP:554/2/profile2/media.smp'
export CAMERA_RTSP_CH4='rtsp://USER:PASSWORD@CAMERA_IP:554/3/profile2/media.smp'

export MQTT_HOST='localhost'
export MQTT_PORT='1883'
export CAMERA_EVENT_SUB_TOPIC='+/onvif-ej/#'

9.4 IVA Area 주차구역 매핑

카메라 웹 설정의 IVA Virtual Area 이름을 주차면 ID와 같게 지정하는 것을 권장한다.
예를 들어 EV01 영역에 차량 Enter/Intrusion 이벤트가 발생하면 Pi 서버는 ch01의
최신 원본 프레임에서 아래 정규화 ROI를 잘라 저장한다.

```sh
export IVA_EV01_AREA_NAME='EV01'
export IVA_EV01_CHANNEL_ID='ch01'
export IVA_EV01_ROI_X='0.10'
export IVA_EV01_ROI_Y='0.20'
export IVA_EV01_ROI_WIDTH='0.40'
export IVA_EV01_ROI_HEIGHT='0.55'
```

ROI 값은 원본 해상도에 대한 0.0~1.0 비율이다. 위 예에서 왼쪽 시작점은 전체 폭의
10%, 위쪽 시작점은 전체 높이의 20%다. ROI를 설정하지 않으면 기본값
`0,0,1,1`을 사용하여 전체 프레임을 저장한다. EV02~EV04도 같은 형식으로 설정한다.

```sh
export IVA_EV02_AREA_NAME='EV02'
export IVA_EV02_CHANNEL_ID='ch02'
export IVA_EV03_AREA_NAME='EV03'
export IVA_EV03_CHANNEL_ID='ch03'
export IVA_EV04_AREA_NAME='EV04'
export IVA_EV04_CHANNEL_ID='ch04'
```

한 대의 WiseAI 카메라 안에서 `name1`, `name2` 두 영역을 사용하는 현재 장비는
`.env.iva.local`에서 두 영역을 모두 `ch01`에 연결한다.

```sh
IVA_EV01_AREA_NAME='name1'
IVA_EV01_CHANNEL_ID='ch01'
IVA_EV02_AREA_NAME='name2'
IVA_EV02_CHANNEL_ID='ch01'
```

서버는 MQTT topic과 payload 양쪽에서 `IVA`, `VirtualArea`, `FieldDetector`,
`ObjectsInside`와 `Enter`/`Intrusion` 조합을 탐지한다. Area 이름이 payload에 있으면 그 이름을 우선
사용하고, 이름이 생략된 firmware에서는 `VideoSourceToken` 채널 매핑을 fallback으로
사용한다. `Value=false` 등의 영역 해제 알림은 사진을 저장하지 않는다. IVA ROI 이미지는 다음 경로에 저장된다.

```text
data/snapshots/ch1/EV01/scene/
```

카메라의 Vehicle BestShot metadata 수신은 별도로 계속 동작하며 기존 경로
에서 수신한다. BestShot 저장은 IVA 이벤트 유무와 독립적으로 동작한다.

```text
data/bestshots/vehicle/
data/bestshots/plate/
```

`BESTSHOT_CORRELATION_WINDOW_MS`는 IVA 이벤트 이후 Vehicle BestShot을 같은
주차면에 연결할 수 있는 시간 창이며 기본값은 8000ms다. 카메라가 동일 IVA MQTT를
연속 발행할 때는 `IVA_DUPLICATE_SUPPRESSION_MS`(기본 1500ms) 안의 이벤트를 한 번으로
처리한다.

```sh
export BESTSHOT_CORRELATION_WINDOW_MS='8000'
export IVA_DUPLICATE_SUPPRESSION_MS='1500'
```

IVA 이벤트가 있으면 주차면 correlation에 활용하지만, IVA가 없어도 채널 기본 매핑으로
BestShot을 저장한다. 향후 STM32 홀센서
수신기는 `ParkingTriggerCoordinator::recordHallState()`를 호출하여 같은 주차면 이벤트에
센서 점유 상태를 추가할 수 있다.

9.5 Gemini 번호판 OCR

카메라의 Plate BestShot이 `data/bestshots/plate/`에 저장되면 C++ OCR worker가
이미지를 Gemini API로 전송한다. 인식된 번호는 SQLite의 이미지 및 주차 세션에
연결되고, `VEHICLE` 테이블 조회 결과에 따라 `EV`, `NON_EV`, `UNKNOWN` 또는
`OCR_FAILED`로 분류된다. OCR 요청은 별도 worker thread에서 처리하므로 RTSP 및
MQTT 수신 루프를 막지 않는다.

OCR worker는 원본을 항상 보존한다. Plate BestShot은 최대 2배 확대 후 약한
bilateral denoise, Lab 밝기 채널 CLAHE, 약한 unsharp mask를 적용한다. IVA Snapshot은
주차면 ROI 안에서 번호판 형태·밝기·문자 edge 밀도를 만족하는 후보를 찾고 원근 보정한
뒤 같은 전처리를 적용한다. Gemini에는 원본과 컬러 화질 개선본을
함께 전송한다.

```text
data/bestshots/plate/enhanced/
data/snapshots/ch1/EV01/scene/
```

전처리본 경로는 `IMAGE_LOG.enhanced_image_path`에 기록한다. 문제가 생기면
`.env.gemini.local`에서 아래 값만 바꾸고 서버를 재시작하여 원본 Plate BestShot OCR로
즉시 롤백할 수 있다. 이때 IVA 장면 OCR은 잘못된 전체 장면 전송을 막기 위해 생략된다.

```sh
PLATE_PREPROCESS_MODE='off'
```

API 키는 소스 코드나 Git 저장소에 넣지 말고 로컬 환경파일 또는 실행 환경에서만
설정한다.

```sh
export GEMINI_API_KEY='YOUR_LOCAL_API_KEY'
export GEMINI_MODEL='gemini-3-flash-preview'
export GEMINI_CONNECT_TIMEOUT_SEC='5'
export GEMINI_REQUEST_TIMEOUT_SEC='30'
```

`GEMINI_API_KEY`가 없으면 OCR만 비활성화되고 BestShot 저장과 나머지 서버 기능은
계속 동작한다. 실제 장비에서는 프로젝트 루트의 Git 비추적 로컬 파일
`.env.gemini.local`에 위 값을 넣으면 서버가 시작할 때 직접 읽는다. 같은 이름의
환경변수가 이미 설정되어 있으면 환경변수가 로컬 파일보다 우선한다.

정상 처리 시 다음 형식의 로그가 출력된다.

```text
[PLATE_OCR] slot=EV01 plate=12가3456 class=EV confidence=0.980000
```

주의사항:

실제 제출용 README에는 카메라 계정과 비밀번호를 절대 그대로 작성하지 않는다.
비밀번호에 !, @, #, % 같은 특수문자가 있으면 URL encoding이 필요할 수 있다.
예: ! 문자는 %21 형태로 변환할 수 있다.
9.2 단일 채널 실행 예시

카메라 1채널만 사용할 경우:

export CAMERA_ID='cam01'
export CAMERA_RTSP='rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'

export MQTT_HOST='localhost'
export MQTT_PORT='1883'
export CAMERA_EVENT_SUB_TOPIC='+/onvif-ej/#'
9.3 다중 채널 실행 예시

4채널을 사용할 경우:

export CAMERA_ID='cam01'

export CAMERA_RTSP_CH1='rtsp://USER:PASSWORD@CAMERA_IP:554/0/profile2/media.smp'
export CAMERA_RTSP_CH2='rtsp://USER:PASSWORD@CAMERA_IP:554/1/profile2/media.smp'
export CAMERA_RTSP_CH3='rtsp://USER:PASSWORD@CAMERA_IP:554/2/profile2/media.smp'
export CAMERA_RTSP_CH4='rtsp://USER:PASSWORD@CAMERA_IP:554/3/profile2/media.smp'

export MQTT_HOST='localhost'
export MQTT_PORT='1883'
export CAMERA_EVENT_SUB_TOPIC='+/onvif-ej/#'
10. 실행 방법
10.1 Mosquitto Broker 실행 확인

먼저 MQTT Broker가 실행 중인지 확인한다.

sudo systemctl status mosquitto

실행 중이 아니라면 다음 명령으로 시작한다.

sudo systemctl start mosquitto

부팅 시 자동 실행하려면 다음 명령을 사용할 수 있다.

sudo systemctl enable mosquitto
10.2 서버 실행

환경변수 설정 후 프로젝트 루트에서 실행한다.

cd ~/pi-server
./cmake-build/pi-server

정상 실행 시 다음과 유사한 로그가 출력된다.

[INFO] pi-server started
[INFO] camera_id=cam01
[INFO] channel_count=4
[INFO] headless mode: cv::imshow disabled
[INFO] snapshot mode: ALL CHANNELS FULL-SIZE original frame
[INFO] preview mode: 640x360 internal frame
[INFO] waiting for camera MQTT events...
[INFO] press Ctrl+C to stop
11. RTSP 수신 옵션

main.cpp에서는 OpenCV FFmpeg RTSP 옵션을 다음과 같이 설정한다.

setenv(
    "OPENCV_FFMPEG_CAPTURE_OPTIONS",
    "rtsp_transport;tcp|stimeout;5000000|max_delay;500000",
    0
);

의미는 다음과 같다.

옵션	의미
rtsp_transport;tcp	RTSP 스트림을 TCP 방식으로 수신
stimeout;5000000	연결 또는 수신 timeout 설정
max_delay;500000	FFmpeg 내부 지연 버퍼 제한

TCP 방식은 UDP보다 지연이 조금 있을 수 있지만, 패킷 손실이 있는 네트워크 환경에서는 더 안정적으로 동작할 수 있다.

12. MQTT 이벤트 테스트

카메라 또는 외부 장치에서 MQTT 이벤트가 들어오면 서버가 이벤트를 처리한다.

테스트를 위해 mosquitto_pub 명령을 사용할 수 있다.

mosquitto_pub \
  -h localhost \
  -p 1883 \
  -t "cam01/onvif-ej/ch01" \
  -m '{"camera_id":"cam01","channel_id":"ch01","event_type":"test"}'

단, 실제 payload 형식은 CameraEventParser.cpp의 파싱 규칙에 맞아야 한다.
Payload 형식이 다르면 MQTT 메시지는 수신되더라도 이벤트 파싱에 실패할 수 있다.

MQTT 수신 여부만 확인하고 싶다면 별도 터미널에서 다음 명령을 사용할 수 있다.

mosquitto_sub -h localhost -p 1883 -t '+/onvif-ej/#' -v
13. 스냅샷 저장 결과

이벤트가 정상적으로 처리되면 스냅샷은 다음 경로에 저장된다.

data/snapshots/

저장 파일 예시는 다음과 같다.

cam01_ch01_FULL_1280x720_20260709_180619_361.jpg
cam01_ch02_FULL_1280x720_20260709_180619_414.jpg
cam01_ch03_FULL_1280x720_20260709_180619_491.jpg
cam01_ch04_FULL_2592x1520_20260709_180619_585.jpg

파일명 규칙은 다음과 같다.

{camera_id}_{channel_id}_FULL_{width}x{height}_{yyyyMMdd}_{HHmmss}_{milliseconds}.jpg

예를 들어:

cam01_ch04_FULL_2592x1520_20260709_180621_534.jpg

위 파일은 다음 의미를 가진다.

항목	값
camera_id	cam01
channel_id	ch04
frame type	FULL
resolution	2592x1520
date	2026-07-09
time	18:06:21.534
format	JPG
14. SQLite DB 확인 방법

이벤트 DB는 기본적으로 다음 위치에 저장된다.

data/db/parking.db

SQLite CLI로 DB를 확인할 수 있다.

sqlite3 data/db/parking.db

SQLite 프롬프트에서 다음 명령을 사용할 수 있다.

.tables

테이블 구조 확인:

.schema

최근 이벤트 확인:

SELECT * FROM events ORDER BY id DESC LIMIT 10;

SQLite 종료:

.quit
15. Snapshot Test

Snapshot 검증은 `pi-server`의 RTSP/IVA 통합 흐름으로 수행한다.

실행 예시:

export CAMERA_RTSP='rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'

./cmake-build/pi-server

정상 동작하면 다음 경로에 테스트 이미지가 저장된다.

data/snapshots/test_snapshot.jpg

이 테스트는 GUI 없이 RTSP 연결, 프레임 수신, JPG 저장이 가능한지 확인할 때 유용하다.

16. 종료 방법

서버 실행 중 다음 키를 누르면 정상 종료된다.

Ctrl + C

서버는 종료 신호를 받으면 다음 순서로 정리한다.

1. MQTT Bridge stop
2. RTSP Receiver stop
3. Database close
4. pi-server stopped 로그 출력

비정상적으로 프로세스가 남아 있는 경우 다음 명령으로 종료할 수 있다.

pkill -f pi-server
17. 주요 로그 예시

정상 시작:

[INFO] pi-server started
[INFO] camera_id=cam01
[INFO] channel_count=4
[INFO] headless mode: cv::imshow disabled
[INFO] snapshot mode: ALL CHANNELS FULL-SIZE original frame
[INFO] preview mode: 640x360 internal frame
[INFO] waiting for camera MQTT events...
[INFO] press Ctrl+C to stop

RTSP URL 미설정:

[ERROR] No RTSP URL configured
[ERROR] HINT: export CAMERA_RTSP='rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'
[ERROR] HINT: or set CAMERA_RTSP_CH1~CAMERA_RTSP_CH4

정상 종료:

[INFO] pi-server stopped
18. 트러블슈팅
18.1 RTSP URL을 설정했는데 서버가 바로 종료되는 경우

확인할 것:

echo $CAMERA_RTSP
echo $CAMERA_RTSP_CH1
echo $CAMERA_RTSP_CH2
echo $CAMERA_RTSP_CH3
echo $CAMERA_RTSP_CH4

환경변수가 비어 있으면 다시 export 해야 한다.

18.2 RTSP 연결이 안 되는 경우

확인할 것:

카메라 IP가 Raspberry Pi에서 ping 되는지 확인
ping CAMERA_IP
RTSP 포트가 열려 있는지 확인
nc -vz CAMERA_IP 554
FFmpeg로 직접 재생 가능한지 확인
ffplay 'rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'
비밀번호 특수문자가 URL encoding 되었는지 확인

예:

!  -> %21
@  -> %40
#  -> %23
%  -> %25
18.3 MQTT 이벤트가 들어오지 않는 경우

Mosquitto가 실행 중인지 확인한다.

sudo systemctl status mosquitto

구독 토픽을 확인한다.

echo $CAMERA_EVENT_SUB_TOPIC

별도 터미널에서 직접 subscribe 테스트를 한다.

mosquitto_sub -h localhost -p 1883 -t '+/onvif-ej/#' -v

다른 터미널에서 publish 테스트를 한다.

mosquitto_pub -h localhost -p 1883 -t 'cam01/onvif-ej/ch01' -m '{"camera_id":"cam01","channel_id":"ch01","event_type":"test"}'
18.4 스냅샷이 저장되지 않는 경우

확인할 것:

data/snapshots 디렉터리가 존재하는지 확인
ls -al data/snapshots
쓰기 권한 확인
touch data/snapshots/test.txt
RTSP 초기 프레임 수신이 성공했는지 로그 확인
MQTT payload가 CameraEventParser 규칙에 맞는지 확인
18.5 DB가 생성되지 않는 경우

확인할 것:

ls -al data/db

DB 디렉터리가 없다면 생성한다.

mkdir -p data/db

SQLite 파일 확인:

sqlite3 data/db/parking.db ".tables"
18.6 카메라 계정/비밀번호가 로그에 노출되는 경우

UrlMasker 유틸리티를 사용하여 RTSP URL을 출력할 때 비밀번호를 숨겨야 한다.

잘못된 예:

rtsp://admin:password@192.168.0.10:554/profile2/media.smp

권장 출력:

rtsp://admin:****@192.168.0.10:554/profile2/media.smp
19. 현재 구현 상태

현재 프로젝트에서 확인 가능한 구현 상태는 다음과 같다.

항목	상태
RTSP 수신	구현
다중 채널 설정	구현
Headless 실행	구현
초기 프레임 대기	구현
MQTT 이벤트 브릿지	구현
Snapshot 저장	구현
SQLite 이벤트 DB	구현
URL 마스킹 유틸	구현
모듈 분리 구조	구현
Qt GUI 표시	별도 연동 예정
STM32 연동	별도 연동 예정
OpenCV 보정 필터	확장 예정
OpenSSL/TLS 보안 통신	후순위 확장 예정
20. 향후 확장 방향
20.1 Qt 관제 클라이언트 연동

현재 서버는 Headless 모드로 실행된다.
향후 Qt 클라이언트와 연동하면 다음 기능을 추가할 수 있다.

4채널 영상 표시
이벤트 목록 표시
스냅샷 이미지 조회
채널별 상태 표시
이벤트 발생 시 UI 알림
SQLite DB 조회 또는 서버 API 연동
20.2 OpenCV 영상 전처리

RTSP 수신 후 preview frame 또는 원본 frame에 OpenCV 전처리를 적용할 수 있다.

예상 기능:

Resize
ROI crop
Brightness/contrast 보정
Gaussian blur
Edge detection
Motion detection
번호판 영역 추출
객체 감지 전처리
20.3 STM32 외부 장치 연동

STM32는 외부 센서와 출력 장치를 담당할 수 있다.

예상 장치:

지자기 센서
LED 경고등
Buzzer
차단기 또는 바리케이드
UART 기반 Raspberry Pi 연동
MQTT 또는 Serial Gateway 연동

예상 흐름:

STM32 Sensor Event
        ↓ UART / MQTT Gateway
Raspberry Pi Server
        ↓
EventDatabase
        ↓
Qt Client / Snapshot / Alert
20.4 보안 기능 확장

향후 보안 강화를 위해 다음 기능을 추가할 수 있다.

MQTT TLS 적용
인증 기반 MQTT 연결
RTSP 계정 정보 외부 파일 분리
.env 파일 사용
OpenSSL 기반 통신 암호화
DB 접근 권한 제한
로그 내 민감정보 마스킹 강화
21. 제출 시 주의사항

제출 전에 반드시 다음 항목을 확인한다.

실제 카메라 IP 제거
실제 카메라 계정 제거
실제 카메라 비밀번호 제거
data/snapshots 내 불필요한 테스트 이미지 정리
parking.db에 민감한 로그가 있으면 삭제 또는 샘플 DB로 교체
빌드 산출물 포함 여부 확인
README의 실행 방법이 현재 코드와 일치하는지 확인
MQTT topic 예시가 실제 설정과 맞는지 확인

민감정보가 포함될 수 있는 항목:

RTSP URL
카메라 IP
카메라 ID
카메라 계정
카메라 비밀번호
MQTT Broker 주소
이벤트 payload 원문
스냅샷 이미지
SQLite DB
22. 제출 전 정리 명령 예시

불필요한 빌드 파일이나 테스트 결과를 정리할 때 사용할 수 있다.

rm -f data/snapshots/test_snapshot.jpg

테스트 스냅샷 전체를 삭제하려면 주의해서 실행한다.

rm -f data/snapshots/*.jpg

DB를 초기화해야 하는 경우:

rm -f data/db/parking.db

단, DB를 삭제하면 기존 이벤트 로그가 모두 사라진다.

23. 프로젝트 실행 요약

가장 기본적인 실행 순서는 다음과 같다.

cd ~/pi-server

sudo apt update

sudo apt install -y \
  build-essential \
  pkg-config \
  cmake \
  ffmpeg \
  libopencv-dev \
  mosquitto \
  mosquitto-clients \
  libmosquitto-dev \
  sqlite3 \
  libsqlite3-dev

cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j2

export CAMERA_ID='cam01'
export CAMERA_RTSP_CH1='rtsp://USER:PASSWORD@CAMERA_IP:554/0/profile2/media.smp'
export CAMERA_RTSP_CH2='rtsp://USER:PASSWORD@CAMERA_IP:554/1/profile2/media.smp'
export CAMERA_RTSP_CH3='rtsp://USER:PASSWORD@CAMERA_IP:554/2/profile2/media.smp'
export CAMERA_RTSP_CH4='rtsp://USER:PASSWORD@CAMERA_IP:554/3/profile2/media.smp'

export MQTT_HOST='localhost'
export MQTT_PORT='1883'
export CAMERA_EVENT_SUB_TOPIC='+/onvif-ej/#'

sudo systemctl start mosquitto

./cmake-build/pi-server

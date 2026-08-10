# IVA ROI 좌표 확인 도구

## 목적

`check_coordinates`는 카메라 기준 이미지에서 주차면을 선택하고 Pi 서버가 사용하는
0~1 정규화 ROI 좌표를 출력하는 독립 C++/OpenCV 도구다. 운영 `pi-server`와 별도
프로세스로 실행하며 서버 설정이나 DB를 자동으로 변경하지 않는다.

구현 파일은 다음과 같다.

- `tools/check_coordinates.cpp`: 이미지/RTSP 입력, ROI 선택, 좌표 계산, 미리보기 저장
- `CMakeLists.txt`: `check_coordinates` 실행 파일 및 자동 테스트 등록
- `tests/fixtures/roi_checker_test.ppm`: headless 좌표 계산 테스트 입력

## 빌드

```bash
cmake -S . -B cmake-build
cmake --build cmake-build --target check_coordinates -j4
```

실행 파일 위치:

```text
cmake-build/check_coordinates
```

## 기존 이미지에서 마우스로 선택

그래픽 세션 또는 X forwarding이 가능한 터미널에서 실행한다.

```bash
./cmake-build/check_coordinates \
  --image data/reference/ch01.jpg \
  --slot EV01
```

창에서 주차면을 드래그하고 Enter 또는 Space를 누른다. 취소는 `c` 키다.

## RTSP 최신 프레임에서 선택

카메라 비밀번호가 명령행이나 저장소에 노출되지 않도록 RTSP URL은 환경변수로
전달한다.

```bash
export CAMERA_RTSP='rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'
./cmake-build/check_coordinates --rtsp-env CAMERA_RTSP --slot EV01
```

소스 옵션을 생략하면 `CAMERA_RTSP_CH1`, `CAMERA_RTSP` 순서로 찾는다.

## GUI 없는 SSH 환경

### 브라우저 웹 모드

VS Code Remote SSH에서는 로컬 포트 전달을 사용해 브라우저에서 직접 영역을
드래그할 수 있다. 기본 바인딩은 외부에 노출되지 않는 `127.0.0.1:8091`이다.

```bash
./cmake-build/check_coordinates \
  --rtsp-env CAMERA_RTSP \
  --slot EV01 \
  --web \
  --port 8091
```

VS Code의 **PORTS** 탭에서 8091 포트를 전달한 뒤 로컬 브라우저로
`http://127.0.0.1:8091`을 연다. `새 프레임 가져오기`를 누르면 ROI 도구가
RTSP에서 현재 프레임 한 장을 다시 받아 화면을 교체한다. 이 도구는 지속적으로
영상을 수신하지 않고 버튼을 누른 시점에만 RTSP 연결을 생성한다.

화면에서 슬롯을 선택하고 영역을 드래그한 다음 `좌표 저장 및 즉시 적용`을 누른다.
실행 중인 Pi 서버의 ROI PUT API가 호출되며
SQLite 저장 성공 직후 다음 촬영부터 새 좌표를 사용한다. 서버 재시작은 필요하지 않다.

평상시에는 다음 명령만 사용한다.

```bash
./run_roi.sh
```

좌표를 즉시 적용하려면 Pi 서버도 실행 중이어야 한다.

```bash
./run_server.sh
```

웹 화면은 카메라 사진을 `<img>`로 표시하고 투명한 Canvas에는 선택 영역만
그린다. 새 프레임으로 교체하면 이전 드래그 영역은 초기화되므로 다시 선택해야
한다. 사진이 브라우저 캐시에 남지 않도록 페이지와 프레임 모두 `no-store`로
제공한다.

같은 네트워크의 다른 PC에서 Pi IP로 직접 접속할 때만 다음처럼 외부 바인딩을
명시한다. 이 개발 도구에는 인증이 없으므로 신뢰할 수 있는 내부망에서만 사용한다.

```bash
./cmake-build/check_coordinates \
  --rtsp-env CAMERA_RTSP \
  --web --bind 0.0.0.0 --port 8091
```

```text
http://PI_IP:8091
```

### 좌표 직접 입력 모드

픽셀 좌표 `x,y,width,height`를 직접 전달하면 창을 열지 않고 정규화 좌표와
확인용 이미지를 생성한다.

```bash
./cmake-build/check_coordinates \
  --image data/reference/ch01.jpg \
  --slot EV01 \
  --rect 100,300,400,600
```

출력 예시:

```text
image_size=1920x1080
slot_id=EV01
pixel_roi=100,300,400,600
IVA_EV01_ROI_X=0.052083
IVA_EV01_ROI_Y=0.277778
IVA_EV01_ROI_WIDTH=0.208333
IVA_EV01_ROI_HEIGHT=0.555556
preview_path=data/roi_checks/EV01_roi_preview.jpg
```

웹 모드는 출력과 함께 Pi 서버 API 및 SQLite `SYSTEM_SETTINGS`에 좌표를 즉시
반영한다. 직접 입력 모드는 좌표와 미리보기만 생성한다.

현재 좌표 조회:

```bash
curl -sS http://127.0.0.1:8080/api/v1/settings/parking-slots/roi
curl -sS http://127.0.0.1:8080/api/v1/settings/parking-slots/EV01/roi
```

## 테스트

```bash
ctest --test-dir cmake-build -R check-coordinates-test --output-on-failure
```

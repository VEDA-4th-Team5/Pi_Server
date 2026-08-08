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
`http://127.0.0.1:8091`을 연다. 화면에서 슬롯을 선택하고 영역을 드래그한 다음
`좌표 저장 및 출력`을 누른다.

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

출력한 `IVA_EV01_ROI_*` 네 줄을 실행 환경에 적용하면 된다. 미리보기에는 선택한
사각형과 슬롯 ID가 표시된다.

## 테스트

```bash
ctest --test-dir cmake-build -R check-coordinates-test --output-on-failure
```

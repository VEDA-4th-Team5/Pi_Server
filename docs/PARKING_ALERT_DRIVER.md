# `/dev/parking_alert` Linux Character Device

## 구현 상태

EVDA-90의 목표에 맞춰 기존 `/dev/tty*`를 대신하는 것이 아니라, 프로젝트 전용 Linux
문자 디바이스 `/dev/parking_alert`를 생성하는 out-of-tree 커널 모듈을 구현했다.

```text
Pi Server / parking-alert-ctl
        ↓ ioctl, read, write, poll
/dev/parking_alert
        ↓ parking_alert.ko
32개 주차면 알림 bit 상태
```

커널은 `EV01` 같은 업무 ID를 해석하지 않는다. 0~31의 안정적인 슬롯 번호와 활성
비트만 관리하며, 주차면 ID 매핑과 알림 발생 정책은 C++ 서버가 담당한다.

## 코드 위치

| 위치 | 역할 |
|---|---|
| `driver/parking_alert/parking_alert.c` | 문자 디바이스 커널 모듈 |
| `driver/parking_alert/Kbuild`, `Makefile` | 현재 Raspberry Pi 커널용 `.ko` 빌드 |
| `driver/parking_alert/99-parking-alert.rules` | `dialout` 그룹에 장치 접근 허용 |
| `include/uapi/parking_alert.h` | 커널과 C/C++ 프로그램이 공유하는 ABI |
| `tools/parking_alert_ctl.c` | 상태 조회·변경·감시 CLI |
| `include/device/LinuxDriverAdapter.hpp` | Pi C++ 서버용 RAII 어댑터 |
| `src/device/LinuxDriverAdapter.cpp` | `open/ioctl/close` 구현 |

## 제공 기능

- 최대 32개 슬롯의 활성 알림을 `active_mask`로 관리
- `SET_SLOT`, `CLEAR_SLOT`, `CLEAR_ALL`, `GET_STATE` ioctl
- 같은 상태의 반복 설정은 새 이벤트로 계산하지 않는 idempotency
- 상태가 바뀔 때만 증가하는 `generation`
- 업무 이벤트와 연결할 수 있는 64-bit `event_id`
- `read()`로 현재 상태 수신, 다음 read는 상태가 바뀔 때까지 대기
- `poll()`로 busy-wait 없이 변경 통지
- 고정 크기 UAPI와 API version 검증

이 모듈은 현재 알림 상태와 사용자 공간 통지 경계를 담당한다. GPIO 핀, UART, LoRa
무선 설정을 커널에 하드코딩하지 않는다. 향후 C++ 서버가 위반 이벤트를 장치에 기록하고,
별도 UART/LoRa 계층이 해당 상태를 STM32 LED/Buzzer로 전달할 수 있다.

## 빌드

일반 서버 빌드는 사용자 도구와 C++ 어댑터를 만든다.

```bash
cmake -S . -B build
cmake --build build -j2
```

커널 모듈은 Raspberry Pi에 설치된 현재 커널 헤더와 정확히 맞춰 별도로 빌드한다.

```bash
cmake --build build --target parking-alert-module
# 또는
make -C driver/parking_alert
```

생성 파일은 `driver/parking_alert/parking_alert.ko`다.

커널 6.1의 `class_create()` API 차이와 `/dev/parking_alert` 미생성 장애 기록은
[`TROUBLESHOOTING.md`의 TS-021](TROUBLESHOOTING.md)을 참고한다.

## 설치·자동 적재 및 권한

현재 커널에 맞춰 모듈을 빌드하고 설치한 뒤, 재부팅 시 자동 적재되도록 설정한다.

```bash
./tools/install_parking_alert_driver.sh
```

스크립트는 모듈을 `/lib/modules/$(uname -r)/extra`에 설치하고
`/etc/modules-load.d/parking-alert.conf`와 udev 권한 규칙을 등록한다. 설치 후에는 다음으로
확인한다.

```bash
lsmod | grep '^parking_alert'
ls -l /dev/parking_alert
./cmake-build/parking-alert-ctl status
```

수동으로 임시 적재하려면 다음 명령을 사용한다.

```bash
sudo insmod driver/parking_alert/parking_alert.ko
ls -l /dev/parking_alert
```

일반 사용자 접근을 영구 적용하려면 다음 규칙을 설치한 뒤 udev를 갱신한다.

```bash
sudo install -m 0644 driver/parking_alert/99-parking-alert.rules \
  /etc/udev/rules.d/99-parking-alert.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=parking_alert
```

현재 계정이 `dialout` 그룹에 포함돼 있어야 한다. 규칙 적용 전에는 제어 도구를 `sudo`로
실행한다.

## 수동 테스트

```bash
sudo ./build/parking-alert-ctl status
sudo ./build/parking-alert-ctl set 0 1001
sudo ./build/parking-alert-ctl set 3 1002
sudo ./build/parking-alert-ctl clear 0 1003
sudo ./build/parking-alert-ctl clear-all
```

`write()` 경로도 별도로 검사할 수 있다.

```bash
sudo ./build/parking-alert-ctl write-set 7 7001
sudo ./build/parking-alert-ctl write-clear 7 7002
```

터미널 A에서 `poll()` 기반 감시를 실행하고 터미널 B에서 상태를 변경한다.

```bash
sudo ./build/parking-alert-ctl watch
```

```bash
sudo ./build/parking-alert-ctl set 1 2001
```

해제는 장치를 사용하는 프로세스를 종료한 뒤 수행한다.

```bash
sudo rmmod parking_alert
```

## Pi 서버 연동 정책

서버는 `PARKING_ALERT_DRIVER_ENABLED=true`일 때 `/dev/parking_alert`를 연다. 슬롯과
커널 bit의 대응은 `PARKING_ALERT_SLOT_MAP`으로 설정한다.

```text
EV01:0,EV02:1,EV03:2,EV04:3,EV05:4,EV06:5,EV07:6,EV08:7
```

- `NON_EV_ALERT`, `VIOLATION_TRIGGERED`: 해당 슬롯 bit SET
- `DEPARTURE`: 해당 슬롯 bit CLEAR
- 서버 시작: 전체 bit를 초기화한 뒤 DB의 활성 `VIOLATION` 세션을 복원
- 드라이버 미설치 또는 ioctl 실패: 오류만 기록하고 DB, MQTT, 촬영 및 주차 판정은 계속 수행

따라서 문자 디바이스는 서버의 주차 상태를 결정하는 입력이 아니라, 이미 확정된 위반 상태를
커널 장치로 전달하는 선택적 출력 계층이다.

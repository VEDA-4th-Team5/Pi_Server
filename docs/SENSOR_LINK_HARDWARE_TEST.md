# STM32 센서 링크 하드웨어 승인 테스트

- 대상 실행 파일: `sensor-link-hardware-test`
- 소스: `tests/hardware/SensorLinkHardwareAcceptanceTest.cpp`
- 대상 장치: STM32 Hall 센서, Flame 센서, UART 또는 투명 UART LoRa 모뎀
- 실행 환경: GUI가 없는 Raspberry Pi Linux

## 1. 목적

기존 `uart-lora-driver-test`는 PTY와 캡처된 frame을 이용해 UART byte transport,
LoRa CRC16, partial frame 복원을 자동 검증한다. 이 문서의 테스트는 실제 STM32와
센서를 사람이 조작하며 다음 항목을 PASS/FAIL로 판정하는 반자동 HIL(Hardware in
the Loop) 승인 시험이다.

- 실제 UART 장치 open 및 8N1 통신
- Hall 센서별 `VACANT → OCCUPIED → VACANT` 상태 전이
- Hall 센서 간 cross-talk
- Flame 센서 `CLEARED → DETECTED → CLEARED` 상태 전이
- 감지 및 해제 지연시간
- STM 노드/센서 ID와 sequence 역행
- 지원하지 않는 원문 또는 손상된 payload

이 도구는 DB, 카메라, Qt, Gemini를 사용하지 않는다. 센서 링크와 STM32 출력만
분리해서 검증하므로 전체 E2E 실패 시 하드웨어/통신 계층을 먼저 배제하는 데 쓴다.

## 2. 안전 조건

`pi-server`와 이 테스트가 같은 UART를 동시에 읽으면 byte가 두 프로세스에 나뉘어
둘 다 잘못된 frame을 받을 수 있다. 기본 동작은 실행 중인 `pi-server`를 발견하면
테스트를 `SKIP`하는 것이다.

```bash
pgrep -a pi-server || true
```

서버가 실행 중이면 정상 종료한다.

```bash
pkill -INT -x pi-server 2>/dev/null || true
```

`--allow-running-server`는 서로 다른 물리 UART를 쓴다는 사실을 확인한 경우에만
사용한다. 동일 UART 공유를 허용하는 옵션이 아니다.

불꽃 시험은 환기되고 주변에 인화물이 없는 장소에서 담당자가 통제된 작은 화염으로
수행한다. 센서에 직접 열을 가하거나 장시간 불꽃을 유지하지 않는다.

## 3. 빌드

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build --target sensor-link-hardware-test -j4
```

도움말:

```bash
./cmake-build/sensor-link-hardware-test --help
```

## 4. 장치 확인

STM32 USB CDC/USB-UART인 경우:

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

Raspberry Pi 보드 UART 또는 LoRa 모뎀인 경우:

```bash
ls -l /dev/serial0 /dev/ttyAMA* 2>/dev/null
```

현재 사용자가 장치 그룹에 속하는지 확인한다.

```bash
id
```

권한이 없으면 운영 정책에 따라 `dialout` 그룹 또는 udev 규칙을 수정한다. 테스트를
편하게 실행하기 위해 UART 장치 권한을 `666`으로 바꾸지 않는다.

## 5. 직접 실행

### 5.1 UART line 모드 전체 시험

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --baud 115200 --mode uart-line --scenario all
```

기본 센서 ID는 다음과 같다.

```text
Hall:  HALL01,HALL02
Flame: FLAME01
```

다른 ID를 사용하면 명시한다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario all --hall-sensors HALL01,HALL02 --fire-sensor FLAME01
```

### 5.2 LoRa frame 모드 전체 시험

```bash
./cmake-build/sensor-link-hardware-test --device /dev/serial0 --baud 115200 --mode lora-frame --scenario all
```

### 5.3 Hall 센서만 시험

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario hall --hall-sensors HALL01,HALL02
```

도구가 출력하는 순서대로 대상물을 제거하고 접근시킨다. 기능 확인만 할 때는 Enter
입력이 필요하지 않다. 포트폴리오 지연시간을 측정할 때는 `--confirm-actions`를 사용해
각 물리 동작 직전에 Enter를 눌러 측정 시작 조건을 통일한다. 각 단계는 기본 30초 동안
기대 상태를 기다린다.

```text
[ACTION] Remove every target from HALL01 and wait for VACANT.
[ACTION] Place the vehicle/magnet over HALL01.
[ACTION] Remove the vehicle/magnet from HALL01.
```

### 5.4 Flame 센서만 시험

최근 FFT window 때문에 불꽃 제거 직후에도 `DETECTED`가 유지될 수 있다. 도구는 Pi에서
다시 화재 여부를 계산하지 않고 STM32가 보낸 `CLEARED`까지의 실제 시간을 측정한다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario fire --fire-sensor FLAME01 --event-timeout-ms 60000
```

### 5.5 수신 원문 관찰

상태 전이 순서를 강제하지 않고 60초 동안 protocol 유효성과 sequence만 확인한다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario observe --observe-seconds 60
```

### 5.6 반복 측정 및 결과 파일 생성

Hall 센서 2개를 각각 30회 시험하고 원시 CSV와 Markdown 요약을 함께 생성한다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --baud 115200 --mode uart-line --scenario hall --hall-sensors HALL01,HALL02 --repeat 30 --confirm-actions --firmware-version STM32_FW_VERSION --hardware-id STM32_NODE_ID --report-dir data/test-results/hardware
```

Flame 센서는 안전을 위해 Hall과 분리하여 1회 예비시험 후 반복 횟수를 늘린다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --baud 115200 --mode uart-line --scenario fire --fire-sensor FLAME01 --repeat 20 --confirm-actions --event-timeout-ms 60000 --firmware-version STM32_FW_VERSION --hardware-id STM32_NODE_ID --report-dir data/test-results/hardware
```

`--run-id`를 생략하면 UTC 시각 기반 ID를 자동 생성한다. 실행이 끝나면 같은 run ID의
두 파일이 생성된다.

```text
data/test-results/hardware/hil-YYYYMMDDTHHMMSSZ.csv
data/test-results/hardware/hil-YYYYMMDDTHHMMSSZ.md
```

CSV는 trial별 원시 결과를 보존한다. Markdown은 센서·상태별 시도 수, PASS/FAIL,
성공률, 최소·평균·P95·최대 지연시간을 제공하므로 Confluence와 포트폴리오에 사용할 수
있다.

지연시간 제한 옵션은 요구사항이 합의된 경우에만 추가한다. 옵션을 생략하면 timeout만
FAIL 기준으로 사용하고 측정값은 그대로 기록한다.

```bash
--hall-detect-limit-ms 7000 \
--hall-clear-limit-ms 5000 \
--fire-detect-limit-ms 3000 \
--fire-clear-limit-ms 10000
```

위 숫자는 명령 형식 예시이며 프로젝트의 확정 요구사항이 아니다. 초기 측정 결과를
근거로 팀이 합격 기준을 정한 뒤 사용한다.

`--confirm-actions` 측정값도 사용자 조작시간을 완전히 제거한 센서 단독 지연시간은
아니다. 보고서에는 `operator-confirmed-to-event`로 기록하며, 완전 자동 지연 측정에는
별도의 자극 장치가 필요하다.

## 6. CTest 하드웨어 라벨

일반 CTest는 실제 UART를 열지 않는다.

```bash
ctest --test-dir cmake-build -R '^sensor-link-hardware-test$' --output-on-failure
```

기대 결과:

```text
sensor-link-hardware-test ... Skipped
```

실기기 CTest를 명시적으로 활성화한다.

```bash
RUN_HARDWARE_TESTS=1 SENSOR_LINK_MODE=uart-line SENSOR_UART_DEVICE=/dev/ttyACM0 SENSOR_UART_BAUD=115200 ctest --test-dir cmake-build -L hardware --output-on-failure -V
```

CTest 실행 중에도 화면의 `[ACTION]` 순서에 맞춰 센서를 조작해야 한다.

통계·CSV·Markdown 생성 코드는 물리 장치 없이 별도 자동 테스트로 검증한다.

```bash
ctest --test-dir cmake-build -R '^hardware-test-report-test$' --output-on-failure
```

반복 시험은 기본 CTest timeout을 초과할 수 있으므로 위 5.6절의 실행 파일을 직접
사용한다.

## 7. 판정 기준

| 항목 | PASS | FAIL |
|---|---|---|
| UART 연결 | 제한 시간 내 장치 open | 장치 없음, 권한/termios 오류, timeout |
| Hall baseline | 해당 ID의 `VACANT` 수신 | 잘못된 ID 또는 timeout |
| Hall 감지 | 해당 ID의 `OCCUPIED` 수신 | timeout |
| Hall 해제 | 해당 ID의 `VACANT` 재수신 | timeout |
| Cross-talk | 기본 2초 관찰 중 다른 Hall이 `OCCUPIED` 아님 | 다른 Hall `OCCUPIED` 수신 |
| Flame 감지 | `DETECTED` 수신 | timeout |
| Flame 해제 | 불꽃 제거 후 `CLEARED` 수신 | timeout |
| Sequence | 같은 STM 노드 또는 센서에서 역행 없음 | 이전 값보다 작은 sequence |
| 원문 | 모든 줄이 지원 grammar로 파싱됨 | 미지원/손상 원문 수신 |

지연시간 제한 옵션을 지정했다면 상태가 timeout 전에 도착해도 제한값을 초과한 trial은
FAIL로 기록한다. 제한 옵션이 없으면 측정값만 보존한다.

동일 sequence 재전송은 WARN으로 집계하지만 바로 FAIL하지 않는다. QoS/무선 재전송일 수
있기 때문이다. 이전 값보다 작은 sequence는 재부팅 epoch 정보 없이 처리하면 stale
event를 받아들일 수 있으므로 FAIL한다.

## 8. 출력 예시

```text
[PASS] sensor link connected mode=uart-line device=/dev/ttyACM0 baud=115200
[RX] transport=uart sensor=HALL01 state=VACANT sequence=101
[PASS] HALL01 baseline VACANT received
[ACTION] Place the vehicle/magnet over HALL01.
[RX] transport=uart sensor=HALL01 state=OCCUPIED sequence=107
[PASS] HALL01 OCCUPIED latency=5031ms
[PASS] HALL01 cross-talk not observed
[ACTION] Remove the vehicle/magnet from HALL01.
[RX] transport=uart sensor=HALL01 state=VACANT sequence=112
[PASS] HALL01 VACANT latency=3012ms
[RX] transport=uart sensor=FLAME01 state=DETECTED sequence=130 energy=43.7
[PASS] FLAME01 DETECTED latency=821ms energy=43.700000
[RX] transport=uart sensor=FLAME01 state=CLEARED sequence=136 energy=2.1
[PASS] FLAME01 CLEARED latency=6045ms

RESULT: 12 passed, 0 failed
```

## 9. 문제 해결

### `pi-server is running`

같은 UART를 서버가 사용 중이다. 서버를 정상 종료한 뒤 다시 실행한다.

### `UART device does not exist`

장치 이름이 변경됐거나 USB가 분리됐다. `dmesg`와 `/dev/ttyACM*`, `/dev/ttyUSB*`를
확인한다.

### `UART did not connect`

장치 권한, baud rate, 다른 프로세스의 점유, termios 지원 여부를 확인한다.

### `unparsed sensor line`

STM32 debug 문자열이 protocol UART에 섞였거나 Pi/STM32 protocol 버전이 다르다.
원인을 확인하는 동안만 `--allow-unparsed`로 WARN 처리할 수 있으며 최종 승인 시험에서는
사용하지 않는다.

### Hall cross-talk FAIL

다른 Hall 위에 금속/차량이 없는지 먼저 확인한다. 조건이 동일한데 반복되면 센서 간격,
배선, 임계값 및 접지 상태를 점검한다.

## 10. 전체 E2E와의 관계

이 테스트가 PASS해도 DB, 촬영, OCR, Qt 전달까지 검증된 것은 아니다. 다음 순서로 범위를
확장한다.

```text
sensor-link-hardware-test
→ pi-server UART/LoRa 입력
→ Hall/Fire 상태 전이
→ SQLite/MQTT
→ Snapshot/OCR
→ Qt 표시
```

전체 시스템 검증은 `docs/HARDWARE_E2E_TEST_GUIDE.md`를 따른다.

## 11. 센터 실기기 시험 및 포트폴리오 완성 체크리스트

다음 순서는 2026-08-21 센터에서 수행할 작업이다. 측정값이나 PASS 결과를 미리 작성하지
않고 실제 결과 파일을 기준으로 채운다.

### 11.1 시험 전 준비

- [ ] 현재 시험 브랜치와 Git commit을 기록한다.
- [ ] STM32 펌웨어 버전과 보드/노드 ID를 기록한다.
- [ ] Raspberry Pi, STM32, Hall 센서 2개, Flame 센서, LoRa 배선을 촬영한다.
- [ ] `ls -l /dev/ttyACM* /dev/ttyUSB* /dev/serial0`로 장치를 확인한다.
- [ ] `pi-server`가 동일 UART를 사용 중이면 정상 종료한다.
- [ ] 전체 소프트웨어 CTest 결과를 저장한다.

### 11.2 예비시험

- [ ] `--repeat 1`로 Hall 센서별 VACANT/OCCUPIED/VACANT를 확인한다.
- [ ] `--repeat 1`로 Flame CLEARED/DETECTED/CLEARED를 확인한다.
- [ ] 센서 ID, baud, UART mode가 실제 장비와 일치하는지 확인한다.
- [ ] 잘못된 ID나 unparsed 원문이 있으면 반복 시험 전에 해결한다.

### 11.3 포트폴리오 본시험

- [ ] HALL01 점유·해제 30회 결과를 생성한다.
- [ ] HALL02 점유·해제 30회 결과를 생성한다.
- [ ] Hall cross-talk 발생 횟수를 확인한다.
- [ ] Flame 감지·해제 20회 결과를 생성한다.
- [ ] Flame FFT energy와 해제 지연을 확인한다.
- [ ] 가능하면 UART 또는 LoRa 분리·재연결 로그를 별도로 수집한다.
- [ ] CSV와 Markdown 파일이 생성됐는지 확인한다.

### 11.4 증거 수집

- [ ] 테스트 실행 전체 화면을 캡처한다.
- [ ] Hall 센서 자극 전·후 사진을 촬영한다.
- [ ] 안전하게 통제된 Flame 시험 사진을 촬영한다.
- [ ] 결과 Markdown의 성공률·평균·P95·최대값 표를 캡처한다.
- [ ] FAIL이 있다면 원문 로그와 재현 조건을 보존한다.

### 11.5 포트폴리오 정리

- [ ] 요구사항과 PASS/FAIL 기준을 실제 측정 결과에 맞게 확정한다.
- [ ] 대표 결함 1건 이상을 `현상 → 재현 → 원인 → 수정 → 회귀시험`으로 작성한다.
- [ ] 임의 수치를 만들지 않고 실제 CSV 값만 사용한다.
- [ ] API Key, 비밀번호, 개인 네트워크 정보는 증거에서 제거한다.
- [ ] Confluence의 테스트 케이스에 결과와 증거 링크를 연결한다.
- [ ] 포트폴리오에는 전체 원문 대신 핵심 표·결함 분석·자동화 코드를 요약한다.

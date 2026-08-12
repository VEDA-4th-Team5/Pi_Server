# Pi Server 트러블슈팅 기록

- 기준일: 2026-08-12
- 대상: Raspberry Pi C++ 서버, Mosquitto, Hanwha Vision Camera, STM32 UART, SQLite, Qt 연동
- 원칙: 새 장애가 발생하면 이 문서에 증상, 확인 명령, 원인, 해결, 재발 방지를 추가한다.

비밀번호, API Key, RTSP 인증정보는 로그와 문서에 기록하지 않는다. 아래 명령의 IP와 장치
경로는 실행 환경에 맞게 바꾼다.

## 1. 장애 기록 양식

```text
### TS-XXX 제목

- 발생일:
- 상태: 조사 중 / 임시 해결 / 해결 / 실기기 재검증 필요
- 증상:
- 영향 범위:
- 확인 명령:
- 확인 결과:
- 원인:
- 해결:
- 재발 방지:
- 관련 파일/이슈:
```

## 2. 공통 1분 점검

```bash
git status -sb
hostname -I
pgrep -a pi-server
ss -lntp | grep ':8080'
ss -ntp | grep ':1883'
systemctl is-active mosquitto
curl --max-time 3 http://127.0.0.1:8080/api/v1/health
tail -n 100 data/logs/pi-server.log
```

실시간 주요 로그:

```bash
tail -n 0 -F data/logs/pi-server.log \
  | grep --line-buffered -E 'FIRE_ALARM|PARKING_SESSION|HALL|CAPTURE|OCR|ERROR|WARN'
```

`rg`가 설치되지 않은 환경에서는 위처럼 `grep`을 사용한다.

## 3. HTTP API 접속 거부

### TS-001 Pi IP는 맞지만 8080 접속 불가

- 상태: 해결 방법 확인
- 증상: `http://<PI_IP>:8080` 또는 Qt 연결이 즉시 거부된다.
- 확인 결과 사례: Pi IP는 `172.20.32.97`이었지만 `pi-server` 프로세스와 8080 listener가 없었다.
- 원인: IP 문제가 아니라 서버 프로세스가 실행되지 않은 상태였다.

확인:

```bash
hostname -I
pgrep -a pi-server
ss -lntp | grep ':8080'
curl --max-time 3 http://127.0.0.1:8080/api/v1/health
```

해결:

```bash
./run_server.sh
```

정상 확인 주소:

```text
http://<PI_IP>:8080/api/v1/health
```

정상 응답:

```json
{"service":"pi-server","status":"ok"}
```

루트 `/`는 health API가 아니다. Qt의 API base URL과 실제 Pi IP가 일치하는지도 확인한다.

## 4. Qt 화재 UI가 반응하지 않음

### TS-002 Pi 화재 MQTT는 정상인데 Qt 경고가 표시되지 않음

- 상태: Pi 정상 확인, Qt 구독·파서·채널 매핑 확인 필요
- 증상: CH1 화재를 발행해도 Qt UI가 움직이지 않는다.
- 확인 결과 사례:
  - `parking/fire/ch01` retained 메시지 존재
  - `event_id`, `event_type=FIRE_SUSPECTED`, `channel_id=ch01` 존재
  - Pi HTTP API 8080은 `200 OK`
  - Qt PC가 Pi Mosquitto 1883에 TCP 연결된 상태

Pi 확인:

```bash
mosquitto_sub -h localhost -v -C 1 -W 2 -t 'parking/fire/ch01'
ss -ntp | grep ':1883'
curl --max-time 3 http://<PI_IP>:8080/api/v1/health
```

최소 화재 계약:

```json
{
  "event_id": "fire-001",
  "event_type": "FIRE_SUSPECTED",
  "channel_id": "ch01"
}
```

운영 토픽 분리:

```text
parking/fire/{channel_id}       채널별 최신 화재 상태, retain=true
parking/v1/events/{channel_id}  실시간 이벤트, retain=false
parking/v1/state/{slot_id}      주차 상태 전용, retain=true
```

Qt에서 확인할 항목:

1. MQTT가 활성화됐는가.
2. 실제 설정 파일이 `parking/fire/+`를 구독하는가.
3. 수정한 Qt 실행파일을 다시 빌드해 실행했는가.
4. `ch01`을 CH1 UI 모델로 변환하는가.
5. 빈 `slot_id` 때문에 채널 화재 메시지를 버리지 않는가.
6. retained 메시지를 팝업 중복 없이 현재 상태에 적용하는가.
7. `OPEN` 또는 `active=true`만으로 화재를 판정하지 않고 `FIRE_SUSPECTED`를 확인하는가.

Pi에서 최소 payload까지 수신 확인됐는데 UI가 반응하지 않으면 Pi 발행 문제가 아니라 Qt의
구독, JSON 파싱 또는 UI 매핑 문제로 분류한다.

### TS-003 주차 상태와 화재 상태가 서로 덮어씀

- 상태: EVDA-159에서 해결
- 원인: 주차와 화재를 동일 retained state 토픽에 발행하면 마지막 메시지가 이전 상태를 지운다.
- 해결: 화재는 `parking/fire/{channel_id}`, 주차는 `parking/v1/state/{slot_id}`로 분리했다.
- 검증: `EV01 OCCUPIED + ch01 FIRE_SUSPECTED`가 동시에 존재할 수 있어야 한다.

### TS-003A Qt Check 후에도 화재가 계속 깜빡임

- 상태: Pi 화재 ACK 경로 구현, Qt 명령·UI 상태 적용 확인 필요
- 원인 사례:
  - Qt가 `parking/v1/commands/fire/{channel_id}`가 아닌 토픽으로 ACK를 보냄
  - OPEN payload의 `alarm_id`가 아닌 다른 ID를 보냄
  - Qt가 `ACKNOWLEDGED`를 화재 해제로 오해하거나 반대로 OPEN처럼 계속 표시함
- 정책: Check는 해제가 아니다. `active=true`를 유지하면서 깜빡임·경고음만 멈추고,
  센서 `FIRE_CLEARED`에서만 화면의 화재 상태를 제거한다.

확인:

```bash
mosquitto_sub -h localhost -v -t 'parking/fire/ch01'
mosquitto_pub -h localhost -q 1 \
  -t 'parking/v1/commands/fire/ch01' \
  -m '{"command":"ALARM_ACK","channel_id":"ch01","alarm_id":"OPEN에서 받은 alarm_id"}'
```

정상 결과는 `event_type=FIRE_ACKNOWLEDGED`, `alarm_state=ACKNOWLEDGED`,
`ack_state=acknowledged`, `active=true`다. 서버 로그의 `active alarm not found`는
채널 또는 alarm ID가 현재 OPEN 상태와 일치하지 않는다는 뜻이다.

가짜 UART를 일반 FIFO로 만들면 `tcgetattr failed: Inappropriate ioctl for device`가
발생한다. 운영 `UartDriver`는 termios 장치를 요구하므로
`tools/fake_fire_sensor.sh --create-pty <server_device> <writer_device>`로 socat PTY pair를
만들어야 한다. 서버는 `server_device`를 열고 테스트 프레임은 `writer_device`로 보낸다.

## 5. 홀센서와 점유 처리

### TS-004 MQTT 가짜 OCCUPIED 신호가 서버에서 처리되지 않음

- 상태: 설정 확인 필요
- 증상: `parking/sensor/hall`에 메시지를 발행해도 DB 세션과 타이머가 시작되지 않는다.
- 원인 사례: `.env.public`의 `HALL_MQTT_INPUT_ENABLED=false`.

확인:

```bash
grep '^HALL_MQTT_INPUT_ENABLED' .env.public 2>/dev/null
```

실제 서버 흐름을 시험하려면 서버 시작 전에 다음을 적용한다.

```bash
export HALL_MQTT_INPUT_ENABLED=true
export HALL_MQTT_TOPIC=parking/sensor/hall
```

입력:

```bash
mosquitto_pub -h localhost -q 1 -t parking/sensor/hall \
  -m 'SENSOR:HALL01:OCCUPIED:1'
```

Qt 화면만 확인하려고 `parking/v1/state/EV01`에 직접 발행하는 것은 UI mock이며, 실제
홀센서 파싱·DB·촬영·타이머 검증으로 간주하지 않는다.

### TS-005 `unsupported sensor message type`

- 상태: 입력 규약으로 해결
- 증상: `SENSOR_MESSAGE_INVALID` 또는 `Hall sensor message rejected`.
- 원인: STM32가 `1`, 임의 문자열 또는 지원하지 않는 prefix를 보냈다.
- 정상 line 형식:

```text
SENSOR:HALL01:OCCUPIED:1
SENSOR:HALL01:VACANT:2
FIRE:FLAME01:DETECTED:3
FIRE:FLAME01:CLEARED:4
```

각 메시지는 개행으로 끝나야 한다. sequence는 같은 센서에서 증가해야 한다.

### TS-006 `sensor id is not mapped to an enabled slot`

- 상태: 설정 매핑으로 해결
- 원인: STM32의 `HALL01`과 `config/parking_slots.json`의 `sensor_id`가 다르거나 슬롯이
  비활성화돼 있다.

확인:

```bash
grep -n 'sensor_id\|slot_id\|enabled' config/parking_slots.json
```

`HALL01 → EV01`처럼 실제 입력 ID와 설정을 정확히 일치시킨다. 알 수 없는 센서를 EV01로
임의 fallback하면 오주차 세션이 생기므로 금지한다.

### TS-007 OCCUPIED/VACANT가 1초마다 반복됨

- 상태: 중복 방지 구현됨, STM32 송신 정책 개선 권장
- 원인: STM32가 상태 변경 시점이 아니라 현재 상태를 주기적으로 전송한다.
- Pi 동작: 이미 점유/공석이면 `slot is already occupied/vacant`로 무시한다.
- 권장: STM32에서 디바운싱 후 상태가 바뀔 때만 전송하고 heartbeat는 별도 메시지로 둔다.

### TS-008 `UNIQUE constraint failed: PARKING_SESSION.slot_id`

- 상태: 복구 로직 존재, BestShot과 홀센서 동시 경로 추가 점검 필요
- 원인: 같은 슬롯에 `ACTIVE` 또는 `VIOLATION` 세션이 이미 있는데 새 세션을 만들었다.
- 주요 사례:
  - 이전 서버가 비정상 종료돼 ACTIVE 세션이 남음
  - 홀센서 세션과 BestShot 세션이 같은 슬롯에 동시에 생성됨
  - 메모리 상태와 DB 상태가 불일치

확인:

```bash
sqlite3 -header -column data/db/parking.db \
  "SELECT session_id,slot_id,status,entry_time,violation_at,exit_time FROM PARKING_SESSION WHERE slot_id='EV01' ORDER BY session_id DESC;"
```

운영 정책 없이 DB 행을 바로 삭제하지 않는다. 시작 시 stale session 복구와 동일 세션 ID
연결을 먼저 확인한다.

## 6. SQLite와 위반 상태

### TS-009 위반 차량이 출차 후 `ENDED`로 보임

- 상태: 현재 정책
- 의미:
  - 점유 중 위반 발생 시 `status=VIOLATION`, `violation_at` 기록
  - 출차 완료 시 현재 상태는 `ENDED`
  - 과거 위반 여부는 `violation_at IS NOT NULL`과 위반 EVENT_LOG로 조회
- 주의: 위반 증거 이미지는 조기 정상 출차 이미지처럼 삭제하면 안 된다.

조회:

```bash
sqlite3 -header -column data/db/parking.db \
  "SELECT session_id,slot_id,status,violation_at,exit_time FROM PARKING_SESSION ORDER BY session_id DESC LIMIT 10;"
```

Qt가 과거 위반 이력을 조회할 때 `status='VIOLATION'`만 사용하면 종료된 위반을 놓친다.
`violation_at` 또는 위반 이벤트를 함께 사용한다.

### TS-010 SQLite 파일을 열었더니 `SQLite format 3`과 깨진 문자가 보임

- 상태: 정상 동작
- 원인: `.db`는 텍스트 파일이 아닌 SQLite 바이너리다.
- 해결: 편집기로 열지 말고 `sqlite3` CLI 또는 SQLite 전용 뷰어를 사용한다.

```bash
sqlite3 data/db/parking.db '.tables'
sqlite3 -header -column data/db/parking.db 'SELECT * FROM VEHICLE;'
```

## 7. 카메라·이미지·OCR

### TS-011 BestShot이 저장되지 않음

- 상태: 현재 운영 기본은 `BESTSHOT_ENABLED=false`이므로 저장되지 않는 것이 정상
- 확인 항목:
  - BestShot을 의도했다면 `BESTSHOT_ENABLED=true`인가.
  - RTSP metadata track에서 vehicle/plate ImageRef가 실제 발생했는가.
  - 카메라 HTTPS Digest 다운로드가 성공했는가.
  - vehicle과 plate의 object/channel 상관관계가 맞는가.
  - 같은 슬롯의 활성 세션 unique constraint가 발생하지 않았는가.
- MQTT 연결만으로 BestShot JPEG가 자동 생성되는 것은 아니다. 카메라가 해당 BestShot
  metadata와 다운로드 URL을 제공해야 한다.

### TS-012 `Gemini OCR unreadable`

- 상태: 입력별 재시도 및 UNKNOWN 처리
- 원인 후보: 번호판이 작음, 흔들림, 역광, crop 실패, API quota/key 문제.
- 정책: OCR 실패를 `NON_EV`로 단정하지 않고 `UNKNOWN`과 `PLATE_OCR_UNRESOLVED`로 기록한다.
- 비밀값 확인 시 API Key 자체를 출력하지 않는다.

### TS-013 MJPEG/H.264 decoder 경고

관찰된 예:

```text
Picture size 0x1520 is invalid
mmco: unref short failure
illegal short term buffer state detected
```

- 일시적 경고 뒤 frame count가 계속 증가하면 스트림 복구 여부를 함께 본다.
- 지속적으로 최신 frame이 끊기면 RTSP profile, 해상도, codec, TCP transport 및 카메라
  상태를 확인한다.

```bash
grep -E 'RTSP opened|frame_count|read_failures|Picture size|mmco' data/logs/pi-server.log | tail -n 100
```

## 8. 빌드와 테스트

### TS-014 CMake가 이동된 소스 파일을 찾지 못함

관찰된 예:

```text
Cannot find source file: src/timer/EventDatabase.cpp
No SOURCES given to target: parking-timer-core
```

- 원인: 소스 이동 후 `CMakeLists.txt` 경로가 갱신되지 않았다.
- 해결: 실제 통합 위치를 CMake target에 반영하고 깨끗한 configure부터 검증한다.

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j"$(nproc)"
ctest --test-dir cmake-build --output-on-failure
```

### TS-015 CTest 디렉터리가 없음

관찰된 예:

```text
Failed to change working directory to .../cmake-build
```

먼저 configure와 build를 실행한다. 프로젝트에서 실제 사용 중인 build 디렉터리 이름을
혼용하지 않는다.

### TS-016 GCC 14 `LoRaDriver::encode` 경고

`std::vector<uint8_t>` 확장 과정에서 `-Wfree-nonheap-object`가 출력된 사례가 있었다.
경고만으로 성공 빌드를 실패로 판단하지 않는다. 다만 sanitizer 또는 단위 테스트에서 실제
메모리 오류가 나오면 별도 결함으로 처리한다.

## 9. Git 작업 오류

### TS-017 push `non-fast-forward`

- 원인: 같은 원격 브랜치에 로컬에 없는 커밋이 존재한다.
- 원격을 먼저 확인하고 merge/rebase 정책을 정한다.

```bash
git fetch origin --prune
git status -sb
git log --oneline --left-right HEAD...@{upstream}
```

원인을 확인하지 않고 `--force`를 사용하지 않는다. 꼭 필요하면 원격 SHA를 확인한 뒤
`--force-with-lease`를 사용한다.

### TS-018 긴 브랜치 push 명령의 줄바꿈으로 `invalid refspec`

- 원인: 브랜치명 중간에 실제 개행이 들어갔다.
- 해결: 현재 브랜치를 그대로 push한다.

```bash
git push -u origin HEAD
```

### TS-019 한글 브랜치명 `Hidden character warning`

- 원인: GitHub가 비ASCII 문자를 보안상 경고한다.
- 기능 오류는 아니지만 영문 브랜치 규칙을 사용한다.

```text
fix/server-qt-fire-alarm-mqtt-protocol-conflict_EVDA-159
```

로컬 브랜치만 바꾸면 upstream이 예전 이름에 남을 수 있다. 다음으로 확인한다.

```bash
git status -sb
git branch -vv
git push -u origin HEAD
```

### TS-020 PR conflict: 로컬 develop과 원격 develop 이력 분기

- 원인 사례: 동일 기능이 로컬 일반 커밋과 GitHub squash merge 커밋으로 각각 존재했다.
- 해결 원칙:
  1. `git fetch origin --prune`
  2. `git merge-base`, `git diff`, `git merge-tree`로 내용과 이력을 분리해 확인
  3. 기능이 더 많은 쪽을 확인한 뒤 충돌 해결
  4. 빌드·테스트 후 병합 커밋
- `git reset --hard`, `git clean`, 무검증 강제 push는 사용하지 않는다.

## 10. 현재 알려진 제한

- Qt 저장소는 이 저장소 밖에 있어 Pi에서 Qt UI 내부 상태를 직접 확인할 수 없다.
- 화재 UART→MQTT는 구현됐지만 화재 영상 교차검증, 관제 ACK, 최종 확정은 미구현이다.
- 실제 센서가 하나인 상태에서 `FLAME01~04 → ch01~04`는 시연용 가짜 입력 매핑이다.
- MQTT는 기본 1883 평문이며 인증/TLS 운영 정책이 남아 있다.
- MQTT/RTSP 오류는 아직 모든 경로가 `SystemEventReporter`에 연결된 것은 아니다.
- 실제 STM32/LoRa와 32면 전체 구성은 추가 실기기 검증이 필요하다.

## 11. 관련 문서

- `docs/SENSOR_COMMUNICATION_ERROR_HANDLING.md`: 오류 queue, 중복 억제, EVENT_LOG
- `docs/UART_LORA_PROTOCOL.md`: UART/LoRa wire protocol
- `docs/MQTT_PROTOCOL_PROPOSAL.md`: Pi–Qt MQTT 계약
- `docs/HTTP_API.md`: Qt HTTP/HTTPS 조회 계약
- `docs/CAMERA_SNAPSHOT_API_INTEGRATION.md`: 카메라 내부 개선 이미지 연동
- `docs/TEST_SCENARIOS.md`: 자동·실기기 검증 시나리오

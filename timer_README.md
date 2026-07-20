# 주차구역 EV 점유 타이머 프로토타입

Raspberry Pi 4 서버에서 동작할 C++ 프로토타입이다. 홀센서 입·출차 신호와 카메라 번호판 인식 결과가 들어왔다고 가정하고, 차량 마스터를 조회한 뒤 EV/PHEV의 점유 제한 시간을 관리한다. 입차 시 `timer_log` 한 행을 INSERT하고, 제한시간 만료 또는 출차 시 같은 행을 UPDATE한다.

기본 데모 제한시간은 **60초**다. 실제 급속 충전구역 값은 **3600초(1시간)**로 바꾸면 된다. 각 차량마다 스레드를 만들지 않고, 하나의 worker와 `std::priority_queue` 최소 힙, `std::condition_variable::wait_until()`을 사용한다.

## 참고 자료와 적용 범위

상위 폴더의 다음 자료를 기준으로 구현했다.

- `주차구역_타이머_로직_명세.pdf`: OFF→ON 입차, ON→OFF 출차, `steady_clock`, 최소 우선순위 큐, lazy deletion
- `VEDA_5조_최종프로젝트_기획서.pdf`: EV 구역 상태 머신, 데모 1~3분/운영 60분, Raspberry Pi 서버 역할
- `is_ev_table_schema.png`: 차량 마스터의 `car_number`, `vehicle_type`
- `log_table_schema.png`: `timer_log`의 상태·시각·증거 이미지 필드
- `전체_아키텍처_참고.html`과 장치별 아키텍처 이미지: Pi의 `TimerManager`, `ParkingSlotManager`, `EVZoneManager`, `EventManager`, `EventDatabase` 책임

이번 범위는 기획서의 Raspberry Pi Business/Core 계층이다. 실제 STM32/UART, RTSP/OCR, MQTT/HTTP/WebSocket, Qt UI, LED/Buzzer는 연결하지 않고 CLI와 JSON 콘솔 출력으로 경계를 시뮬레이션한다.

```text
Hall Sensor OFF→ON       Camera/OCR plate result
          │                         │
          └──── CLI entry 명령 ─────┘
                         │
                         ▼
             mock_vehicle_master 조회
                 │          │       │
              EV/PHEV    NON_EV   UNKNOWN
                 │          │       │
                 │       즉시 알림  관리자 확인
                 ▼
            timer_log INSERT(PARKED)
                 │
                 ▼
       TimerManager min-priority-queue
            │                     │
       deadline 도달          OFF→ON의 반대인
            │                 ON→OFF 출차
            ▼                     ▼
 UPDATE(VIOLATION)       UPDATE(DEPARTS, canceled=1)
            │                     │
            └──── JSON 이벤트 ────┘ → 향후 Qt/MQTT 어댑터
```

## 디렉터리 구성

```text
timer_
├── CMakeLists.txt
├── Makefile
├── requirements.txt
├── config/timer.conf
├── db/
│   ├── schema.sql
│   └── seed.sql
├── include/parking_timer/
│   ├── EventDatabase.hpp
│   ├── EventManager.hpp
│   ├── ParkingSlotManager.hpp
│   ├── RuntimeConfig.hpp
│   ├── TimerManager.hpp
│   └── Types.hpp
├── src/
│   ├── EventDatabase.cpp
│   ├── EventManager.cpp
│   ├── ParkingSlotManager.cpp
│   ├── RuntimeConfig.cpp
│   ├── TimerManager.cpp
│   ├── Types.cpp
│   └── main.cpp
└── tests/timer_tests.cpp
```

## 빌드 요구사항

- CMake 3.16 이상
- C++20을 지원하는 GCC/Clang
- GNU Make 또는 CMake가 지원하는 다른 빌드 도구
- SQLite 3.8 이상 런타임과 개발 헤더(partial index 사용)
- pthreads(일반적인 Linux toolchain에 포함)

Debian/Ubuntu/Raspberry Pi OS에서는 보통 다음 패키지로 준비할 수 있다.

```bash
sudo apt update
sudo apt install build-essential cmake libsqlite3-dev sqlite3
```

이 개발 환경에는 필요한 SQLite3가 이미 있어 새 라이브러리를 설치하지 않았다. Python이나 pip 의존성도 없으며, 그 사실을 `requirements.txt`에 기록했다.

현재 검증 환경은 Raspberry Pi 4 계열 ARM64 Linux다. 저장소의 기존 실행 파일을 다른 Pi OS에 그대로 복사하기보다 **대상 Raspberry Pi에서 위 명령으로 다시 빌드**하는 것을 권장한다. 32비트 armhf는 별도 검증하지 않았다.

## 빌드와 테스트

프로젝트 루트에서 CMake가 Makefile을 생성한 뒤 `make`로 빌드하려면 다음과 같이 실행한다.

```bash
cd timer_
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j"$(nproc)"
```

전통적인 out-of-source 방식도 동일하다.

```bash
cd timer_
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
```

실행 파일은 `timer_/build/parking_timer`에 생성된다. 제공한 상위 `Makefile`을 쓰면 더 짧게 실행할 수 있다.

```bash
cd timer_
make          # configure + build
make test     # build + CTest
make demo     # 2초짜리 빠른 자동 데모
make run      # 기본 60초 설정으로 대화형 실행
make docs     # docs/api/html/index.html에 Doxygen API 명세 생성
make clean
```

`make clean`은 `build/` 전체를 제거하므로 그 안에 둔 SQLite DB도 함께 사라진다. 운영 데이터는 예를 들어 `/var/lib/parking-timer/events.sqlite3`처럼 빌드 디렉터리 밖의 쓰기 가능한 경로에 두어야 한다.

### API 명세 문서

`Doxyfile`은 `include`, `src`, `tests`의 C++ API와 내부 타이머 로직을 추출한다. HTML 문서는 다음 명령으로 생성한다.

```bash
make docs
# 또는 doxygen Doxyfile
```

생성된 시작 페이지는 `docs/api/html/index.html`이다. Doxygen은 Debian/Ubuntu에서 `sudo apt install doxygen`으로 설치한다.

자동 테스트는 다음 항목을 확인한다.

- EV/PHEV/NON_EV/UNKNOWN 차량 분류
- EV 입차의 `PARKED` INSERT
- 같은 구역의 중복 입차 거부
- 만료 시 `VIOLATION`, `violation_at`, `image_path_2` UPDATE
- 위반 후 출차 시 `DEPARTS`로 바뀌면서 기존 `violation_at` 보존
- 만료 전 출차 시 `is_canceled=1` 및 lazy deletion
- 더 늦은 타이머를 기다리는 도중 새로 들어온 빠른 deadline이 worker를 깨우는지 검증
- 이벤트 publisher callback 예외가 worker/process를 종료시키지 않는지 검증

직접 테스트하려면 다음을 사용한다.

```bash
ctest --test-dir build --output-on-failure
```

### 현재 구현 검증 결과

2026-07-14 ARM64 Linux 환경에서 다음을 확인했다.

- Release `cmake + make` clean build 성공(활성화한 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` 경고 없음)
- CTest 1/1 통과
- UndefinedBehaviorSanitizer 빌드/CTest 통과
- 2초 smoke demo의 EV 만료 및 PHEV 조기 출차 상태 전이 정상
- 실제 60초 demo에서 `parked_at`부터 `violation_at`까지 SQLite 계산값 **60.008초**
- demo DB `PRAGMA integrity_check` 결과 `ok`, foreign-key 위반 0건

## 실행 방법

### 자동 시뮬레이션

1분 요구사항을 그대로 확인하려면 다음 명령을 실행한다. 약 60초가 걸린다.

```bash
./build/parking_timer \
  --db ./build/demo-60s.sqlite3 \
  --timeout-seconds 60 \
  --reset-logs \
  --demo
```

빠른 확인은 `make demo`를 사용한다. 이 대상만 실행 시간을 줄이기 위해 2초를 전달한다. 자동 데모의 순서는 다음과 같다.

1. zone 1에 EV `123가4567` 입차 → `PARKED` INSERT 및 타이머 시작
2. zone 2에 PHEV `234나5678` 입차 → `PARKED` INSERT 및 타이머 시작
3. 비EV `345다6789` 조회 → `NON_EV_ALERT`, 장기점유 큐에는 넣지 않음
4. PHEV가 만료 전 출차 → 같은 행을 `DEPARTS`로 UPDATE하고 큐 노드는 취소 표시
5. EV 제한시간 만료 → `VIOLATION_TRIGGERED`, `violation_at` UPDATE
6. 위반 EV 출차 → 같은 행을 `DEPARTS`로 UPDATE하되 위반 시각 보존

### 대화형 시뮬레이션

```bash
./build/parking_timer --db ./build/events.sqlite3 --timeout-seconds 60
```

CLI 명령은 홀센서와 번호판 인식 결과를 대신한다.

```text
entry 1 123가4567
entry 2 234나5678 snapshots/phev_first.jpg
logs
wait 10
exit 2
wait 50
logs
exit 1
quit
```

지원 명령:

| 명령 | 의미 |
|---|---|
| `entry <zone> <car> [image]` | 홀센서 OFF→ON + 번호판 인식 결과 입력 |
| `exit <zone>` | 홀센서 ON→OFF 입력, 활성 로그 UPDATE |
| `wait <seconds>` | 프로세스를 유지해 worker 만료를 관찰 |
| `logs` | 현재 `timer_log` 출력 |
| `vehicles` | 테스트 차량 마스터 출력 |
| `status` | 취소 대기 노드를 포함한 큐 크기 출력 |
| `help`, `quit` | 도움말, 종료 |

## 설정 우선순위

낮은 우선순위부터 다음 순서로 덮어쓴다.

1. `config/timer.conf`
2. 환경변수 `PARKING_TIMEOUT_SECONDS`, `DATABASE_PATH`
3. CLI `--timeout-seconds`, `--db`

기본 데모 설정:

```ini
PARKING_TIMEOUT_SECONDS=60
DATABASE_PATH=events.sqlite3
```

운영 급속 충전구역 1시간 설정 예:

```bash
PARKING_TIMEOUT_SECONDS=3600 ./build/parking_timer
```

실행 옵션 전체는 `./build/parking_timer --help`로 확인한다. `--sql-dir`로 `schema.sql`과 `seed.sql`이 있는 별도 디렉터리도 지정할 수 있다. CMake 빌드 시 기본 SQL/config 파일은 `build/db`, `build/config`에도 복사된다.

실행 파일만 단독 복사하면 스키마와 설정을 찾을 수 없다. 배포할 때는 실행 파일 옆에 `db/schema.sql`, `db/seed.sql`, `config/timer.conf`를 같은 구조로 함께 두거나 `--sql-dir`과 `--config`에 절대 경로를 지정한다. 상대 `DATABASE_PATH`는 실행 파일 위치가 아니라 현재 작업 디렉터리 기준이며, WAL 모드의 `-wal`/`-shm` 파일을 만들 수 있도록 해당 디렉터리에 쓰기 권한이 있어야 한다.

## SQLite 스키마

### `mock_vehicle_master`

사용자가 부른 “is_ev 테이블”의 이미지 안 실제 권장 테이블명은 `mock_vehicle_master`다. 이미지 설명도 Boolean `is_electric`보다 PHEV 예외를 표현할 수 있는 `vehicle_type`을 권장하므로 이를 따랐다.

| 컬럼 | SQLite 타입 | 제약/의미 |
|---|---|---|
| `car_number` | `TEXT` | PK, NOT NULL, 차량번호 |
| `vehicle_type` | `TEXT` | `EV`, `PHEV`, `GASOLINE`, `DIESEL` |

임의 테스트 seed:

| 차량번호 | 종류 | 타이머 분류 |
|---|---|---|
| `123가4567` | EV | 타이머 시작 |
| `234나5678` | PHEV | 급속구역에서는 타이머 시작 |
| `345다6789` | GASOLINE | 즉시 `NON_EV_ALERT`, 큐 제외 |
| `456라7890` | DIESEL | 즉시 `NON_EV_ALERT`, 큐 제외 |

### `timer_log`

| 컬럼 | SQLite 타입 | 의미 |
|---|---|---|
| `id` | `INTEGER` | 자동 증가 PK, 큐 노드가 참조하는 불변 세션 ID |
| `car_number` | `TEXT` | 차량 마스터 FK |
| `zone_id` | `INTEGER` | 충전구역 ID, 양수 |
| `status` | `TEXT` | `PARKED`, `VIOLATION`, `DEPARTS` |
| `parked_at` | `TEXT` | 최초 주차 판정 UTC 시각 |
| `violation_at` | `TEXT NULL` | 제한시간 만료 UTC 시각 |
| `departed_at` | `TEXT NULL` | 출차 UTC 시각 |
| `image_path_1` | `TEXT NULL` | 최초 주차 증거 이미지 경로 |
| `image_path_2` | `TEXT NULL` | 만료 시점 증거 이미지 경로 |
| `is_canceled` | `INTEGER` | 0/1, lazy deletion용 확장 필드 |

SQLite에는 별도 DATETIME 저장 클래스가 없으므로 시각은 정렬 가능한 ISO-8601 UTC 문자열(`2026-07-14T05:54:42.983Z`)로 저장한다. `is_canceled`는 스키마 이미지에는 없지만 타이머 명세의 lazy deletion 절에 직접 요구되어 추가했다. `departed_at IS NULL`인 행에 대해 구역별 unique partial index를 두어 한 구역의 활성 세션을 하나로 제한한다.

DB를 직접 확인하는 예:

```bash
sqlite3 build/demo.sqlite3 \
  "SELECT id, car_number, zone_id, status, parked_at, violation_at, departed_at, is_canceled FROM timer_log ORDER BY id;"
```

## 상태 전이와 SQL 의미

```text
EMPTY
  └─ OFF→ON + EV/PHEV 확인
       └─ PARKED (INSERT)
            ├─ deadline 도달 ─→ VIOLATION (UPDATE)
            │                       └─ ON→OFF ─→ DEPARTS (UPDATE, violation_at 보존)
            └─ deadline 전 ON→OFF ────────────→ DEPARTS (UPDATE, canceled=1)
```

입차 시 새 주차 세션 한 행만 만든다. 이후 만료와 출차는 INSERT-INSERT가 아니라 사진 명세가 권장한 INSERT-UPDATE 방식으로 같은 `id`를 변경한다. 출차 후 재입차하면 새로운 세션/새 행이 생성된다.

비EV는 기획서대로 즉시 경고하지만, 이번 `timer_log`는 EV/PHEV 장기점유 세션 테이블이므로 행과 1시간 큐를 만들지 않는다. 미등록 차량은 `UNKNOWN_VEHICLE`로 출력하고 관리자 확인 대상으로 둔다.

## 타이머 알고리즘

`TimerManager`는 다음 순서로 동작한다.

1. 입차 시 `deadline = steady_clock::now() + parking_timeout`을 계산한다.
2. `{deadline, sequence, timer_log.id, zone_id, car_number}`를 최소 우선순위 큐에 넣는다.
3. 단일 worker가 가장 이른 항목에 대해 `condition_variable::wait_until(deadline)`으로 대기한다. 큐가 비었을 때 CPU polling을 하지 않는다.
4. 더 빠른 deadline이 새로 들어오면 `notify_one()`으로 기존 대기를 깨워 top을 다시 계산한다.
5. deadline에 도달하면 `id`가 아직 `PARKED`, `departed_at IS NULL`, `is_canceled=0`인지 조건부 UPDATE로 재검증한다.
6. 변경 행이 정확히 1개일 때만 `VIOLATION_TRIGGERED` 이벤트를 출력한다.
7. 출차는 큐 중간 원소를 탐색·삭제하지 않는다. DB를 `DEPARTS/is_canceled=1`로 만들고, 해당 노드가 만료되어 top에 왔을 때 조건부 UPDATE 0행으로 확인해 버린다.

worker의 DB 처리나 향후 이벤트 publisher callback이 예외를 던져도 스레드 밖으로 전파되어 프로세스가 `std::terminate`되지 않도록 최종 예외 경계를 둔다. 만료 DB UPDATE가 busy/I/O 오류로 실패하면 노드를 버리지 않고 1·2·4·8·16·32초(이후 32초) backoff로 다시 큐에 넣는다. 이때 `violation_at`은 DB 복구 시각이 아니라 최초 steady deadline 도달 때 캡처한 UTC 시각을 계속 사용한다. 또한 DB INSERT 직후 큐 등록이 실패하면 활성 `PARKED` 고아 행이 남지 않도록 `DEPARTS/is_canceled=1` 보상 UPDATE를 시도한다.

판정용 경과시간에 `system_clock`을 사용하지 않으므로 NTP 보정이나 운영체제 wall-clock 변경이 실행 중 타이머 길이를 바꾸지 않는다. DB와 관제 화면에 보여줄 시각만 `system_clock` 기반 UTC로 기록한다.

### 만료와 출차가 동시에 발생할 때

PDF에는 동일 순간의 우선순위가 정의되어 있지 않다. 이 구현은 SQLite 작업을 프로세스 내부 mutex로 직렬화하며 **먼저 DB 상태를 바꾼 이벤트가 승리**한다.

- 출차 UPDATE가 먼저면 `is_canceled=1`이 되어 만료 UPDATE는 0행이고 위반 이벤트가 없다.
- 만료 UPDATE가 먼저면 `violation_at`이 기록되고, 바로 뒤 출차가 `DEPARTS`로 바꾸되 그 위반 시각을 보존한다.

큐 키를 `zone_id`만 쓰지 않고 불변 `timer_log.id`를 사용하므로, 출차 후 같은 구역에 새 차량이 재입차해도 과거 lazy 노드가 새 세션을 위반 처리하지 않는다.

## Qt/STM32/카메라 통합 지점

콘솔의 한 줄 JSON은 향후 Qt 측 HTTP/WebSocket/MQTT 이벤트의 mock이다. 예:

```json
{"event_type":"VIOLATION_TRIGGERED","zone_id":1,"car_number":"123가4567","occurred_at":"2026-07-14T05:54:42.983Z","detail":"snapshots/123가4567_1_violation.jpg"}
```

- `entry`: 실제 통합 시 `SensorLinkManager`의 Hall OFF→ON 이벤트와 `PlateRecognitionManager`의 번호판 결과가 합쳐져 호출된다.
- `exit`: Hall ON→OFF 이벤트가 호출한다.
- `EventManager`: 현재 stdout JSON 대신 MQTT/HTTP/WebSocket publish 구현을 주입할 자리다.
- `image_path_1/2`: 카메라 서비스가 넘길 스토리지 경로 자리이며, 이 프로토타입은 실제 이미지 파일을 생성하지 않는다.
- `EventDatabase`: Pi 로컬 SQLite 저장소다.

## 의도적으로 제외한 Stage 2와 제약사항

- 완속 EV 14시간/PHEV 7시간 및 00:00~06:00 누적 제외
- 최근 30분/2시간 슬라이딩 윈도우를 이용한 짧은 출차·재입차(kiting) 누적 판정
- 일반차 5분 정차 확인과 두 차례 실제 촬영
- 프로세스 재시작 후 활성 `PARKED` 타이머 큐 복구
- 실제 카메라 캡처/OCR, STM32 UART/LoRa, 센서 디바운싱
- Qt 알림 ACK, 네트워크 재전송, 실제 MQTT/HTTP/WebSocket
- stdin이 없는 systemd 서비스용 상시 이벤트 루프(현재 대화형 모드는 stdin EOF 시 종료)

특히 `steady_clock` deadline은 프로세스 메모리에만 있으므로 재시작 복구에는 별도 정책이 필요하다. 운영 버전에서는 부팅 시 활성 로그를 읽고 신뢰 가능한 monotonic 기준으로 남은 시간을 재구성하거나, 시작/종료 이벤트를 append-only로 보관하는 설계를 추가해야 한다. 이번 프로토타입은 실행 중 정확한 타이머·DB 상태 전이 검증에 초점을 맞춘다.

DB 위반 UPDATE 자체는 위 backoff로 재시도하지만, 외부 Qt/MQTT publisher가 실패한 뒤 전달을 보장하는 transactional outbox는 아직 없다. 실제 관제 통합에서는 이벤트 테이블과 ACK/retry worker를 추가해야 한다.

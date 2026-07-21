# Parking Timer

주차 장기 점유 타이머는 메인 Raspberry Pi 서버와 동일한
`database::EventDatabase` 및 `db/schema.sql`을 사용한다. 별도의 `timer_log`나
`mock_vehicle_master` 테이블은 만들지 않는다.

## DB 매핑

- 차량 분류: `VEHICLE.is_ev`, `VEHICLE.is_phev`
- 주차면: `PARKING_SLOT.slot_id`
- 입차·위반·출차: `PARKING_SESSION`
- 최초/위반 이미지: `IMAGE_LOG`
- 타이머 입차 이미지 종류: `TIMER_ENTRY`
- 타이머 위반 이미지 종류: `TIMER_VIOLATION`

일반 차량은 `is_ev=0`, `is_phev=0`으로만 표현하며 연료 종류를 추가로 구분하지
않는다. 타이머와 DB는 `EV01` 형식의 문자열 `slot_id`를 그대로 사용한다.

세션 상태 매핑은 다음과 같다.

| 타이머 표시 | DB 상태 |
|---|---|
| `PARKED` | `ACTIVE` |
| `VIOLATION` | `VIOLATION` |
| `DEPARTS` | `ENDED` |

## 빌드와 테스트

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

타이머 실행 파일:

```bash
./build/parking-timer --help
```

`--reset-logs`는 `IMAGE_LOG.enhancement_type='TIMER_ENTRY'`로 식별되는 타이머
세션만 삭제하며 카메라 BestShot 세션은 삭제하지 않는다.

# Test Scenarios

기준일: 2026-07-26

## 자동 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build -j2
ctest --test-dir cmake-build --output-on-failure
```

현재 CTest 항목:

1. `parking-domain-test`: 슬롯 구성, 상태 전이, 중복/역순 이벤트
2. `sensor-parking-pipeline-test`: partial/multi-line 센서 parser와 adapter
3. `capture-scheduler-test`: SQLite 세션 ID, T0+30/60초, 중복, 출차 취소, 발행 재시도
4. `parking-occupancy-confirmation-gate-test`: 자동 확정, flap 취소, 단조시계 경과시간
5. `parking-timer-tests`: EV/PHEV 분류, 중복 입차, 만료, 출차, 복구 및 오류 처리
6. `hall-timer-integration-test`: 단일 OCCUPIED 자동 확정 → 실제 JPEG → DB/OCR 대역 → 타이머 → 위반 증거 → 조기 정리
7. `http-api-test`: 슬롯·세션·이미지 API와 data root 경로 보안
8. `uart-lora-driver-test`: PTY UART 양방향, partial line, LoRa partial/multi frame,
   CRC 오류 재동기화, SensorLink callback 및 alert 송신
9. `system-event-reporter-test`: 비동기 queue, 중복 억제, 저장 재시도와 예외 격리

`hall-timer-integration-test`는 카메라 대신 메모리의 OpenCV frame을 사용하지만 실제
`SnapshotStorage`와 SQLite를 사용한다. OCCUPIED 중복 방지, VACANT 시 OCR 취소,
추가 OCCUPIED 없이 유예시간 후 자동 확정, SQLite 정수 세션 ID의 scheduler 전달,
위반 전 파일/IMAGE_LOG 삭제, 위반 후 증거 보존까지 검증한다.

## 실기기 수동 검증

1. 카메라 RTSP 연결 후 초기 frame 로그를 확인한다.
2. `parking/v1/events/+`, `parking/v1/state/+`를 구독한다.
3. `SENSOR:HALL01:OCCUPIED:1`을 `parking/sensor/hall`에 QoS 1로 발행한다.
4. EV01의 실제 JPEG, ACTIVE 세션, `SLOT_OCCUPIED`를 확인한다.
5. 테스트 환경에서만 `PARKING_TIMEOUT_SECONDS`를 짧게 설정해 실제 위반 Snapshot과
   `OVERTIME_VIOLATION`을 확인한다.
6. HTTP의 `session_images_url`과 이미지 URL이 200으로 응답하는지 확인한다.
7. 제한시간 전 VACANT와 제한시간 후 VACANT의 보존 정책 차이를 확인한다.

## `/dev/parking_alert` 실기기 테스트

`docs/PARKING_ALERT_DRIVER.md`의 순서로 모듈을 적재하고 다음을 확인한다.

1. `/dev/parking_alert`가 character device로 생성된다.
2. `status`는 API version, mask, generation을 반환한다.
3. `set/clear/clear-all` ioctl이 해당 bit만 변경한다.
4. 동일 `set` 반복 시 generation이 증가하지 않는다.
5. `write-set/write-clear`가 ioctl과 같은 상태 전이를 만든다.
6. `watch`의 poll/read가 busy-wait 없이 변경된 상태를 받는다.
7. 0~31 밖 슬롯과 잘못된 API version은 거부된다.

운영 기본 제한시간은 3600초다. 실제 DB인 `data/db/parking.db`를 파괴적 테스트에
사용하지 말고 `EVENT_DB_PATH`로 임시 DB를 지정한다. 실제 Gemini 테스트는 API Key와
비용이 필요하므로 기본 CTest와 분리한다.

## 남은 테스트

- 실제 STM32 UART partial/multi-line 입력 및 재연결
- 실제 LoRa 모뎀 CRC frame, 무선 단절 및 재연결
- 4채널 × EV 슬롯 실제 ROI 검증
- Qt retained 상태 복구와 HTTP 이미지 표시
- MQTT broker 단절/재연결 중 이벤트 유실 정책
- Qt 명령 및 화재 상태 머신(구현 후)

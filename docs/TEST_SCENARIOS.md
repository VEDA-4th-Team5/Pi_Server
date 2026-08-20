# Test Scenarios

기준일: 2026-08-20

## 자동 테스트

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build -j2
ctest --test-dir cmake-build --output-on-failure
```

현재 CTest에는 소프트웨어 자동 테스트 30개와 기본 실행 시 Skip되는 실기기 HIL 테스트
1개가 등록되어 있다. 대표 항목은 다음과 같다.

1. `gemini-ocr-client-test`: Gemini HTTP/JSON/MIME/재시도 계약
2. `hall-ocr-policy-test`: 30초 우선 OCR, 60초 fallback, UNKNOWN 정책
3. `plate-illuminator-test`: 야간 LED 점등/해제 정책
4. `hall-capture-coordinator-test`: SQLite 세션 ID와 이미지·OCR·타이머 연결
5. `camera-iva-event-test`: WiseAI 다중 notification, Intrusion/Exit, 토큰·슬롯 매핑
6. `fire-alarm-test`: OPEN → ACKNOWLEDGED → RESOLVED 전이와 중복 억제
7. `parking-domain-test`: 슬롯 구성, 상태 전이, 중복/역순 이벤트
8. `sensor-parking-pipeline-test`: partial/multi-line 센서 parser와 adapter
9. `parking-session-worker-test`: 세션 worker 상태 전이
10. `capture-scheduler-test`: SQLite 세션 ID, T0+30/60초, 중복, 출차 취소
11. `parking-occupancy-confirmation-gate-test`: 자동 확정, flap 취소, 단조시계
12. `uart-lora-driver-test`: PTY UART 양방향, partial line, LoRa partial/multi frame,
   CRC 오류 재동기화, SensorLink callback 및 alert 송신
13. `hardware-test-report-test`: HIL 반복 통계, 평균·P95, CSV escaping, Markdown 생성
14. `sensor-link-hardware-test`: 실제 Hall/Flame UART·LoRa 반자동 HIL, 기본 CTest Skip
15. `system-event-reporter-test`: 비동기 queue, 중복 억제, 저장 재시도와 예외 격리
16. `parking-timer-tests`: EV/PHEV 분류, 만료, 출차, 복구 및 오류 처리
17. `hall-timer-integration-test`: OCCUPIED → JPEG/DB/OCR → 타이머 → 정리/보존
18. `camera-iva-occupancy-integration-test`: IVA 입차/출차, 확인 대기, 세션 연결
19. `evidence-capture-worker-test`: 시작·장기점유 증거, 중복 방지, VACANT 취소,
    파일/DB 실패 시 가짜 행과 고아 파일 방지
20. `hall-capture-pipeline-test`: Snapshot/RTSP 촬영 port → ROI JPEG → IMAGE_LOG/OCR
21. `camera-snapshot-api-client-test`: `/startserver`, discovery, generate, JPEG 검증/재시도
22. `http-api-test`: 슬롯·세션·이미지·설정 API와 data root 경로 보안

`hall-timer-integration-test`는 카메라 대신 메모리의 OpenCV frame을 사용하지만 실제
`SnapshotStorage`와 SQLite를 사용한다. OCCUPIED 중복 방지, VACANT 시 OCR 취소,
추가 OCCUPIED 없이 유예시간 후 자동 확정, SQLite 정수 세션 ID의 scheduler 전달,
위반 전 파일/IMAGE_LOG 삭제, 위반 후 증거 보존까지 검증한다. 동일 슬롯의 대기 센서
이벤트는 최신 상태로 병합되고 서로 다른 슬롯은 bounded capacity를 넘지 않는 것도 확인한다.

`evidence-capture-worker-test`는 조기 출차 취소 직후 pending Job이 제거되어 큐 용량이
회수되는지, 재시작 세션의 원래 T0 기준으로 `OVERSTAY_EVIDENCE`가 한 번만 복원되는지
검증한다. `parking-timer-tests`는 증거 worker와 타이머의 동일 deadline 경합에서 빈 경로를
즉시 확정하지 않고 제한적으로 재시도하는지 확인한다.

## 실기기 수동 검증

실제 STM32 Hall/Flame 센서 링크는 일반 CTest와 분리된 반자동 HIL 실행 파일로
검증한다. 빌드·안전 조건·PASS/FAIL 기준은
[`SENSOR_LINK_HARDWARE_TEST.md`](SENSOR_LINK_HARDWARE_TEST.md)를 참고한다.

```bash
cmake --build cmake-build --target sensor-link-hardware-test -j4
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario all
```

포트폴리오용 반복 측정은 CTest가 아니라 실행 파일을 직접 사용한다. trial별 CSV와
평균·P95를 포함한 Markdown 요약을 함께 생성한다.

```bash
./cmake-build/sensor-link-hardware-test --device /dev/ttyACM0 --mode uart-line --scenario hall --hall-sensors HALL01,HALL02 --repeat 30 --confirm-actions --report-dir data/test-results/hardware
```

일반 CTest에서는 `sensor-link-hardware-test`가 `Skipped`여야 한다. 실기기 라벨은
`RUN_HARDWARE_TESTS=1`을 명시한 경우에만 실행한다.

1. `camera snapshot API ready` 로그와 `/images/generate` 촬영을 확인한다.
2. `parking/v1/events/+`, `parking/v1/state/+`를 구독한다.
3. `SENSOR:HALL01:OCCUPIED:1`을 `parking/sensor/hall`에 QoS 1로 발행한다.
4. EV01의 실제 JPEG, ACTIVE 세션, `SLOT_OCCUPIED`를 확인한다.
5. REST API로 기준시간을 60초까지 낮춰 실제 위반 Snapshot과
   `OVERTIME_VIOLATION`을 확인한다. 위반 판정과 장기점유 증거 촬영은 같은 기준값을
   사용하므로 별도 환경변수를 맞출 필요가 없다.
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
- 일반 주차 알람 ACK와 Qt `STATUS_REQUEST`

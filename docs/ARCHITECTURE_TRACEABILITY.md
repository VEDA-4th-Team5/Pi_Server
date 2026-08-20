# Architecture Traceability

기준일: 2026-08-12

| Interface | 실제 코드 대응 | 상태 |
|---|---|---|
| I-01 Hall Sensor → STM32 | 이 저장소에 STM32 펌웨어와 실제 센서 입력 코드는 없음 | 미구현 |
| I-02 STM32 → Pi UART | `UartDriver`, `device::SensorLinkManager`, `SensorProtocolParser`, `HallParkingService` | 구현: POSIX UART/재연결/화재·홀 공유 라인 분기 |
| I-03 Camera → Pi MQTT | `MqttEventBridge`, `CameraEventParser`, `IvaOccupancyCoordinator` | 구현: WiseAI Intrusion/Exit, 다중 notification 분리, 슬롯 매핑 |
| I-04 Pi ↔ Camera Snapshot/현재 프레임 | `CameraSnapshotApiClient`, `HallCaptureExecutor`, `SnapshotStorage`; 선택적 `RtspStreamReceiver` | 구현: original/enhanced 다운로드, 실행 중 ROI crop, IMAGE_LOG/OCR 연결; RTSP는 fallback |
| I-05 Camera → Qt RTSP 4채널 | Qt 클라이언트 저장소의 책임이며 Pi는 영상 프록시를 하지 않음 | Pi 범위 외 |
| I-06 Pi ↔ Gemini HTTPS OCR | `GeminiOcrClient`, `OcrWorker`, `PlateNormalizer` | 구현 |
| I-07 Pi ↔ Local Vehicle DB | `EventDatabase::classifyVehicle`, `db_get_vehicle_by_plate` | 구현 |
| I-08 Pi ↔ SQLite/File Storage | `EventDatabase`, `db_manager.c`, `SnapshotStorage`, `data/` | 구현 |
| I-09 Pi ↔ Qt MQTT | `EventManager` publisher, `MqttEventBridge::publishQtEvent`, fire ACK subscriber | 구현: 주차 상태·OCR·위반·출차 발행, 화재 ACK 수신 |
| I-10 Qt ↔ Pi HTTPS | `ParkingHttpServer`의 login/Bearer/health/slot/session/image API | 구현: Argon2id 계정, SQLite 세션, TLS fail-closed |
| I-11 Flame Sensor → STM32 | 코드 없음 | 미구현 |
| I-12 STM32 → LoRa 송신 UART | `LoRaDriver` binary framing 규격과 C++ 진단 도구 | 부분 구현: Pi 호환 규격 구현, STM32 송신 코드 미구현 |
| I-13 LoRa 무선 구간 | 투명 UART 모뎀 전제; 무선 칩 설정은 하드웨어 미확정 | 부분 구현: frame/CRC만 구현 |
| I-14 LoRa 수신 → Pi | `UartDriver` → `LoRaDriver` → `SensorLinkManager` → `HallParkingService`; 알림 상태 경계는 `parking_alert.ko`/`LinuxDriverAdapter` | 구현: `/dev/parking_alert` 실기기 read/write/ioctl/poll 검증, LoRa 실물 검증 필요 |
| I-15 Pi → Qt 화재 근거/Alarm ACK | `FireAlarmManager`, `MqttEventBridge` ACK handler, `parking/fire/{channel}` | 구현: OPEN/ACKNOWLEDGED/RESOLVED, STM32 실제 경보 출력은 별도 |
| I-16 Pi → STM32 번호판 조명 LED | `PlateIlluminator` → `SensorLinkManager::sendAlertCommand`, payload 규격은 `docs/UART_LORA_PROTOCOL.md` | 부분 구현: Pi 송신부와 점등 정책 구현, STM32 수신·GPIO·페일세이프 타이머 미구현 |

## 현재 홀센서 대체 입력 흐름

```text
parking/sensor/hall MQTT
→ MqttEventBridge::onMessage()
→ HallParkingService::handleLine()
→ SensorProtocolParser / ParkingSensorEventAdapter
→ OCCUPIED confirmation gate / EV01~EV04 상태 전이
→ SQLite PARKING_SESSION 생성
→ CaptureScheduler의 T0+30초/60초 작업
→ Camera Snapshot API original/enhanced 다운로드 + ROI crop
→ IMAGE_LOG / Gemini OCR / EV·PHEV Timer
→ EventManager
→ parking/v1/events|state/{slot_id}
```

실제 UART와 LoRa frame도 MQTT test transport와 동일하게
`HallParkingService::handleLine()`을 재사용한다.

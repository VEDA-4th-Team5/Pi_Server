# Architecture Traceability

| Interface | Current implementation | Status |
|---|---|---|
| I-01 Hall Sensor to STM32 | No STM32 source in this repository | Missing |
| I-02 STM32 to Pi UART | `SensorLinkManager` parses `SENSOR:`/`FIRE:` frames; fire path wired to `FireAlarmManager`, hall path wired to `ParkingSessionWorker` (session state machine). `ParkingTriggerCoordinator::recordHallState()` correlation is a separate, not-yet-wired concern (EVDA-134 scope note) | Partial |
| I-03 Camera to Pi MQTT/BestShot | `MqttEventBridge`, `CameraEventParser`, `BestShotReceiver` | Implemented |
| I-04 Pi and Camera image acquisition | `RtspStreamReceiver`, `SnapshotStorage`, BestShot HTTPS download; post-entry capture request scheduler (`CaptureScheduler` + `CaptureSchedulerRuntime`, T0+30s/T0+60s) publishes on a **draft** topic behind `CAPTURE_SCHED_ENABLED`, pending the EVDA-138 capture protocol | Partial |
| I-05 Camera to Qt RTSP | Qt client is outside this repository | Missing here |
| I-06 Pi to Gemini OCR | `GeminiOcrClient`, `OcrWorker` | Implemented |
| I-07 Pi to Vehicle DB | `EventDatabase`, `db_get_vehicle_by_plate` | Implemented |
| I-08 SQLite/File storage | `EventDatabase`, `db_manager.c`, `data/` | Implemented |
| I-09 Pi to Qt MQTT | `MqttEventBridge::publish` | Partial |
| I-10 Qt to Pi HTTP | No HTTP service | Missing |
| I-11 Fire candidate to control room | `SensorLinkManager` to `FireAlarmManager` to `parking/fire/<slot_id>` MQTT; no DB row, no camera cross-check, no auto action | Partial |
| I-12 through I-15 LoRa/ACK | No production implementation | Missing |

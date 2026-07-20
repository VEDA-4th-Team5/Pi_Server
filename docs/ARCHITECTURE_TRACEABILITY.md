# Architecture Traceability

| Interface | Current implementation | Status |
|---|---|---|
| I-01 Hall Sensor to STM32 | No STM32 source in this repository | Missing |
| I-02 STM32 to Pi UART | `ParkingTriggerCoordinator::recordHallState()` is only an internal hook | Partial |
| I-03 Camera to Pi MQTT/BestShot | `MqttEventBridge`, `CameraEventParser`, `BestShotReceiver` | Implemented |
| I-04 Pi and Camera image acquisition | `RtspStreamReceiver`, `SnapshotStorage`, BestShot HTTPS download | Partial |
| I-05 Camera to Qt RTSP | Qt client is outside this repository | Missing here |
| I-06 Pi to Gemini OCR | `GeminiOcrClient`, `OcrWorker` | Implemented |
| I-07 Pi to Vehicle DB | `EventDatabase`, `db_get_vehicle_by_plate` | Implemented |
| I-08 SQLite/File storage | `EventDatabase`, `db_manager.c`, `data/` | Implemented |
| I-09 Pi to Qt MQTT | `MqttEventBridge::publish` | Partial |
| I-10 Qt to Pi HTTP | No HTTP service | Missing |
| I-11 through I-15 Fire/LoRa/ACK | No production implementation | Missing |

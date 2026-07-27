# Architecture Traceability

| Interface | Current implementation | Status |
|---|---|---|
| I-01 Hall Sensor to STM32 | No STM32 source in this repository | Missing |
| I-02 STM32 to Pi UART | `SensorLinkManager` parses `SENSOR:`/`FIRE:` frames; fire path wired to `FireAlarmManager`, hall path wired to `ParkingSessionWorker` (session state machine). `ParkingTriggerCoordinator::recordHallState()` correlation is a separate, not-yet-wired concern (EVDA-134 scope note) | Partial |
| I-03 Camera to Pi MQTT/BestShot | `MqttEventBridge`, `CameraEventParser`, `BestShotReceiver` | Implemented |
| I-04 Pi and Camera image acquisition | `RtspStreamReceiver`, `SnapshotStorage`, BestShot HTTPS download; post-entry capture request scheduler (`CaptureScheduler` + `CaptureSchedulerRuntime`, T0+30s/T0+60s) publishes on a **draft** topic behind `CAPTURE_SCHED_ENABLED`, pending the EVDA-138 capture protocol. The capture image itself comes from the verified local path (`SnapshotStorage::saveSlotRoiSnapshot`, RTSP latest frame + slot ROI crop), not from an unconfirmed camera response — swap only that step when EVDA-138 lands | Partial |
| I-05 Camera to Qt RTSP | Qt client is outside this repository | Missing here |
| I-06 Pi to Gemini OCR | `GeminiOcrClient`, `OcrWorker`; hall captures run through `OcrWorker::enqueueHallCapture` and report back via `HallCaptureCallback`, so the 30s-first / 60s-fallback policy (`HallOcrPolicy`) owns the retry budget instead of the transport | Implemented |
| I-07 Pi to Vehicle DB | `EventDatabase`, `db_get_vehicle_class_by_plate` (EV/PHEV/NON_EV; the older `db_get_vehicle_by_plate` collapses PHEV into NON_EV and is kept only for the C test) | Implemented |
| I-08 SQLite/File storage | `EventDatabase`, `db_manager.c`, `data/`; `HallCaptureCoordinator` binds the in-memory hall session id to `PARKING_SESSION.session_id` so captures, `IMAGE_LOG` rows (`HALL_30S`/`HALL_60S`), OCR results and `EVENT_LOG` rows all hang off one session (EVDA-136) | Implemented |
| I-09 Pi to Qt MQTT | `MqttEventBridge::publish` | Partial |
| I-10 Qt to Pi HTTP | `ParkingHttpServer` behind `HTTP_API_ENABLED` serves slot/session/image queries (`http-api-test` covers it). `ev_status` reports `UNKNOWN` — never `NON_EV` — when no vehicle is bound, so an unread plate is not shown as a proven combustion vehicle. PHEV is still reported as `NON_EV` here because the slot query does not select `is_phev` | Partial |
| I-11 Fire candidate to control room | `SensorLinkManager` to `FireAlarmManager` to `parking/fire/<slot_id>` MQTT; no DB row, no camera cross-check, no auto action | Partial |
| I-12 through I-15 LoRa/ACK | No production implementation | Missing |

## Notes

- `parking_timer::TimerManager` now builds into `pi-server` as well as the standalone
  `parking-timer` binary. A hall session confirmed as EV/PHEV is scheduled on the overtime
  timer under its **existing** `PARKING_SESSION.session_id`; creating a session there would
  duplicate the parking event and trip `ux_parking_session_active_slot`. Both binaries share
  one `database::EventDatabase`, so no second DB layer was introduced.
- `parking-session-worker-test` fails on `develop` as of 578b713 (EVDA-135 merge): the gated
  worker returns `std::nullopt` where the test expects a no-change transition result. Not
  caused by EVDA-136 — verified against a clean checkout of that commit.

# MQTT Protocol Proposal

Existing camera input subscription: `+/onvif-ej/#`.

Existing Pi-to-Qt prefix is configured by `QT_EVENT_TOPIC_PREFIX`. A finalized
parking protocol still needs agreement on event ID, QoS, retain, command topic,
and error responses. Proposed state payload fields include slot, parking state,
plate/OCR state, EV state, occupancy timestamps, evidence paths, and timestamp.

Qt commands such as `ALARM_ACK` and `STATUS_REQUEST` are not implemented yet.

## Fire candidate alarm (draft, not finalized)

Topic: `parking/fire/<slot_id>` (prefix from `FIRE_TOPIC_PREFIX`). A fire sensor
that has no slot mapping publishes to `parking/fire/unmapped` rather than being
dropped. **Qt must subscribe to this branch explicitly** unless it already
subscribes to `parking/#`.

The payload uses the same field set as the camera event payload
(`EventPayloadBuilder`) so the Qt parser stays single. Fire-specific values:

| Field | Value |
|---|---|
| `source_type` | `sensor_uart` |
| `source_id` | STM32 sensor id, e.g. `FIRE01` |
| `event_type` | `sensor_fire_suspected` / `sensor_fire_cleared` |
| `severity` | `critical` when detected, `info` when cleared |
| `active` | `true` while the sensor reports fire |
| `slot_id` | mapped from `FIRE_SENSOR_SLOT_MAP`, empty when unmapped |
| `snapshot_mode` | `none` (no image is captured on this path yet) |
| `raw_topic` | transport name, e.g. `uart` |
| `raw_payload` | the received frame, e.g. `FIRE:FIRE01:DETECTED:12:1700000000000` |

`camera_id`, `channel_id`, `iva_area_id`, `snapshot_path`, `clip_path` and
`ack_state` are present with empty or default values so Qt can parse both event
kinds with one code path.

This is a **candidate**, not a confirmed fire: the Pi never auto-confirms and
never takes an action. Confirmation is the control room operator's decision.

UART frame (text, draft): `FIRE:<sensor_id>:<DETECTED|CLEARED>[:<sequence>[:<unix_epoch_ms>]]`.
Repeated identical states and out-of-order sequence numbers are suppressed by
`FireAlarmManager`.

## Post-entry capture request (draft, not finalized — EVDA-135)

After a hall session starts (OCCUPIED confirmed at T0), the Pi asks the camera to
shoot the settled plate at **T0+30s** and **T0+60s**. `CaptureScheduler` owns the
schedule/dedup/retry policy; `CaptureSchedulerRuntime` publishes each request.
This path is **off by default** — set `CAPTURE_SCHED_ENABLED=true` to enable it —
because the camera capture protocol itself is **EVDA-138 and not yet confirmed**.
Topic prefix and payload below are a placeholder; when EVDA-138 lands, only the
publisher lambda in `main.cpp` and `CAPTURE_TOPIC_PREFIX` change.

Topic: `<CAPTURE_TOPIC_PREFIX>/<slot_id>` (default prefix `parking/capture`).

Draft payload (`capture_request_draft_v0`):

| Field | Value |
|---|---|
| `session_id` | hall occupancy session id (matches `PARKING_SESSION` logs) |
| `slot_id` | e.g. `EV01` |
| `sensor_id` | hall sensor id, e.g. `HALL01` |
| `camera_id` / `channel_id` | from `AppConfig` / `iva_areas` mapping |
| `area_name` | IVA area name for the slot |
| `reason` | `HALL_OCCUPIED_30S` / `HALL_OCCUPIED_60S` |
| `attempt` | 1-based; increments on retry |
| `roi` | normalized `{x,y,w,h}` (0.0~1.0) from `iva_areas` |
| `response_timeout_ms` | how long the Pi waits for a response before retrying |
| `occupied_at` / `requested_at` | ISO-8601 T0 and dispatch time |

Isolation (AC): a capture with no response within `response_timeout_ms`
(default 3000) is retried every `CAPTURE_RETRY_INTERVAL_MS` (default 2000), at
most `CAPTURE_MAX_RETRIES` (default 2) times. Each `session_id` yields at most
one successful 30s and one successful 60s request. A camera/MQTT fault never
stops the session state machine or the 1-hour timer, and an early VACANT cancels
any not-yet-sent captures for that session.

`config/env` keys: `CAPTURE_SCHED_ENABLED`, `CAPTURE_TOPIC_PREFIX`,
`CAPTURE_RESPONSE_TIMEOUT_MS`, `CAPTURE_RETRY_INTERVAL_MS`, `CAPTURE_MAX_RETRIES`.
Reference: `hall_ocr_integration/Camera MQTT Capture Protocol` §4 · §5 · §8.

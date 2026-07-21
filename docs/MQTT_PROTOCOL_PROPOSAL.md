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

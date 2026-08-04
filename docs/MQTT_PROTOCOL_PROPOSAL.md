# MQTT Protocol Proposal

Existing camera input subscription: `+/onvif-ej/#`.

Existing Pi-to-Qt prefix is configured by `QT_EVENT_TOPIC_PREFIX`. A finalized
parking protocol still needs agreement on event ID, QoS, retain, command topic,
and error responses. Proposed state payload fields include slot, parking state,
plate/OCR state, EV state, occupancy timestamps, evidence paths, and timestamp.

화재 `ALARM_ACK` 명령은 구현됐다. 일반 주차 알람 ACK와 `STATUS_REQUEST`는 아직
구현되지 않았다.

## Demo channel fire candidate alarm

For the demonstration, `FLAME01` through `FLAME04` are independent input IDs
mapped to Qt camera channels `ch01` through `ch04`. The latest state is retained
on `parking/fire/<channel_id>`. The same logical event is also published without
retain on `parking/v1/events/<channel_id>`. Both messages carry the same
`event_id`. Parking occupancy remains on `parking/v1/state/<slot_id>` and fire
messages must not overwrite that topic.

The payload uses the same field set as the camera event payload
(`EventPayloadBuilder`) so the Qt parser stays single. Fire-specific values:

| Field | Value |
|---|---|
| `event_id` | `fire-<sensor_id>-<sequence>`; timestamp fallback without sequence |
| `alarm_id` | OPEN에서 생성된 화재 알람 ID; ACK/CLEAR가 같은 알람을 가리킬 때 유지 |
| `source_type` | `sensor_uart` |
| `source_id` | STM32/demo input id, e.g. `FLAME01` |
| `event_type` | `FIRE_SUSPECTED` / `FIRE_CLEARED` |
| `alarm_kind` | `FIRE_SUSPECTED` / `NONE` |
| `alarm_state` | `OPEN` / `RESOLVED` |
| `severity` | `critical` when detected, `info` when cleared |
| `active` | `true` while the sensor reports fire |
| `scope` | `CAMERA_CHANNEL` |
| `channel_id` | mapped from `FIRE_SENSOR_CHANNEL_MAP` |
| `zone_id` | empty |
| `slot_id` | empty; channel fire is not a parking-slot classification |
| `snapshot_mode` | `none` (no image is captured on this path yet) |
| `raw_topic` | transport name, e.g. `uart` |
| `raw_payload` | the received frame, e.g. `FIRE:FLAME01:DETECTED:12:1700000000000` |

`camera_id`, `channel_id`, `iva_area_id`, `snapshot_path`, `clip_path` and
`ack_state` are present with empty or default values so Qt can parse both event
kinds with one code path.

This is a **candidate**, not a confirmed fire: the Pi never auto-confirms and
never takes an action. Confirmation is the control room operator's decision.

UART frame (text, draft): `FIRE:<sensor_id>:<DETECTED|CLEARED>[:<sequence>[:<unix_epoch_ms>]]`.
Repeated identical states and out-of-order sequence numbers are suppressed by
`FireAlarmManager`.

### Fire alarm acknowledgement

Qt Check command topic:

```text
parking/v1/commands/fire/{channel_id}
```

Payload:

```json
{
  "command": "ALARM_ACK",
  "channel_id": "ch01",
  "alarm_id": "fire-FLAME01-12"
}
```

`alarm_id`는 현재 `FIRE_SUSPECTED` OPEN payload에서 받은 값을 그대로 사용한다.
하위 호환을 위해 `alarm_id` 대신 OPEN의 `event_id`도 허용한다. 토픽 channel과 payload
channel이 다르거나, 활성 알람이 없거나, ID가 다르면 거부한다.

정상 ACK 상태는 다음 필드를 가진다.

```json
{
  "event_type": "FIRE_ACKNOWLEDGED",
  "alarm_kind": "FIRE_SUSPECTED",
  "alarm_state": "ACKNOWLEDGED",
  "ack_state": "acknowledged",
  "active": true
}
```

ACK는 화재 해제가 아니다. 동일 DETECTED는 ACK 상태를 OPEN으로 되돌리지 않으며,
센서 `FIRE_CLEARED`를 받은 경우에만 `RESOLVED / active=false`가 된다. 동일 ACK 재전송은
성공으로 간주하되 새 상태 메시지를 만들지 않는다.

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

# MQTT Protocol Proposal

Existing camera input subscription: `+/onvif-ej/#`.

Existing Pi-to-Qt prefix is configured by `QT_EVENT_TOPIC_PREFIX`. A finalized
parking protocol still needs agreement on event ID, QoS, retain, command topic,
and error responses. Proposed state payload fields include slot, parking state,
plate/OCR state, EV state, occupancy timestamps, evidence paths, and timestamp.

Qt commands such as `ALARM_ACK` and `STATUS_REQUEST` are not implemented yet.

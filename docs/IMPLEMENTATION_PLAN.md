# Implementation Plan

## Current baseline

- C++17 headless Raspberry Pi server
- Camera RTSP frame reception and Hanwha BestShot metadata reception
- Camera MQTT IVA event parsing
- OpenCV original/enhanced image pipeline
- Gemini plate OCR and SQLite EV lookup

## Next phases

1. Add STM32 UART line buffering and validated slot-state messages.
2. Add idempotent OCCUPIED/VACANT session transitions.
3. Capture multiple RTSP frames on a Hall Sensor event and select clear frames.
4. Publish normalized parking state to the Qt client.
5. Add read-only HTTP status endpoints.
6. Add fire-candidate interfaces and mocks only after sensor/LoRa protocol confirmation.

Unconfirmed hardware and protocol details must remain configurable rather than hardcoded.

# Implementation Plan (historical — 초기 계획, 아래 6단계 모두 완료됨)

> 이 문서가 "Next phases"로 나열한 STM32 UART, OCCUPIED/VACANT 전이, Qt 발행,
> HTTP 상태 API, 화재 인터페이스는 모두 구현됐다. C++17이라는 baseline도
> 현재는 C++20이다(`CMakeLists.txt`). 현재 아키텍처는
> [`docs/architecture/README.md`](architecture/README.md)를 참고한다.

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

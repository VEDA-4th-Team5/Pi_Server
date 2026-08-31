# Developer Walkthrough (historical — 프로젝트 초기 상태)

> 이 문서는 UART/HTTP/타이머/화재 알람이 구현되기 전 초기 상태의 기록이다.
> 아래 "not yet implemented" 목록은 현재 모두 구현돼 있다. 현재 아키텍처는
> [`docs/architecture/README.md`](architecture/README.md)를 참고한다.

1. `src/main.cpp` constructs and starts every server component.
2. `src/camera/RtspStreamReceiver.cpp` keeps the latest camera frame.
3. `src/mqtt/MqttEventBridge.cpp` receives camera MQTT events.
4. `src/snapshot/SnapshotStorage.cpp` crops and stores an IVA slot image.
5. `src/ocr/PlateImageEnhancer.cpp` writes the enhanced image.
6. `src/ocr/OcrWorker.cpp` queues OCR work.
7. `src/ocr/GeminiOcrClient.cpp` performs the Gemini HTTPS request.
8. `src/database/EventDatabase.cpp` stores images/events and performs EV lookup.
9. `src/bestshot/BestShotReceiver.cpp` receives Vehicle/Plate BestShot references.
10. `src/parking/ParkingTriggerCoordinator.cpp` correlates IVA or future Hall events.

UART, Qt command handling, HTTP, timers, and fire alarm state machines are not yet
implemented in this repository.

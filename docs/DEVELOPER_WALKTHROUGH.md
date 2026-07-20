# Developer Walkthrough

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

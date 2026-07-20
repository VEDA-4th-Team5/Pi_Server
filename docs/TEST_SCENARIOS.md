# Test Scenarios

## Currently available

- CMake compile of `pi-server`, `gemini-ocr-test`, and `db-manager-test`.
- Manual RTSP/MQTT IVA integration test.
- Manual Gemini OCR test using an existing plate image.

## Required automated coverage

- UART valid/corrupt/partial/multi-line parsing;
- duplicate OCCUPIED/VACANT idempotency;
- session entry and exit transaction rollback;
- OCR failure remains UNKNOWN rather than NON_EV;
- MQTT state payload and invalid Qt commands;
- restart recovery of active sessions;
- fire alarm state transitions and mock verifier;
- Camera, Gemini, and MQTT failure adapters.

Never run `db-manager-test` against `data/db/parking.db`; pass an initialized copy.

# Merge Notes

`pi-server_merged` is based on `pi-server_submit`. The original submit and
Taejun directories and timestamped copies remain unchanged.

Imported from `pi_server_taejun`:

- transport-neutral sensor message parsing and sensor-to-slot adaptation;
- sensor sequence duplicate rejection;
- parking slot configuration and lookup;
- in-memory occupancy session transitions;
- active-session indexing;
- pure C++ parking-domain and sensor-pipeline tests.

Kept from `pi-server_submit`:

- RTSP and MQTT runtime;
- BestShot receiver;
- OpenCV enhancement and Gemini OCR;
- SQLite parking/vehicle database;
- directory layout and CMake build.

The imported sensor domain is intentionally not wired into `main.cpp` yet.
The physical UART device, framing, and final STM32 wire protocol are not
confirmed. Current tests use `SENSOR:HALL02:OCCUPIED:10` style input and prove
the parser, mapping, duplicate guard, and OCCUPIED/VACANT transitions without
hardware.

Added after the original merge:

- embedded C++ HTTP/HTTPS API for Qt parking-slot and image queries;
- read-only SQLite views used by the API;
- isolated HTTP API integration test using a temporary DB and image file.

# Codex Handoff

## Workspace

- Target Raspberry Pi project root: `/home/B777/Pi_Server`
- Source repository: `VEDA-4th-Team5/Pi_Server`
- Current branch: `feat/EVDA-212-entry-decision-logic-fix`
- Current HEAD: `f827d0d [Fix] IVA 영역 점유 판정 및 촬영 경로 정리 (EVDA-212)`
- Previous feature commit: `f6e3625 [Feat] IVA·홀센서 하이브리드 입출차 판정 구현 (EVDA-212)`
- Source Codex CLI version: `codex-cli 0.147.0`

## Mandatory Working Rules

Read `AGENTS.md` before taking any action.

- Do not modify source, settings, databases, or documentation without explicit user approval.
- For a requested change, investigate read-only first, report evidence and a proposed implementation, then wait for approval.
- Do not use Python. Server and Qt code are C/C++; STM32 firmware is C.
- Preserve user-owned or unrelated changes. In particular, do not stage or modify `AGENTS.md` or `lora_console.py` unless explicitly requested.
- Do not commit camera credentials, Gemini keys, Telegram tokens, or other secrets.

## Current Architecture

```text
Hanwha WiseAI IVA MQTT + STM32 Hall UART
        -> Mosquitto / HallParkingService
        -> authoritative occupancy transaction
        -> one PARKING_SESSION session_id per slot
        -> CaptureScheduler / EvidenceCaptureWorker
        -> camera frame or configured capture source
        -> normalized slot ROI crop
        -> SnapshotStorage / IMAGE_LOG / Gemini OCR
        -> HTTP API and normalized MQTT events for Qt
```

The project also contains fire MQTT state handling, LoRa/UART support, SQLite persistence, restart recovery, and Qt-facing HTTP APIs.

## EVDA-212 Hybrid Occupancy Policy

Entry is an OR policy:

```text
IVA Intrusion
OR
Hall OCCUPIED held for the configured confirmation interval
=> create exactly one active session
```

If the second source confirms later, it updates the same session instead of creating another session.

The session persists four source-state fields:

- `hall_confirmed`
- `hall_occupied`
- `iva_confirmed`
- `iva_occupied`

The effective occupancy expression is:

```text
(hall_confirmed && hall_occupied)
||
(iva_confirmed && iva_occupied)
```

Exit is source-aware:

- A source that never confirmed occupancy must not terminate the session.
- An IVA-only session ignores unrelated Hall VACANT and ends after confirmed IVA Exit.
- A Hall-only session ends from Hall VACANT without waiting for IVA.
- If both sources confirmed, one source becoming vacant does not terminate the session while the other remains occupied.
- IVA Exit uses the configured confirmation delay; a new Intrusion supersedes the pending exit.

## IVA Area State

- Native WiseAI events are authoritative.
- Fixed custom publication messages without source timestamp are not authoritative occupancy observations.
- Occupancy is tracked by the stable camera/token/rule area, not by WiseAI `ObjectId`.
- `ObjectId` remains diagnostic metadata only because the camera may report different IDs between Intrusion and Exit.
- Multiple mapped IVA areas are aggregated; the slot stays occupied while any mapped area is active.

## Capture and Cleanup

Both IVA and Hall entry paths converge on the same session/capture pipeline.

Storage stages are:

```text
data/snapshots/ch1/<SLOT>/occupancy_start/
data/snapshots/ch1/<SLOT>/occupied_30s/
data/snapshots/ch1/<SLOT>/occupied_60s/
data/snapshots/ch1/<SLOT>/overstay/
```

Internal compatibility identifiers such as `HALL_30S` and `HALL_60S` remain in DB/MQTT-facing logic; only filesystem directory names were generalized.

On session end:

- Cancel pending evidence capture, 30/60-second capture, timer, and OCR work.
- If `violation_at` is absent, delete temporary files and corresponding `IMAGE_LOG` rows.
- If `violation_at` exists, retain violation evidence and database history.

## Last Verification

Command:

```bash
ctest --test-dir cmake-build --output-on-failure -j4
```

Result:

- 27 of 28 tests passed.
- IVA, hybrid occupancy, timer, evidence, HTTP, OCR, LoRa, and MQTT lifecycle tests passed.
- `fire-broker-restart-process-test` failed because its child Mosquitto process exited during startup with an empty test log. Treat this as unresolved until reproduced and diagnosed; do not silently claim the full suite passes.
- `git diff --check` passed before commit `f827d0d`.

## Local Working Tree at Handoff

The source working tree still contains user-owned/uncommitted files that were intentionally excluded from commit `f827d0d`:

```text
M  AGENTS.md
?? lora_console.py
```

The handoff document itself may also be untracked after transfer. Do not stage these files without explicit approval.

## Next Recommended Checks on the New Pi

1. Install the same Codex CLI version and run `codex login` on the new Pi.
2. Confirm the project branch and HEAD shown above.
3. Verify environment files point to the actual camera, broker, UART devices, and SQLite path without printing secrets.
4. Reconfigure and rebuild locally because copied build artifacts can contain absolute paths from the source Pi.
5. Run CTest and separately diagnose `fire-broker-restart-process-test`.
6. Perform hardware validation for:
   - IVA-only entry/exit
   - Hall-only entry/exit
   - IVA-first then Hall confirmation
   - Hall-first then IVA confirmation
   - one source clearing while the other remains occupied
   - early departure image deletion
   - violation evidence preservation
7. Confirm the configured camera IP against `AGENTS.md` and local environment files before starting the server.

## Useful Commands

```bash
cd /home/B777/Pi_Server
git status --short
git branch --show-current
git log -3 --oneline
cmake -S . -B cmake-build
cmake --build cmake-build -j4
ctest --test-dir cmake-build --output-on-failure -j4
./run_server.sh
```

Resume migrated Codex sessions after login with:

```bash
codex resume --all -C /home/B777/Pi_Server
```

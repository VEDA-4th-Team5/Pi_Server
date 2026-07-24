#pragma once

#include "parking/ParkingSensorEvent.hpp"

#include <chrono>
#include <string>
#include <unordered_map>

namespace parking {

// Filters short OCCUPIED<->VACANT flaps (e.g. a driver nosing in and pulling
// back out while aligning) before they ever become a session, by requiring
// OCCUPIED to hold for at least `confirmThreshold` before it is "confirmed".
//
// This is NOT the STM32 hardware debounce described in
// ParkingSessionWorker's header comment. That debounce operates at the
// electrical/firmware level (ms-scale signal bounce) and stays on the STM32
// per the interface contract. This gate operates at the business-logic level
// (seconds-scale re-parking maneuvers) and is a deliberate Pi-side policy,
// opt-in via PARKING_OCCUPANCY_CONFIRM_MS (0 = disabled, matches pre-EVDA-135
// behavior where the first OCCUPIED immediately becomes T0).
//
// Pure and deterministic: no thread, no clock of its own — the caller
// supplies event timestamps, so it is unit-testable without sleeping.
// Not thread-safe by itself; ParkingSessionWorker holds its own mutex around
// every call.
class ParkingOccupancyConfirmationGate {
public:
    explicit ParkingOccupancyConfirmationGate(
        std::chrono::milliseconds confirmThreshold);

    enum class Decision {
        Forward,  // let the event reach the state machine
        Suppress  // not confirmed yet (or a discarded flap) — drop it here
    };

    // slotAlreadyOccupied reflects ParkingSlotManager's current state for
    // this slot (an already-confirmed, still-active session). The gate has
    // no dependency on ParkingSlotManager itself; the caller passes this in
    // so the gate stays a pure, independently testable policy.
    [[nodiscard]] Decision evaluate(
        const ParkingSensorEvent& event, bool slotAlreadyOccupied);

private:
    struct Pending {
        std::chrono::system_clock::time_point firstSeenAt;
    };

    std::chrono::milliseconds confirmThreshold_;
    std::unordered_map<std::string, Pending> pendingBySlot_;
};

}  // namespace parking

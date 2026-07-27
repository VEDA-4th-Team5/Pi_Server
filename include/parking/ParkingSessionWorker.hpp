#pragma once

#include "parking/ParkingOccupancyConfirmationGate.hpp"
#include "parking/ParkingSensorEvent.hpp"
#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace parking {

// L1 Core orchestration for the hall-sensor parking-occupancy path.
//
// It takes an already-adapted ParkingSensorEvent (sensor_id -> slot mapping is
// done upstream by sensor::ParkingSensorEventAdapter), rejects duplicate or
// out-of-order packets, optionally waits for OCCUPIED to hold before treating
// it as real, drives the slot state machine, and fans the resulting
// transition out to registered sinks (the active-session index, logging, and
// the T0+30s/T0+60s capture scheduler from EVDA-135).
//
// This class performs NO signal debounce of its own. Per the interface
// contract (architecture/overview/interface-matrix.md), electrical/firmware
// debouncing is the STM32's responsibility and must not be repeated on the
// Pi. The 1~2s polling stream is handled idempotently: repeated
// OCCUPIED/VACANT states collapse to DuplicateOccupiedIgnored /
// DuplicateVacantIgnored inside ParkingSlotManager, and out-of-order packets
// are dropped by ParkingSensorSequenceGuard when the transport supplies
// sequence numbers.
//
// Separately, `confirmThreshold` (opt-in, default disabled) is a
// business-logic policy, not a debounce: it filters seconds-scale
// OCCUPIED<->VACANT flaps (e.g. a driver aligning by pulling in and back out)
// via ParkingOccupancyConfirmationGate, so T0 reflects a settled parking
// attempt rather than the first raw blip. See that class's header for the
// distinction from the STM32 boundary above.
//
// The class owns no I/O. The transport (SensorLinkManager) calls onSensorEvent
// from its reader thread, so every public method is mutex-guarded.
class ParkingSessionWorker {
public:
    // Invoked once per accepted event, in registration order, while the worker
    // mutex is held. A sink must not call back into the worker.
    using TransitionSink =
        std::function<void(const ParkingTransitionResult&)>;

    // confirmThreshold: minimum time OCCUPIED must hold before it is
    // confirmed as a session start. Zero (default) disables the gate, giving
    // pre-EVDA-135 behavior where the first OCCUPIED is immediately T0.
    explicit ParkingSessionWorker(
        std::vector<ParkingSlotConfig> slotConfigs,
        std::chrono::milliseconds confirmThreshold =
            std::chrono::milliseconds::zero());

    void addSink(TransitionSink sink);

    // Feed one adapted sensor event.
    // Returns std::nullopt when the sequence guard rejects the packet (no state
    // change, no sink invoked); otherwise returns the slot state machine
    // transition and invokes every registered sink with it.
    std::optional<ParkingTransitionResult> onSensorEvent(
        const ParkingSensorEvent& event);

    [[nodiscard]] std::size_t slotCount() const noexcept;

private:
    mutable std::mutex mutex_;
    ParkingSensorSequenceGuard sequenceGuard_;
    ParkingSlotManager slotManager_;
    std::optional<ParkingOccupancyConfirmationGate> confirmationGate_;
    std::vector<TransitionSink> sinks_;
};

}  // namespace parking

#pragma once

#include "parking/ParkingSensorEvent.hpp"
#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace parking {

// L1 Core orchestration for the hall-sensor parking-occupancy path.
//
// It takes an already-adapted ParkingSensorEvent (sensor_id -> slot mapping is
// done upstream by sensor::ParkingSensorEventAdapter), rejects duplicate or
// out-of-order packets, drives the slot state machine, and fans the resulting
// transition out to registered sinks (the active-session index, logging, and
// later the T0+30s/T0+60s capture scheduler in EVDA-135).
//
// This class performs NO signal debounce. Per the interface contract
// (architecture/overview/interface-matrix.md), debouncing is the STM32's
// responsibility and must not be repeated on the Pi. The 1~2s polling stream
// is handled idempotently: repeated OCCUPIED/VACANT states collapse to
// DuplicateOccupiedIgnored / DuplicateVacantIgnored inside ParkingSlotManager,
// and out-of-order packets are dropped by ParkingSensorSequenceGuard when the
// transport supplies sequence numbers.
//
// The class owns no I/O. The transport (SensorLinkManager) calls onSensorEvent
// from its reader thread, so every public method is mutex-guarded.
class ParkingSessionWorker {
public:
    // Invoked once per accepted event, in registration order, while the worker
    // mutex is held. A sink must not call back into the worker.
    using TransitionSink =
        std::function<void(const ParkingTransitionResult&)>;

    explicit ParkingSessionWorker(
        std::vector<ParkingSlotConfig> slotConfigs);

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
    std::vector<TransitionSink> sinks_;
};

}  // namespace parking

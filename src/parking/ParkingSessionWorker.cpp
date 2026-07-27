#include "parking/ParkingSessionWorker.hpp"

#include <utility>

namespace parking {

ParkingSessionWorker::ParkingSessionWorker(
    std::vector<ParkingSlotConfig> slotConfigs,
    std::chrono::milliseconds confirmThreshold)
    : slotManager_(std::move(slotConfigs)) {
    // A zero threshold means "no gate at all" (pre-EVDA-135 behavior), not a
    // gate with a zero-length window: the gate always suppresses the first
    // sighting and waits for a second event to confirm, which would still
    // delay T0 by one poll cycle even at threshold==0.
    if (confirmThreshold > std::chrono::milliseconds::zero()) {
        confirmationGate_.emplace(confirmThreshold);
    }
}

void ParkingSessionWorker::addSink(TransitionSink sink) {
    if (!sink) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

std::optional<ParkingTransitionResult>
ParkingSessionWorker::onSensorEvent(const ParkingSensorEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Drop duplicate/out-of-order packets. Events without a transport sequence
    // are always accepted so the same pipeline works during text-protocol tests.
    if (!sequenceGuard_.accept(event)) {
        return std::nullopt;
    }

    if (confirmationGate_.has_value()) {
        const ParkingSlot* slot = slotManager_.findSlot(event.slotId);
        // Only gate events the state machine would otherwise accept as a
        // real slot/sensor match. Unknown slots, disabled slots, and sensor
        // mismatches must still reach slotManager_.handle() below so their
        // proper error codes surface instead of being silently suppressed.
        const bool eligibleForGate =
            slot != nullptr && slot->config.enabled &&
            !event.sensorId.empty() && event.sensorId == slot->config.sensorId;

        if (eligibleForGate) {
            const bool alreadyOccupied = slot->occupied();
            if (confirmationGate_->evaluate(event, alreadyOccupied) ==
                ParkingOccupancyConfirmationGate::Decision::Suppress) {
                return std::nullopt;
            }
        }
    }

    ParkingTransitionResult result = slotManager_.handle(event);

    for (const auto& sink : sinks_) {
        sink(result);
    }
    return result;
}

std::size_t ParkingSessionWorker::slotCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return slotManager_.slotCount();
}

}  // namespace parking

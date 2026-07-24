#include "parking/ParkingSessionWorker.hpp"

#include <utility>

namespace parking {

ParkingSessionWorker::ParkingSessionWorker(
    std::vector<ParkingSlotConfig> slotConfigs)
    : slotManager_(std::move(slotConfigs)) {
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

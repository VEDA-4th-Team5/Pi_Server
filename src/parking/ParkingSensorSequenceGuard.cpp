#include "parking/ParkingSensorSequenceGuard.hpp"

namespace parking {

bool ParkingSensorSequenceGuard::accept(
    const ParkingSensorEvent& event,
    std::string* reason) {
    if (!event.sourceSequence.has_value()) {
        return true;
    }

    const auto found = lastBySensor_.find(event.sensorId);
    if (found != lastBySensor_.end() &&
        *event.sourceSequence <= found->second) {
        if (reason != nullptr) {
            *reason = "duplicate or stale sensor sequence";
        }
        return false;
    }

    lastBySensor_[event.sensorId] = *event.sourceSequence;
    return true;
}

void ParkingSensorSequenceGuard::reset(
    const std::string& sensorId) {
    lastBySensor_.erase(sensorId);
}

void ParkingSensorSequenceGuard::clear() {
    lastBySensor_.clear();
}

}  // namespace parking

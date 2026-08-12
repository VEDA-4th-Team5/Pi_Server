#include "parking/ParkingSensorSequenceGuard.hpp"

namespace parking {

bool ParkingSensorSequenceGuard::accept(
    const ParkingSensorEvent& event,
    std::string* reason) {
    auto& state = stateBySensor_[event.sensorId];
    const sensor::SensorSequenceFact fact{
        event.sourceProtocolVersion,
        event.sourceBootId,
        event.sourceSequence};
    const auto decision = sensor::evaluateSensorSequence(state, fact);
    if (!decision.accepted()) {
        if (reason != nullptr) *reason = decision.reason;
        return false;
    }
    sensor::commitSensorSequence(state, fact);
    return true;
}

void ParkingSensorSequenceGuard::reset(
    const std::string& sensorId) {
    stateBySensor_.erase(sensorId);
}

void ParkingSensorSequenceGuard::clear() {
    stateBySensor_.clear();
}

}  // namespace parking

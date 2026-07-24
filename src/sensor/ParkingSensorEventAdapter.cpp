#include "sensor/ParkingSensorEventAdapter.hpp"

namespace sensor {

ParkingSensorEventAdapter::ParkingSensorEventAdapter(
    const parking::SensorSlotIndex& slotIndex)
    : slotIndex_(slotIndex) {}

std::optional<parking::ParkingSensorEvent>
ParkingSensorEventAdapter::adapt(
    const SensorProtocolMessage& message,
    std::string* error) const {
    const auto match =
        slotIndex_.findBySensorId(message.sensorId);

    if (!match.has_value()) {
        if (error != nullptr) {
            *error = "sensor id is not mapped to an enabled slot";
        }
        return std::nullopt;
    }

    parking::ParkingSensorEvent event;
    event.slotId = match->slotId;
    event.sensorId = message.sensorId;
    event.state = message.state;
    event.occurredAt = message.occurredAt;
    event.sourceSequence = message.sequence;
    event.sourceTransport = message.transport;
    event.receivedMonotonic = message.receivedMonotonic;
    return event;
}

}  // namespace sensor

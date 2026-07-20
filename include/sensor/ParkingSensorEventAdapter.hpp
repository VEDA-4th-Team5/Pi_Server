#pragma once

#include "parking/ParkingSensorEvent.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "sensor/SensorProtocolMessage.hpp"

#include <optional>
#include <string>

namespace sensor {

class ParkingSensorEventAdapter {
public:
    explicit ParkingSensorEventAdapter(
        const parking::SensorSlotIndex& slotIndex);

    [[nodiscard]] std::optional<parking::ParkingSensorEvent> adapt(
        const SensorProtocolMessage& message,
        std::string* error = nullptr) const;

private:
    const parking::SensorSlotIndex& slotIndex_;
};

}  // namespace sensor

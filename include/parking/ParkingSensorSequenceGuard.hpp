#pragma once

#include "parking/ParkingSensorEvent.hpp"
#include "sensor/SensorSequencePolicy.hpp"

#include <string>
#include <unordered_map>

namespace parking {

// In-memory wrapper used by the legacy parking-domain worker.  Production Hall
// commits the same SensorSequencePolicy together with its durable transition.
class ParkingSensorSequenceGuard {
public:
    [[nodiscard]] bool accept(
        const ParkingSensorEvent& event,
        std::string* reason = nullptr);

    void reset(const std::string& sensorId);
    void clear();

private:
    std::unordered_map<std::string, sensor::SensorSequenceState>
        stateBySensor_;
};

}  // namespace parking

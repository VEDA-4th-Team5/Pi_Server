#pragma once

#include "sensor/SensorProtocolMessage.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace sensor {

// Test protocol:
//   SENSOR:<sensor_id>:<OCCUPIED|VACANT>
//   SENSOR:<sensor_id>:<state>:<sequence>
//   SENSOR:<sensor_id>:<state>:<sequence>:<unix_epoch_ms>
//
// The final STM32 binary protocol may use a different parser. Both parsers
// must produce the same SensorProtocolMessage.
class SensorProtocolParser {
public:
    [[nodiscard]] std::optional<SensorProtocolMessage> parse(
        const std::string& line,
        std::chrono::system_clock::time_point receivedAt,
        std::string* error = nullptr) const;
};

}  // namespace sensor

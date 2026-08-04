#pragma once

#include "sensor/FireSensorMessage.hpp"
#include "sensor/SensorProtocolMessage.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace sensor {

// Test protocol:
//   SENSOR:<sensor_id>:<OCCUPIED|VACANT>
//   SENSOR:<sensor_id>:<state>:<sequence>
//   SENSOR:<sensor_id>:<state>:<sequence>:<unix_epoch_ms>
//   FIRE:<sensor_id>:<DETECTED|CLEARED>
//   FIRE:<sensor_id>:<state>:<sequence>
//   FIRE:<sensor_id>:<state>:<sequence>:<unix_epoch_ms>
//
// The final STM32 binary protocol may use a different parser. Both parsers
// must produce the same SensorProtocolMessage / FireSensorMessage.
class SensorProtocolParser {
public:
    [[nodiscard]] std::optional<SensorProtocolMessage> parse(
        const std::string& line,
        std::chrono::system_clock::time_point receivedAt,
        std::string* error = nullptr) const;

    [[nodiscard]] std::optional<FireSensorMessage> parseFire(
        const std::string& line,
        std::chrono::system_clock::time_point receivedAt,
        std::string* error = nullptr) const;

    // Cheap prefix test so a transport reader can route a line to the right
    // parser without attempting both and discarding one error message.
    [[nodiscard]] static bool isFireLine(const std::string& line);
};

}  // namespace sensor

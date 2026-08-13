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
//   SENSOR2:<sensor_id>:<state>:<boot_id>:<sequence>[:unix_epoch_ms]
//   FIRE:<sensor_id>:<DETECTED|CLEARED>
//   FIRE:<sensor_id>:<state>:<sequence>
//   FIRE:<sensor_id>:<state>:<sequence>:<unix_epoch_ms>
//   FIRE2:<sensor_id>:<state>:<boot_id>:<sequence>[:unix_epoch_ms]
//
// STM <-> Pi LoRa spec v1.1 (real hardware, node field, single per-node
// sequence counter):
//   SENSOR:<node>:<sensor_id>:<state>:<sequence>
//   FIRE:<node>:<sensor_id>:<state>:<sequence>:<energy>
// <node> is "STM1" or "STM2". It is what tells this grammar apart from the
// legacy one above at the same field count, since the wire format carries no
// out-of-band version tag past the LoRa frame header.
//
// Both parsers must produce the same SensorProtocolMessage / FireSensorMessage.
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

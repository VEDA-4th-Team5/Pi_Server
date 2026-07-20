#pragma once

#include "parking/ParkingSensorEvent.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace sensor {

// Transport-neutral result of parsing a UART/LoRa/test text packet.
// It intentionally contains sensor_id rather than slot_id. Mapping belongs to
// configuration on the Raspberry Pi.
struct SensorProtocolMessage {
    std::string sensorId;
    parking::ParkingSensorState state{
        parking::ParkingSensorState::Vacant};
    std::chrono::system_clock::time_point occurredAt{
        std::chrono::system_clock::now()};
    std::optional<std::uint64_t> sequence;
    std::string transport{"text-test"};
};

}  // namespace sensor

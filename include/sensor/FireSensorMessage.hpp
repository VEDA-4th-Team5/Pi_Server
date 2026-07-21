#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace sensor {

enum class FireSensorState {
    Detected,
    Cleared
};

// Transport-neutral result of parsing a UART/LoRa/test fire packet.
// Like SensorProtocolMessage it carries sensor_id rather than slot_id;
// the sensor -> slot mapping is Raspberry Pi configuration.
struct FireSensorMessage {
    std::string sensorId;
    FireSensorState state{FireSensorState::Cleared};
    std::chrono::system_clock::time_point occurredAt{
        std::chrono::system_clock::now()};
    std::optional<std::uint64_t> sequence;
    std::string transport{"text-test"};

    // 관제실이 판단 근거를 볼 수 있도록 수신한 원문을 그대로 보관한다.
    std::string raw;
};

[[nodiscard]] const char* toString(FireSensorState state) noexcept;

}  // namespace sensor

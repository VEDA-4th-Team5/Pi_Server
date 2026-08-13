#pragma once

#include "sensor/SensorProtocolVersion.hpp"

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
    SensorProtocolVersion protocolVersion{SensorProtocolVersion::LegacyV1};
    std::optional<std::string> bootId;
    std::string transport{"text-test"};

    // LoRa 규격 v1.1(FIRE:<node>:<sensor>:<state>:<seq>:<energy>)에서만 채워지는
    // STM 노드 ID다. node와 마찬가지로 SensorProtocolMessage 쪽 주석을 참고한다.
    std::optional<std::string> node;

    // 1~20Hz 대역 에너지 합(표시용). 화재 판정은 STM32가 하므로 이 값으로 다시
    // 판정하지 않는다 — state만 근거로 삼는다.
    std::optional<double> energy;

    // 관제실이 판단 근거를 볼 수 있도록 수신한 원문을 그대로 보관한다.
    std::string raw;
};

[[nodiscard]] const char* toString(FireSensorState state) noexcept;

}  // namespace sensor

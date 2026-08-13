#pragma once

#include "sensor/SensorProtocolVersion.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace parking {

enum class ParkingSensorState {
    Occupied,
    Vacant
};

struct ParkingSensorEvent {
    std::string slotId;
    std::string sensorId;
    ParkingSensorState state{ParkingSensorState::Vacant};
    std::chrono::system_clock::time_point occurredAt{
        std::chrono::system_clock::now()};

    // Optional transport metadata. The parking domain does not require it,
    // but a UART/LoRa adapter can use it for duplicate and stale-packet
    // protection.
    std::optional<std::uint64_t> sourceSequence;
    sensor::SensorProtocolVersion sourceProtocolVersion{
        sensor::SensorProtocolVersion::LegacyV1};
    std::optional<std::string> sourceBootId;
    std::string sourceTransport{"unknown"};

    // 수신 순간의 단조시계 값이다. NTP로 벽시계가 바뀌어도 점유 확정과
    // 촬영 마감시간 계산이 흔들리지 않도록 경과시간 계산에만 사용한다.
    std::chrono::steady_clock::time_point receivedMonotonic{
        std::chrono::steady_clock::now()};
};

[[nodiscard]] const char* toString(
    ParkingSensorState state) noexcept;

}  // namespace parking

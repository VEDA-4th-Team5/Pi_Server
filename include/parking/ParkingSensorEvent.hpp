#pragma once

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
    std::string sourceTransport{"unknown"};
};

[[nodiscard]] const char* toString(
    ParkingSensorState state) noexcept;

}  // namespace parking

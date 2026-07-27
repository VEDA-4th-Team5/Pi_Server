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

    // Monotonic receive time, captured alongside occurredAt at the same
    // transport boundary (SensorLinkManager::dispatchLine). Used only for
    // measuring elapsed durations (e.g. ParkingOccupancyConfirmationGate's
    // hold-time check) so a system_clock/NTP jump while the process is
    // running can never corrupt a duration measurement. occurredAt
    // (system_clock, wall time) remains the source of T0 for sessions,
    // logs, DB rows and MQTT payloads -- this field is never used for that.
    std::chrono::steady_clock::time_point receivedMonotonic{
        std::chrono::steady_clock::now()};
};

[[nodiscard]] const char* toString(
    ParkingSensorState state) noexcept;

}  // namespace parking

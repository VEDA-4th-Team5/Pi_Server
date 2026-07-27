#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace parking {

enum class ParkingSessionState {
    Active,
    Completed
};

class ParkingOccupancySession {
public:
    // startedAtMonotonic is a steady_clock reading taken at the same instant
    // as startedAt (see ParkingSensorEvent::receivedMonotonic). startedAt
    // (system_clock) remains T0 for logs/DB/MQTT; startedAtMonotonic is only
    // for computing capture deadlines immune to wall-clock jumps.
    ParkingOccupancySession(
        std::string sessionId,
        std::string slotId,
        std::string sensorId,
        std::chrono::system_clock::time_point startedAt,
        std::chrono::steady_clock::time_point startedAtMonotonic);

    [[nodiscard]] const std::string& sessionId() const noexcept;
    [[nodiscard]] const std::string& slotId() const noexcept;
    [[nodiscard]] const std::string& sensorId() const noexcept;
    [[nodiscard]] ParkingSessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] std::chrono::system_clock::time_point
    startedAt() const noexcept;

    [[nodiscard]] std::chrono::steady_clock::time_point
    startedAtMonotonic() const noexcept;

    [[nodiscard]] const std::optional<
        std::chrono::system_clock::time_point>&
    endedAt() const noexcept;

    void complete(std::chrono::system_clock::time_point endedAt);

private:
    std::string sessionId_;
    std::string slotId_;
    std::string sensorId_;
    ParkingSessionState state_{ParkingSessionState::Active};
    std::chrono::system_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point startedAtMonotonic_;
    std::optional<std::chrono::system_clock::time_point> endedAt_;
};

}  // namespace parking

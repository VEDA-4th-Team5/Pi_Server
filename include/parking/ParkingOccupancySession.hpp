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
    ParkingOccupancySession(
        std::string sessionId,
        std::string slotId,
        std::string sensorId,
        std::chrono::system_clock::time_point startedAt);

    [[nodiscard]] const std::string& sessionId() const noexcept;
    [[nodiscard]] const std::string& slotId() const noexcept;
    [[nodiscard]] const std::string& sensorId() const noexcept;
    [[nodiscard]] ParkingSessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] std::chrono::system_clock::time_point
    startedAt() const noexcept;

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
    std::optional<std::chrono::system_clock::time_point> endedAt_;
};

}  // namespace parking

#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace parking {

/** @brief 센서 기반 주차 세션의 최소 생명주기 상태다. */
enum class ParkingSessionState {
    Active,
    Completed
};

/** @brief 한 차량의 입차부터 출차까지 시간과 슬롯/센서 식별자를 보존한다. */
class ParkingOccupancySession {
public:
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

    /** @brief 활성 세션을 완료하며 시작보다 이른 종료 시각은 허용하지 않는다. */
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

#pragma once

#include "parking/ParkingSensorEvent.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace parking {

// 짧은 OCCUPIED/VACANT 재정렬 신호가 DB 세션이 되는 것을 막는 Pi 정책이다.
// 상태 변경형 STM32도 지원하기 위해 첫 OCCUPIED를 보관하고, 추가 신호가 없어도
// 단조시계 deadline이 지나면 takeDue()가 확정 이벤트를 반환한다.
class ParkingOccupancyConfirmationGate {
public:
    explicit ParkingOccupancyConfirmationGate(
        std::chrono::milliseconds confirmThreshold);

    enum class Decision {
        Forward,
        Suppress
    };

    [[nodiscard]] Decision evaluate(
        const ParkingSensorEvent& event, bool slotAlreadyOccupied);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    nextDeadline() const;

    [[nodiscard]] std::vector<ParkingSensorEvent> takeDue(
        std::chrono::steady_clock::time_point monotonicNow,
        std::chrono::system_clock::time_point wallNow);

private:
    struct Pending {
        ParkingSensorEvent firstEvent;
        std::chrono::steady_clock::time_point deadline;
    };

    std::chrono::milliseconds confirmThreshold_;
    std::unordered_map<std::string, Pending> pendingBySlot_;
};

}  // namespace parking

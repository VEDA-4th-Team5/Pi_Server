#pragma once

#include "event/IvaSlotOccupancyAggregator.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace event {

enum class IvaOccupancyAction {
    Enter,
    Intrusion,
    Exit,
    Unsupported
};

struct IvaOccupancySignal {
    std::string slotId;
    std::string cameraId;
    std::string videoSourceToken;
    std::string ruleName;
    std::string objectId;
    IvaOccupancyAction action{IvaOccupancyAction::Unsupported};
    // Raw WiseAI Action은 실제 분석 결과다. 고정 Publication EXIT는 false로
    // 전달하여 실제 출차로 오인하지 않도록 한다.
    bool authoritativeExit{true};
};

enum class IvaCoordinationCode {
    Occupied,
    ExitPending,
    ExitCanceled,
    Duplicate,
    IgnoredEnter,
    IgnoredCustomExit,
    Unsupported
};

struct IvaCoordinationResult {
    IvaCoordinationCode code{IvaCoordinationCode::Unsupported};
    IvaOccupancySignal signal;
    std::size_t activeAreaCount{};
    std::size_t knownAreaCount{};
    std::size_t configuredAreaCount{};
};

/**
 * @brief WiseAI IVA 액션을 슬롯 점유/출차 확인 정책으로 정규화한다.
 *
 * INTRUSION만 점유 시작으로 인정한다. EXIT는 즉시 출차시키지 않고 확인
 * deadline을 예약하며, 그 전에 같은 슬롯의 INTRUSION이 오면 예약을 취소한다.
 * ENTER는 카메라 경계 통과 알림일 뿐 주차 점유 증거로 사용하지 않는다.
 */
class IvaOccupancyCoordinator {
public:
    IvaOccupancyCoordinator(
        const std::vector<parking::ParkingSlotConfig>& slots,
        std::chrono::milliseconds exitConfirmDelay);

    /** @brief IVA 액션 한 건을 점유 전이 또는 출차 예약으로 반영한다. */
    [[nodiscard]] IvaCoordinationResult handle(
        IvaOccupancySignal signal,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    /** @brief 가장 빠른 출차 확인 시각을 반환한다. */
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    nextDeadline() const;

    /** @brief 확인 시각이 지난 출차 신호를 큐에서 제거해 반환한다. */
    [[nodiscard]] std::vector<IvaOccupancySignal> takeDue(
        std::chrono::steady_clock::time_point now);

private:
    struct PendingExit {
        IvaOccupancySignal signal;
        std::chrono::steady_clock::time_point deadline;
    };

    std::chrono::milliseconds exitConfirmDelay_;
    IvaSlotOccupancyAggregator aggregator_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PendingExit> pendingBySlot_;
};

}  // namespace event

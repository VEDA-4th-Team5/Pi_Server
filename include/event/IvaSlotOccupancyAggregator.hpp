#pragma once

#include "parking/ParkingSlotConfig.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace event {

struct IvaSlotOccupancyUpdate {
    std::string slotId;
    bool occupied{false};
    std::size_t activeAreaCount{0};
    std::size_t knownAreaCount{0};
    std::size_t configuredAreaCount{0};
};

/**
 * @brief 같은 주차면을 관찰하는 여러 IVA 영역을 OR 조건으로 집계한다.
 *
 * 하나라도 활성 상태면 OCCUPIED다. VACANT는 해당 슬롯에 설정된 모든 영역의
 * 상태를 한 번 이상 수신했고 전부 비활성일 때만 반환한다.
 */
class IvaSlotOccupancyAggregator {
public:
    explicit IvaSlotOccupancyAggregator(
        const std::vector<parking::ParkingSlotConfig>& slots);

    [[nodiscard]] std::optional<IvaSlotOccupancyUpdate> update(
        const std::string& slotId,
        const std::string& cameraId,
        const std::string& videoSourceToken,
        const std::string& ruleName,
        bool active);

private:
    struct SlotState {
        std::unordered_set<std::string> configuredAreas;
        std::unordered_map<std::string, bool> observedStates;
        std::optional<bool> lastEmitted;
    };

    [[nodiscard]] static std::string observationKey(
        const std::string& cameraId,
        const std::string& videoSourceToken,
        const std::string& ruleName);

    std::mutex mutex_;
    std::unordered_map<std::string, SlotState> statesBySlot_;
};

}  // namespace event

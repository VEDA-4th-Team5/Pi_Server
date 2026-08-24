#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace device {

/** 주차 도메인 이벤트를 /dev/parking_alert 슬롯 bit 명령으로 투영한다. */
class ParkingAlertController {
public:
    struct Backend {
        std::function<void(std::uint32_t, std::uint64_t)> setSlot;
        std::function<void(std::uint32_t, std::uint64_t)> clearSlot;
        std::function<void()> clearAll;
    };

    ParkingAlertController(std::string slotMap, Backend backend);

    /** 기존 커널 상태를 지운 뒤 DB의 활성 위반 슬롯을 복원한다. */
    bool initialize(
        const std::vector<std::pair<std::string, std::int64_t>>& activeAlerts);

    /** NON_EV/OVERSTAY는 SET, DEPARTURE는 CLEAR로 변환한다. */
    bool handleEvent(std::string_view eventType, std::int64_t sessionId,
                     std::string_view slotId);

    [[nodiscard]] std::size_t mappedSlotCount() const noexcept;

private:
    static std::unordered_map<std::string, std::uint32_t> parseSlotMap(
        const std::string& value);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint32_t> slotMap_;
    Backend backend_;
};

}  // namespace device

#pragma once

#include "app/AppConfig.hpp"
#include "camera/OnvifIvaEventSource.hpp"
#include "event/IvaOccupancyCoordinator.hpp"
#include "parking/ParkingSlotConfig.hpp"

#include <optional>
#include <string>
#include <vector>

namespace event {

/** @brief ONVIF WiseAI 원본 이벤트를 기존 주차 점유 명령으로 변환한다. */
class OnvifIvaEventAdapter {
public:
    [[nodiscard]] static std::optional<IvaOccupancySignal> adapt(
        const camera::OnvifIvaEvent& source,
        const app::AppConfig& config,
        const std::vector<parking::ParkingSlotConfig>& slots,
        std::string* error = nullptr);
};

}  // namespace event

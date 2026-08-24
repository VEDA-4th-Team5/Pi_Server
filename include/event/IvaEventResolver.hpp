#pragma once

#include "app/AppConfig.hpp"
#include "event/CameraEvent.hpp"
#include "parking/ParkingSlotConfig.hpp"

#include <optional>
#include <string>
#include <vector>

namespace event {

/** @brief 카메라 IVA 이벤트를 설정에 등록된 단 하나의 슬롯/ROI로 변환한 결과다. */
struct IvaResolvedTarget {
    std::string slotId;
    std::string channelId;
    std::string ruleName;
    // 점유 집계에는 설정에 등록된 카메라 원본 WiseAI binding token을 사용해
    // Intrusion과 Exit가 같은 영역을 갱신한다.
    std::string observationVideoSourceToken;
    std::string areaName;
    double roiX{};
    double roiY{};
    double roiWidth{1.0};
    double roiHeight{1.0};
};

/**
 * @brief (camera_id, video_source_token, rule_name) 조합으로 IVA 슬롯을 찾는다.
 *
 * 채널 토큰 또는 Rule 이름이 없는 이벤트는 기본 CH1로 추측하지 않는다. 같은 조합이
 * 여러 슬롯에 매핑된 경우에도 잘못된 슬롯을 선택하지 않고 실패한다.
 */
class IvaEventResolver {
public:
    [[nodiscard]] static std::optional<IvaResolvedTarget> resolve(
        const std::string& cameraId,
        const CameraEvent& cameraEvent,
        const std::vector<parking::ParkingSlotConfig>& slots,
        const std::vector<app::IvaAreaConfig>& areas,
        std::string* error = nullptr);

    /**
     * @brief smart-parking-iva-v1 Publication을 선언된 slot/rule/channel로 검증한다.
     *
     * slot_id와 rule_name을 먼저 검증하고 topic token이 해당 슬롯의 논리
     * channel과 일치하는지 확인한 뒤 원본 camera binding token을 반환한다.
     */
    [[nodiscard]] static std::optional<IvaResolvedTarget>
    resolveSmartParkingPublication(
        const std::string& cameraId,
        const CameraEvent& cameraEvent,
        const std::vector<parking::ParkingSlotConfig>& slots,
        const std::vector<app::IvaAreaConfig>& areas,
        std::string* error = nullptr);
};

}  // namespace event

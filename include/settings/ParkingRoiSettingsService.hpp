#pragma once

#include "app/AppConfig.hpp"
#include "parking/AppliedParkingRoi.hpp"
#include "snapshot/NormalizedRoi.hpp"

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace database { class EventDatabase; }

namespace settings {

struct ParkingRoiSetting {
    std::string slotId;
    snapshot::NormalizedRoi roi{};
    std::uint64_t revision{};
};

struct ParkingRoiUpdateResult {
    bool success{};
    bool slotFound{};
    snapshot::NormalizedRoi roi{};
    std::uint64_t revision{};
    std::string error;
};

/** @brief 슬롯별 ROI를 SQLite와 실행 중 메모리에 함께 유지한다. */
class ParkingRoiSettingsService {
public:
    static constexpr const char* kSettingPrefix = "parking_slot_roi.";

    ParkingRoiSettingsService(database::EventDatabase& database,
                              const std::vector<app::IvaAreaConfig>& bootstrap);

    /** @brief DB 저장값을 불러오고, 없으면 AppConfig 좌표를 초기값으로 저장한다. */
    bool initialize();
    [[nodiscard]] std::optional<snapshot::NormalizedRoi> roiForSlot(
        const std::string& slot_id) const;
    [[nodiscard]] std::optional<parking::AppliedParkingRoi> resolveForUse(
        const std::string& slot_id) const;
    [[nodiscard]] std::vector<ParkingRoiSetting> list() const;
    /** @brief DB 저장 성공 후에만 실행 중 좌표를 즉시 교체한다. */
    ParkingRoiUpdateResult update(const std::string& slot_id,
                                  snapshot::NormalizedRoi roi);
    [[nodiscard]] static bool isValid(
        const snapshot::NormalizedRoi& roi) noexcept;

private:
    static std::string settingKey(const std::string& slot_id);
    static std::string serialize(const parking::AppliedParkingRoi& roi);
    static std::optional<parking::AppliedParkingRoi> parse(
        const std::string& slot_id,
        const std::string& value);

    database::EventDatabase& database_;
    std::unordered_set<std::string> known_slots_;
    std::unordered_map<std::string, snapshot::NormalizedRoi> bootstrap_;
    mutable std::shared_mutex roi_mutex_;
    std::unordered_map<std::string, parking::AppliedParkingRoi> rois_;
    std::mutex update_mutex_;
};

}  // namespace settings

#include "settings/ParkingRoiSettingsService.hpp"

#include "database/EventDatabase.hpp"
#include "parking_timer/Types.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace settings {

ParkingRoiSettingsService::ParkingRoiSettingsService(
    database::EventDatabase& database,
    const std::vector<app::IvaAreaConfig>& bootstrap)
    : database_(database) {
    for (const auto& area : bootstrap) {
        known_slots_.insert(area.slot_id);
        if (area.roi_configured) {
            bootstrap_[area.slot_id] = {area.roi_x, area.roi_y,
                                        area.roi_width, area.roi_height};
        }
    }
}

bool ParkingRoiSettingsService::initialize() {
    std::lock_guard update_lock(update_mutex_);
    std::unordered_map<std::string, parking::AppliedParkingRoi> loaded;
    for (const auto& slot_id : known_slots_) {
        const auto fallback = bootstrap_.find(slot_id);
        if (fallback != bootstrap_.end() && !isValid(fallback->second)) {
            util::logError("Invalid bootstrap parking ROI: slot=" + slot_id);
            return false;
        }
        std::optional<parking::AppliedParkingRoi> roi;
        try {
            const auto stored = database_.getSystemSetting(settingKey(slot_id));
            if (stored) {
                const auto parsed = parse(slot_id, *stored);
                if (parsed) {
                    roi = parsed;
                } else {
                    util::logWarn("Invalid stored parking ROI: slot=" +
                                  slot_id);
                }
            }
            if (!roi && fallback != bootstrap_.end()) {
                roi = parking::AppliedParkingRoi{slot_id, fallback->second, 1};
                if (!database_.upsertSystemSetting(settingKey(slot_id),
                                                   serialize(*roi))) {
                    util::logError("Parking ROI persistence failed: slot=" +
                                   slot_id);
                    return false;
                }
            }
        } catch (const std::exception& error) {
            util::logError("Parking ROI load failed: slot=" + slot_id +
                           " error=" + error.what());
            return false;
        }
        if (roi) {
            loaded.emplace(slot_id, *roi);
        } else {
            util::logWarn("Parking ROI is not configured: slot=" + slot_id);
        }
    }
    {
        std::unique_lock roi_lock(roi_mutex_);
        rois_ = std::move(loaded);
    }
    return true;
}

std::optional<snapshot::NormalizedRoi>
ParkingRoiSettingsService::roiForSlot(const std::string& slot_id) const {
    const auto applied = resolveForUse(slot_id);
    if (!applied) return std::nullopt;
    return applied->value;
}

std::optional<parking::AppliedParkingRoi>
ParkingRoiSettingsService::resolveForUse(const std::string& slot_id) const {
    std::shared_lock lock(roi_mutex_);
    const auto found = rois_.find(slot_id);
    if (found == rois_.end()) return std::nullopt;
    return found->second;
}

std::vector<ParkingRoiSetting> ParkingRoiSettingsService::list() const {
    std::shared_lock lock(roi_mutex_);
    std::vector<ParkingRoiSetting> values;
    values.reserve(rois_.size());
    for (const auto& [slot_id, applied] : rois_)
        values.push_back({slot_id, applied.value, applied.revision});
    std::sort(values.begin(), values.end(),
              [](const ParkingRoiSetting& left,
                 const ParkingRoiSetting& right) {
                  return left.slotId < right.slotId;
              });
    return values;
}

ParkingRoiUpdateResult ParkingRoiSettingsService::update(
    const std::string& slot_id, const snapshot::NormalizedRoi roi) {
    std::lock_guard update_lock(update_mutex_);
    if (!known_slots_.contains(slot_id)) {
        return {false, false, {}, 0, "parking slot is not configured"};
    }
    if (!isValid(roi)) {
        return {false, true, {}, 0,
                "ROI must be normalized and remain inside the image"};
    }
    std::uint64_t next_revision = 1;
    {
        std::shared_lock roi_lock(roi_mutex_);
        const auto current = rois_.find(slot_id);
        if (current != rois_.end())
            next_revision = current->second.revision + 1;
    }
    const parking::AppliedParkingRoi applied{slot_id, roi, next_revision};
    if (!database_.upsertSystemSetting(settingKey(slot_id),
                                       serialize(applied))) {
        util::logError("Parking ROI update failed: slot=" + slot_id);
        return {false, true, {}, 0, "failed to persist ROI"};
    }
    {
        std::unique_lock roi_lock(roi_mutex_);
        rois_[slot_id] = applied;
    }

    const std::string changed_at = parking_timer::utcNow();
    std::ostringstream message;
    message << std::fixed << std::setprecision(6)
            << "x=" << roi.x << " y=" << roi.y
            << " width=" << roi.width << " height=" << roi.height
            << " revision=" << next_revision
            << " changed_at=" << changed_at << " success=true";
    database_.insertSystemEvent("PARKING_ROI_UPDATED", slot_id, message.str());
    util::logInfo("Parking ROI updated: slot=" + slot_id + " " +
                  message.str());
    return {true, true, roi, next_revision, {}};
}

bool ParkingRoiSettingsService::isValid(
    const snapshot::NormalizedRoi& roi) noexcept {
    return std::isfinite(roi.x) && std::isfinite(roi.y) &&
           std::isfinite(roi.width) && std::isfinite(roi.height) &&
           roi.x >= 0.0 && roi.y >= 0.0 && roi.width > 0.0 &&
           roi.height > 0.0 && roi.x < 1.0 && roi.y < 1.0 &&
           roi.x + roi.width <= 1.0 && roi.y + roi.height <= 1.0;
}

std::string ParkingRoiSettingsService::settingKey(
    const std::string& slot_id) {
    return std::string(kSettingPrefix) + slot_id;
}

std::string ParkingRoiSettingsService::serialize(
    const parking::AppliedParkingRoi& applied) {
    const auto& roi = applied.value;
    std::ostringstream output;
    output << std::setprecision(17) << roi.x << ',' << roi.y << ','
           << roi.width << ',' << roi.height << ',' << applied.revision;
    return output.str();
}

std::optional<parking::AppliedParkingRoi> ParkingRoiSettingsService::parse(
    const std::string& slot_id,
    const std::string& value) {
    std::istringstream input(value);
    snapshot::NormalizedRoi roi{};
    char separator1{}, separator2{}, separator3{};
    if (!(input >> roi.x >> separator1 >> roi.y >> separator2 >> roi.width >>
          separator3 >> roi.height) || separator1 != ',' || separator2 != ',' ||
        separator3 != ',') {
        return std::nullopt;
    }
    std::uint64_t revision = 1;
    input >> std::ws;
    if (!input.eof()) {
        char separator4{};
        if (!(input >> separator4 >> revision) || separator4 != ',' ||
            revision == 0) {
            return std::nullopt;
        }
        input >> std::ws;
    }
    if (!input.eof() || !isValid(roi)) return std::nullopt;
    return parking::AppliedParkingRoi{slot_id, roi, revision};
}

}  // namespace settings

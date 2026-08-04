#include "settings/OverstayThresholdService.hpp"

#include "database/EventDatabase.hpp"
#include "parking_timer/Types.hpp"
#include "util/Logger.hpp"

#include <sstream>
#include <utility>

namespace settings {

OverstayThresholdService::OverstayThresholdService(
    database::EventDatabase& database, const int bootstrap_seconds)
    : database_(database),
      bootstrap_seconds_(isValid(bootstrap_seconds)
          ? bootstrap_seconds : kDefaultSeconds),
      threshold_seconds_(bootstrap_seconds_) {}

bool OverstayThresholdService::initialize() {
    std::lock_guard lock(update_mutex_);
    try {
        const auto stored = database_.getSystemSetting(kSettingKey);
        if (stored.has_value()) {
            std::size_t consumed{};
            const int value = std::stoi(*stored, &consumed);
            if (consumed == stored->size() && isValid(value)) {
                threshold_seconds_.store(value, std::memory_order_release);
                return true;
            }
            util::logWarn("Invalid stored overstay threshold; restoring default");
        }
    } catch (const std::exception& error) {
        util::logError("Overstay threshold load failed: " +
                       std::string(error.what()));
        return false;
    }
    if (!database_.upsertSystemSetting(
            kSettingKey, std::to_string(bootstrap_seconds_))) {
        util::logError("Overstay threshold default persistence failed");
        return false;
    }
    threshold_seconds_.store(bootstrap_seconds_, std::memory_order_release);
    return true;
}

int OverstayThresholdService::thresholdSeconds() const noexcept {
    return threshold_seconds_.load(std::memory_order_acquire);
}

bool OverstayThresholdService::isValid(const int seconds) noexcept {
    return seconds >= kMinimumSeconds && seconds <= kMaximumSeconds;
}

ThresholdUpdateResult OverstayThresholdService::update(const int seconds) {
    std::lock_guard lock(update_mutex_);
    const int previous = thresholdSeconds();
    if (!isValid(seconds)) {
        return {false, previous, previous,
                "thresholdSeconds must be between 60 and 86400"};
    }
    if (seconds == previous) return {true, previous, previous, {}};
    if (!database_.upsertSystemSetting(kSettingKey, std::to_string(seconds))) {
        util::logError("Overstay threshold update failed: old=" +
                       std::to_string(previous) + " requested=" +
                       std::to_string(seconds));
        return {false, previous, previous, "failed to persist setting"};
    }

    threshold_seconds_.store(seconds, std::memory_order_release);
    if (apply_callback_) {
        try {
            apply_callback_(std::chrono::seconds(seconds));
        } catch (const std::exception& error) {
            util::logError("Overstay threshold runtime apply failed: " +
                           std::string(error.what()));
        } catch (...) {
            util::logError("Overstay threshold runtime apply failed");
        }
    }

    const std::string changed_at = parking_timer::utcNow();
    std::ostringstream message;
    message << "old_seconds=" << previous << " new_seconds=" << seconds
            << " changed_at=" << changed_at << " success=true";
    database_.insertSystemEvent("OVERSTAY_THRESHOLD_UPDATED", {}, message.str());
    util::logInfo("Overstay threshold updated: old=" +
                  std::to_string(previous) + " new=" +
                  std::to_string(seconds) + " changed_at=" + changed_at);
    return {true, previous, seconds, {}};
}

void OverstayThresholdService::setApplyCallback(ApplyCallback callback) {
    std::lock_guard lock(update_mutex_);
    apply_callback_ = std::move(callback);
}

}  // namespace settings

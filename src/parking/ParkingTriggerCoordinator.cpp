#include "parking/ParkingTriggerCoordinator.hpp"

#include "database/EventDatabase.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace parking {

ParkingTriggerCoordinator::ParkingTriggerCoordinator(
    database::EventDatabase& database,
    const int correlation_window_ms,
    Clock clock)
    : database_(database),
      correlation_window_(correlation_window_ms > 0
                              ? correlation_window_ms
                              : 8000),
      clock_(std::move(clock)) {
    if (!clock_) clock_ = &ParkingTriggerCoordinator::systemNowEpochMs;
}

ParkingCorrelationMatch ParkingTriggerCoordinator::resolve(
    const std::string& camera_id,
    const std::string& channel_id,
    const std::string& object_id) const {
    if (camera_id.empty() || channel_id.empty() || object_id.empty()) {
        return {};
    }
    return database_.resolveParkingCorrelation(
        camera_id, channel_id, object_id, nowEpochMs());
}

BestShotAttachResult ParkingTriggerCoordinator::attachBestShotIfActive(
    const CommittedCorrelationLease& lease,
    const BestShotEvidenceKind kind,
    const std::string& image_ref,
    const std::string& image_path,
    const std::string& plate_text) {
    if (lease.correlationId.empty() || lease.occupancyAttemptId.empty() ||
        lease.sessionId < 0 || lease.slotId.empty() || image_ref.empty() ||
        image_path.empty()) {
        return {BestShotAttachCode::Conflict, -1,
                "incomplete committed correlation lease or evidence identity"};
    }
    return database_.attachBestShotIfActive(
        lease, kind, image_ref, image_path, plate_text, nowEpochMs());
}

std::int64_t ParkingTriggerCoordinator::nowEpochMs() const {
    return clock_();
}

std::chrono::milliseconds
ParkingTriggerCoordinator::correlationWindow() const noexcept {
    return correlation_window_;
}

std::int64_t ParkingTriggerCoordinator::systemNowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace parking

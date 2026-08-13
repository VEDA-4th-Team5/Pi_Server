#pragma once

#include "parking/ParkingCorrelation.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace database {
class EventDatabase;
}

namespace parking {

/**
 * Thin, fail-closed facade over the durable correlation store.
 *
 * This class deliberately owns no slot/channel/session maps.  A lookup is
 * resolved from SQLite on every use and an evidence attachment is authorized
 * again by the SQLite transaction.  Consequently an in-memory result can
 * never substitute for the active-session predicate at attachment time.
 */
class ParkingTriggerCoordinator {
public:
    using Clock = std::function<std::int64_t()>;

    explicit ParkingTriggerCoordinator(
        database::EventDatabase& database,
        int correlation_window_ms = 8000,
        Clock clock = {});

    [[nodiscard]] ParkingCorrelationMatch resolve(
        const std::string& camera_id,
        const std::string& channel_id,
        const std::string& object_id) const;

    [[nodiscard]] BestShotAttachResult attachBestShotIfActive(
        const CommittedCorrelationLease& lease,
        BestShotEvidenceKind kind,
        const std::string& image_ref,
        const std::string& image_path,
        const std::string& plate_text = {});

    [[nodiscard]] std::int64_t nowEpochMs() const;
    [[nodiscard]] std::chrono::milliseconds correlationWindow() const noexcept;

private:
    static std::int64_t systemNowEpochMs();

    database::EventDatabase& database_;
    std::chrono::milliseconds correlation_window_;
    Clock clock_;
};

}  // namespace parking

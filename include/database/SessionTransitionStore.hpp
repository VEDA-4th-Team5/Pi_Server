#pragma once

#include "parking/SlotTransitionTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace database {

class EventDatabase;

/**
 * Narrow durable boundary for slot-command admission and exact session
 * transitions.  The actor is the scheduler; this store owns only SQLite
 * transactions and idempotent replay identities.
 */
class SessionTransitionStore {
public:
    explicit SessionTransitionStore(EventDatabase& database) noexcept;
    [[nodiscard]] bool ready() const noexcept;

    parking::SlotAdmissionResult admit(
        const parking::SlotTransitionCommand& command,
        std::size_t pendingCapacity);
    std::size_t admitDueDeadlines(
        std::int64_t nowEpochMs,
        std::size_t pendingCapacity,
        const std::vector<std::string>& blockedSlots = {});
    std::vector<parking::DurableSlotCommand> listRunnable(
        std::int64_t nowEpochMs) const;
    parking::CommittedOccupancyTransition apply(
        const std::string& commandId);
    std::vector<parking::CommittedOccupancyTransition> listPendingEffects(
        std::int64_t nowEpochMs) const;
    bool completeEffects(const std::string& commandId);
    bool deferEffects(const std::string& commandId,
                      std::int64_t nextAttemptAtEpochMs,
                      const std::string& error) noexcept;
    bool defer(const std::string& commandId,
               std::int64_t nextAttemptAtEpochMs,
               const std::string& error) noexcept;
    [[nodiscard]] std::optional<std::int64_t> nextDeadlineEpochMs() const;
    [[nodiscard]] std::size_t pendingCount() const;
    [[nodiscard]] std::size_t pendingEffectCount() const;
    [[nodiscard]] std::size_t pendingDrainCount(
        std::int64_t shutdownCutoffEpochMs) const;
    /** @brief 보존 기간이 지난 종결 INBOX 행을 한 배치 삭제하고 건수를 돌려준다. */
    std::size_t purgeSettled(std::int64_t createdBeforeEpochMs,
                             std::size_t batchLimit) noexcept;

private:
    EventDatabase& database_;
};

}  // namespace database

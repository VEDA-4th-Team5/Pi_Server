#include "database/SessionTransitionStore.hpp"

#include "database/EventDatabase.hpp"

namespace database {

SessionTransitionStore::SessionTransitionStore(
    EventDatabase& database) noexcept
    : database_(database) {}

bool SessionTransitionStore::ready() const noexcept {
    return database_.occupancySchemaReady();
}

parking::SlotAdmissionResult SessionTransitionStore::admit(
    const parking::SlotTransitionCommand& command,
    const std::size_t pendingCapacity) {
    return database_.admitSlotTransitionCommand(command, pendingCapacity);
}

std::size_t SessionTransitionStore::admitDueDeadlines(
    const std::int64_t nowEpochMs,
    const std::size_t pendingCapacity,
    const std::vector<std::string>& blockedSlots) {
    return database_.admitDueSlotDeadlines(
        nowEpochMs, pendingCapacity, blockedSlots);
}

std::vector<parking::DurableSlotCommand>
SessionTransitionStore::listRunnable(const std::int64_t nowEpochMs) const {
    return database_.listRunnableSlotTransitionCommands(nowEpochMs);
}

parking::CommittedOccupancyTransition SessionTransitionStore::apply(
    const std::string& commandId) {
    return database_.applySlotTransitionCommand(commandId);
}

std::vector<parking::CommittedOccupancyTransition>
SessionTransitionStore::listPendingEffects(
    const std::int64_t nowEpochMs) const {
    return database_.listPendingSlotTransitionEffects(nowEpochMs);
}

bool SessionTransitionStore::completeEffects(
    const std::string& commandId) {
    return database_.completeSlotTransitionEffects(commandId);
}

bool SessionTransitionStore::deferEffects(
    const std::string& commandId,
    const std::int64_t nextAttemptAtEpochMs,
    const std::string& error) noexcept {
    return database_.deferSlotTransitionEffects(
        commandId, nextAttemptAtEpochMs, error);
}

bool SessionTransitionStore::defer(
    const std::string& commandId,
    const std::int64_t nextAttemptAtEpochMs,
    const std::string& error) noexcept {
    return database_.deferSlotTransitionCommand(
        commandId, nextAttemptAtEpochMs, error);
}

std::optional<std::int64_t>
SessionTransitionStore::nextDeadlineEpochMs() const {
    return database_.nextScheduledSlotDeadlineEpochMs();
}

std::size_t SessionTransitionStore::pendingCount() const {
    return database_.pendingSlotTransitionCommandCount();
}

std::size_t SessionTransitionStore::pendingEffectCount() const {
    return database_.pendingSlotTransitionEffectCount();
}

std::size_t SessionTransitionStore::purgeSettled(
    const std::int64_t createdBeforeEpochMs,
    const std::size_t batchLimit) noexcept {
    return database_.purgeSettledSlotTransitionCommands(
        createdBeforeEpochMs, batchLimit);
}

std::size_t SessionTransitionStore::pendingDrainCount(
    const std::int64_t shutdownCutoffEpochMs) const {
    return database_.pendingSlotTransitionDrainCount(shutdownCutoffEpochMs);
}

}  // namespace database

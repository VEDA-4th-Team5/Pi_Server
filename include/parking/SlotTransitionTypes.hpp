#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace parking {

enum class SlotCommandKind { HallObservation, CameraObservation, ExitDeadline };

enum class SlotCommandStatus {
    PendingUnprepared,
    PendingPrepared,
    Applied,
    RejectedInvalid
};

struct SlotTransitionCommand {
    std::string commandId;
    SlotCommandKind kind{SlotCommandKind::HallObservation};
    std::string slotId;
    std::string sensorId;
    std::string sourceIdentity;
    std::optional<std::uint64_t> sourceSequence;
    std::string occurredAt;
    std::string payloadJson;
    std::int64_t dueAtEpochMs{};
};

struct DurableSlotCommand {
    SlotTransitionCommand command;
    std::int64_t admissionOrdinal{};
    SlotCommandStatus status{SlotCommandStatus::PendingUnprepared};
    std::string occupancyAttemptId;
    std::string correlationId;
    std::uint64_t observationGeneration{};
    std::string deadlineId;
    std::optional<std::int64_t> expectedSessionId;
    int attemptCount{};
    std::int64_t nextAttemptAtEpochMs{};
    std::string lastError;
};

enum class SlotAdmissionCode {
    Admitted,
    Existing,
    AlreadyTerminal,
    RejectedStale,
    Conflict,
    CapacityFull,
    RetryableFailure
};

struct SlotAdmissionResult {
    SlotAdmissionCode code{SlotAdmissionCode::RetryableFailure};
    std::string commandId;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == SlotAdmissionCode::Admitted ||
               code == SlotAdmissionCode::Existing ||
               code == SlotAdmissionCode::AlreadyTerminal;
    }
};

enum class CommittedOccupancyCode {
    SessionStarted,
    SessionEnded,
    NoChange,
    ExitScheduled,
    AlreadyApplied,
    RejectedInvalidTimestamp,
    StaleGeneration,
    RetryableFailure
};

struct CommittedOccupancyTransition {
    CommittedOccupancyCode code{CommittedOccupancyCode::RetryableFailure};
    std::string commandId;
    SlotCommandKind sourceKind{SlotCommandKind::HallObservation};
    std::string slotId;
    std::string sensorId;
    std::int64_t sessionId{-1};
    std::string occupancyAttemptId;
    std::string correlationId;
    std::uint64_t observationGeneration{};
    std::string occurredAt;
    std::int64_t occurredAtEpochMs{};
    std::string sourceTransport;
    std::string message;

    [[nodiscard]] bool terminal() const noexcept {
        return code != CommittedOccupancyCode::RetryableFailure;
    }
};

struct OccupancyDeadlineRecord {
    std::string deadlineId;
    std::string slotId;
    std::string occupancyAttemptId;
    std::int64_t expectedSessionId{-1};
    std::uint64_t observationGeneration{};
    std::int64_t dueAtEpochMs{};
};

[[nodiscard]] const char* toString(SlotCommandKind value) noexcept;
[[nodiscard]] const char* toString(SlotCommandStatus value) noexcept;
[[nodiscard]] std::optional<SlotCommandKind> slotCommandKindFromString(
    const std::string& value) noexcept;
[[nodiscard]] std::optional<SlotCommandStatus> slotCommandStatusFromString(
    const std::string& value) noexcept;
[[nodiscard]] bool sameSlotSourceFact(
    const SlotTransitionCommand& lhs,
    const SlotTransitionCommand& rhs) noexcept;

}  // namespace parking

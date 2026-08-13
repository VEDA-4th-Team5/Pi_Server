#include "parking/SlotTransitionTypes.hpp"

#include <nlohmann/json.hpp>

namespace parking {

const char* toString(const SlotCommandKind value) noexcept {
    switch (value) {
    case SlotCommandKind::HallObservation:
        return "HALL_OBSERVATION";
    case SlotCommandKind::CameraObservation:
        return "CAMERA_OBSERVATION";
    case SlotCommandKind::ExitDeadline:
        return "EXIT_DEADLINE";
    }
    return "UNKNOWN";
}

const char* toString(const SlotCommandStatus value) noexcept {
    switch (value) {
    case SlotCommandStatus::PendingUnprepared:
        return "PENDING_UNPREPARED";
    case SlotCommandStatus::PendingPrepared:
        return "PENDING_PREPARED";
    case SlotCommandStatus::Applied:
        return "APPLIED";
    case SlotCommandStatus::RejectedInvalid:
        return "REJECTED_INVALID";
    }
    return "UNKNOWN";
}

std::optional<SlotCommandKind> slotCommandKindFromString(
    const std::string& value) noexcept {
    if (value == "HALL_OBSERVATION")
        return SlotCommandKind::HallObservation;
    if (value == "CAMERA_OBSERVATION")
        return SlotCommandKind::CameraObservation;
    if (value == "EXIT_DEADLINE")
        return SlotCommandKind::ExitDeadline;
    return std::nullopt;
}

std::optional<SlotCommandStatus> slotCommandStatusFromString(
    const std::string& value) noexcept {
    if (value == "PENDING_UNPREPARED")
        return SlotCommandStatus::PendingUnprepared;
    if (value == "PENDING_PREPARED")
        return SlotCommandStatus::PendingPrepared;
    if (value == "APPLIED") return SlotCommandStatus::Applied;
    if (value == "REJECTED_INVALID")
        return SlotCommandStatus::RejectedInvalid;
    return std::nullopt;
}

bool sameSlotSourceFact(const SlotTransitionCommand& lhs,
                        const SlotTransitionCommand& rhs) noexcept {
    if (lhs.commandId != rhs.commandId || lhs.kind != rhs.kind ||
        lhs.slotId != rhs.slotId || lhs.sensorId != rhs.sensorId ||
        lhs.sourceIdentity != rhs.sourceIdentity ||
        lhs.sourceSequence != rhs.sourceSequence) {
        return false;
    }
    try {
        auto left = nlohmann::json::parse(lhs.payloadJson);
        auto right = nlohmann::json::parse(rhs.payloadJson);
        if (lhs.kind == SlotCommandKind::HallObservation) {
            // Sequenced UART frames do not carry source time. Receive time and
            // confirmation due time belong to the first admission, not to the
            // physical fact used for redelivery identity.
            left.erase("occurred_at_epoch_ms");
            right.erase("occurred_at_epoch_ms");
            return left == right;
        }
        if (lhs.kind == SlotCommandKind::CameraObservation) {
            // Camera UtcTime is authoritative and remains in the payload. Only
            // the local receive-time deadline is volatile across QoS replay.
            left.erase("deadline_due_at_epoch_ms");
            right.erase("deadline_due_at_epoch_ms");
            return lhs.occurredAt == rhs.occurredAt && left == right;
        }
        return lhs.occurredAt == rhs.occurredAt &&
            lhs.dueAtEpochMs == rhs.dueAtEpochMs && left == right;
    } catch (...) {
        return false;
    }
}

}  // namespace parking

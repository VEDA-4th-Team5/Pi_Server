#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace parking {

enum class ParkingCorrelationState {
    Pending,
    Committed,
    Ended,
    Failed,
    Expired,
    Quarantined
};

struct CommittedCorrelationLease {
    std::string correlationId;
    std::string occupancyAttemptId;
    std::int64_t sessionId{-1};
    std::string slotId;
    std::string cameraId;
    std::string videoSourceToken;
    std::string ruleName;
    std::string objectId;
    std::string channelId;
    std::uint64_t bindingRevision{};
    std::int64_t expiresAtEpochMs{};
};

enum class ParkingCorrelationMatchKind {
    Unmatched,
    Pending,
    Unique,
    Ambiguous
};

struct ParkingCorrelationMatch {
    ParkingCorrelationMatchKind kind{ParkingCorrelationMatchKind::Unmatched};
    std::optional<CommittedCorrelationLease> lease;
    std::vector<std::string> candidateCorrelationIds;
};

enum class BestShotEvidenceKind {
    Vehicle,
    Plate
};

enum class BestShotAttachCode {
    Attached,
    AlreadyAttached,
    Unmatched,
    Inactive,
    Expired,
    Conflict,
    RetryableFailure
};

struct BestShotAttachResult {
    BestShotAttachCode code{BestShotAttachCode::RetryableFailure};
    std::int64_t imageId{-1};
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == BestShotAttachCode::Attached ||
               code == BestShotAttachCode::AlreadyAttached;
    }
};

[[nodiscard]] const char* toString(BestShotEvidenceKind kind) noexcept;

}  // namespace parking

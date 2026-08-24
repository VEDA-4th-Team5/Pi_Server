#pragma once

#include "parking/ParkingCorrelation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bestshot {

enum class BestShotKind { Vehicle, Plate };

struct BestShotMetadataEvent {
    std::string cameraId;
    std::string channelId;
    std::string objectId;
    std::string imageRef;
    std::string plateText;
    std::string rtspUrl;
    BestShotKind kind{BestShotKind::Vehicle};
    std::int64_t receivedAtEpochMs{};
};

enum class BestShotProcessCode {
    RoutedExternally,
    Attached,
    AlreadyAttached,
    DuplicateInFlight,
    Pending,
    Ambiguous,
    Unmatched,
    DownloadFailed,
    AttachRejected,
    ExpiredPending,
    InvalidMetadata
};

struct BestShotProcessResult {
    BestShotProcessCode code{BestShotProcessCode::InvalidMetadata};
    parking::BestShotEvidenceKind kind{
        parking::BestShotEvidenceKind::Vehicle};
    parking::BestShotAttachCode attachCode{
        parking::BestShotAttachCode::Unmatched};
    std::string evidenceIdentity;
    std::string cameraId;
    std::string channelId;
    std::string objectId;
    std::string imageRef;
    std::string correlationId;
    std::int64_t sessionId{-1};
    std::string imagePath;
    std::string message;
    std::vector<std::string> candidateCorrelationIds;
};

}  // namespace bestshot

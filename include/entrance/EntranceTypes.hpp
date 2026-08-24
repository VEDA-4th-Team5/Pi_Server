#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace entrance {

enum class BestShotKind { Vehicle, Plate };

enum class ObjectState {
    EvQueued,
    EvProcessing,
    OcrQueued,
    OcrProcessing,
    Finalizing,
    Completed,
    Failed,
    Duplicate
};

struct ObjectKey {
    std::string cameraId;
    std::string channelId;
    std::string objectId;

    bool operator==(const ObjectKey&) const = default;
    [[nodiscard]] std::string text() const {
        return cameraId + "|" + channelId + "|" + objectId;
    }
};

struct ObjectKeyHash {
    std::size_t operator()(const ObjectKey& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.cameraId);
        seed ^= std::hash<std::string>{}(key.channelId) +
                0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::string>{}(key.objectId) +
                0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct BestShotEvent {
    ObjectKey key;
    BestShotKind kind{BestShotKind::Vehicle};
    std::string imageRef;
    std::string rtspUrl;
    std::int64_t receivedAtEpochMs{};
};

enum class ObserveCode {
    EvQueued,
    Ignored,
    DuplicateSuppressed,
    Invalid,
    CapacityRejected
};

struct ObserveResult {
    ObserveCode code{ObserveCode::Invalid};
    ObjectKey key;
    ObjectState state{ObjectState::EvQueued};
    bool newObject{};
    std::size_t duplicateCount{};
};

struct ExpiredObject {
    ObjectKey key;
    std::int64_t eventId{-1};
    std::size_t duplicateCount{};
    std::string reason;
};

}  // namespace entrance

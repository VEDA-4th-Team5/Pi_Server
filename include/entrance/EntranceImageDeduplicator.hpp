#pragma once

#include "entrance/EntranceTypes.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace entrance {

struct ImageDedupResult {
    bool fingerprinted{};
    bool duplicate{};
    ObjectKey canonicalKey;
    int hammingDistance{-1};
    std::size_t duplicateCount{};
    std::optional<std::int64_t> canonicalEventId;
};

/**
 * Plate BestShot의 perceptual hash를 짧게 보관해 카메라가 다른 ObjectId를
 * 부여한 동일 이미지를 EV 분석과 Gemini OCR 전에 차단한다.
 */
class EntranceImageDeduplicator {
public:
    EntranceImageDeduplicator(std::chrono::milliseconds window,
                              int hammingThreshold,
                              std::size_t capacity);

    ImageDedupResult observe(const ObjectKey& key,
                             const std::string& imagePath,
                             std::int64_t receivedAtEpochMs);
    bool bindEventId(const ObjectKey& key, std::int64_t eventId);
    [[nodiscard]] std::size_t trackedCount() const;

private:
    struct Record {
        ObjectKey key;
        std::array<std::uint8_t, 8> hash{};
        std::int64_t lastSeenEpochMs{};
        std::int64_t eventId{-1};
        std::size_t duplicateCount{};
    };

    static std::optional<std::array<std::uint8_t, 8>> fingerprint(
        const std::string& imagePath);
    static int hammingDistance(const std::array<std::uint8_t, 8>& left,
                               const std::array<std::uint8_t, 8>& right);
    void eraseExpiredLocked(std::int64_t nowEpochMs);

    const std::int64_t windowMs_;
    const int hammingThreshold_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Record> records_;
};

}  // namespace entrance

#include "entrance/EntranceImageDeduplicator.hpp"

#include <opencv2/img_hash.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace entrance {

EntranceImageDeduplicator::EntranceImageDeduplicator(
    const std::chrono::milliseconds window,
    const int hammingThreshold,
    const std::size_t capacity)
    : windowMs_(std::max<std::int64_t>(1, window.count())),
      hammingThreshold_(std::clamp(hammingThreshold, 0, 64)),
      capacity_(std::max<std::size_t>(1, capacity)) {}

ImageDedupResult EntranceImageDeduplicator::observe(
    const ObjectKey& key,
    const std::string& imagePath,
    const std::int64_t receivedAtEpochMs) {
    ImageDedupResult result;
    result.canonicalKey = key;
    const auto hash = fingerprint(imagePath);
    if (!hash.has_value()) return result;
    result.fingerprinted = true;

    std::lock_guard lock(mutex_);
    eraseExpiredLocked(receivedAtEpochMs);

    auto best = records_.end();
    int bestDistance = std::numeric_limits<int>::max();
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->key.cameraId != key.cameraId ||
            it->key.channelId != key.channelId) {
            continue;
        }
        const int distance = hammingDistance(it->hash, *hash);
        if (distance < bestDistance) {
            best = it;
            bestDistance = distance;
        }
    }

    if (best != records_.end() && bestDistance <= hammingThreshold_) {
        best->lastSeenEpochMs = receivedAtEpochMs;
        ++best->duplicateCount;
        result.duplicate = true;
        result.canonicalKey = best->key;
        result.hammingDistance = bestDistance;
        result.duplicateCount = best->duplicateCount;
        if (best->eventId >= 0) result.canonicalEventId = best->eventId;
        return result;
    }

    if (records_.size() >= capacity_) {
        const auto oldest = std::min_element(
            records_.begin(), records_.end(),
            [](const Record& left, const Record& right) {
                return left.lastSeenEpochMs < right.lastSeenEpochMs;
            });
        if (oldest != records_.end()) records_.erase(oldest);
    }
    records_.push_back({key, *hash, receivedAtEpochMs, -1, 0});
    return result;
}

bool EntranceImageDeduplicator::bindEventId(const ObjectKey& key,
                                             const std::int64_t eventId) {
    if (eventId < 0) return false;
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(
        records_.begin(), records_.end(),
        [&key](const Record& record) { return record.key == key; });
    if (found == records_.end()) return false;
    if (found->eventId >= 0) return found->eventId == eventId;
    found->eventId = eventId;
    return true;
}

std::size_t EntranceImageDeduplicator::trackedCount() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

std::optional<std::array<std::uint8_t, 8>>
EntranceImageDeduplicator::fingerprint(const std::string& imagePath) {
    try {
        const cv::Mat image = cv::imread(imagePath, cv::IMREAD_GRAYSCALE);
        if (image.empty()) return std::nullopt;
        cv::Mat hash;
        cv::img_hash::pHash(image, hash);
        if (hash.empty() || hash.type() != CV_8U || hash.total() != 8)
            return std::nullopt;
        if (!hash.isContinuous()) hash = hash.clone();
        std::array<std::uint8_t, 8> bytes{};
        std::memcpy(bytes.data(), hash.data, bytes.size());
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

int EntranceImageDeduplicator::hammingDistance(
    const std::array<std::uint8_t, 8>& left,
    const std::array<std::uint8_t, 8>& right) {
    int distance{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        distance += std::popcount(
            static_cast<unsigned int>(left[index] ^ right[index]));
    }
    return distance;
}

void EntranceImageDeduplicator::eraseExpiredLocked(
    const std::int64_t nowEpochMs) {
    std::erase_if(records_, [this, nowEpochMs](const Record& record) {
        return nowEpochMs - record.lastSeenEpochMs >= windowMs_;
    });
}

}  // namespace entrance

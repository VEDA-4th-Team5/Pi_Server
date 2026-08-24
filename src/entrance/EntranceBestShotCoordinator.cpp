#include "entrance/EntranceBestShotCoordinator.hpp"

#include <algorithm>

namespace entrance {

EntranceBestShotCoordinator::EntranceBestShotCoordinator(
    const std::chrono::milliseconds objectTtl,
    const std::size_t capacity)
    : objectTtlMs_(std::max<std::int64_t>(1, objectTtl.count())),
      capacity_(std::max<std::size_t>(1, capacity)) {}

ObserveResult EntranceBestShotCoordinator::observe(
    const BestShotEvent& event) {
    ObserveResult result;
    result.key = event.key;
    // 입구 판정은 Plate BestShot 하나만으로 시작한다. Vehicle 메타데이터는
    // 같은 객체를 기다리게 만들지 않고 상위 수신 경로에서 소비한다.
    if (event.kind != BestShotKind::Plate) {
        result.code = ObserveCode::Ignored;
        return result;
    }
    if (event.key.cameraId.empty() || event.key.channelId.empty() ||
        event.key.objectId.empty() || event.imageRef.empty() ||
        event.receivedAtEpochMs <= 0) {
        return result;
    }

    std::lock_guard lock(mutex_);
    eraseExpiredTerminalLocked(event.receivedAtEpochMs);
    auto found = entries_.find(event.key);
    if (found == entries_.end()) {
        if (entries_.size() >= capacity_) {
            result.code = ObserveCode::CapacityRejected;
            return result;
        }
        Entry entry;
        entry.firstSeenEpochMs = event.receivedAtEpochMs;
        entry.lastUpdatedEpochMs = event.receivedAtEpochMs;
        entry.state = ObjectState::EvQueued;
        found = entries_.emplace(event.key, entry).first;
        result.newObject = true;
        result.code = ObserveCode::EvQueued;
        result.state = entry.state;
        return result;
    }

    Entry& entry = found->second;
    entry.lastUpdatedEpochMs = event.receivedAtEpochMs;
    ++entry.duplicateCount;
    result.code = ObserveCode::DuplicateSuppressed;
    result.state = entry.state;
    result.duplicateCount = entry.duplicateCount;
    return result;
}

bool EntranceBestShotCoordinator::bindEventId(const ObjectKey& key,
                                               const std::int64_t eventId) {
    if (eventId < 0) return false;
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return false;
    if (found->second.eventId >= 0) return found->second.eventId == eventId;
    found->second.eventId = eventId;
    return true;
}

std::optional<std::int64_t> EntranceBestShotCoordinator::eventId(
    const ObjectKey& key) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second.eventId < 0)
        return std::nullopt;
    return found->second.eventId;
}

bool EntranceBestShotCoordinator::markEvProcessing(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::EvQueued) return false;
    found->second.state = ObjectState::EvProcessing;
    return true;
}

bool EntranceBestShotCoordinator::markOcrQueued(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::EvProcessing) return false;
    found->second.state = ObjectState::OcrQueued;
    return true;
}

bool EntranceBestShotCoordinator::markOcrProcessing(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::OcrQueued) return false;
    found->second.state = ObjectState::OcrProcessing;
    return true;
}

bool EntranceBestShotCoordinator::beginFinalization(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::OcrProcessing) return false;
    found->second.state = ObjectState::Finalizing;
    return true;
}

bool EntranceBestShotCoordinator::complete(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::Finalizing) return false;
    found->second.state = ObjectState::Completed;
    return true;
}

bool EntranceBestShotCoordinator::fail(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state == ObjectState::Completed ||
        found->second.state == ObjectState::Failed) return false;
    found->second.state = ObjectState::Failed;
    return true;
}

bool EntranceBestShotCoordinator::markImageDuplicate(const ObjectKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() ||
        found->second.state != ObjectState::EvQueued) return false;
    found->second.state = ObjectState::Duplicate;
    return true;
}

std::vector<ExpiredObject> EntranceBestShotCoordinator::sweep(
    const std::int64_t nowEpochMs) {
    std::vector<ExpiredObject> expired;
    std::lock_guard lock(mutex_);
    for (auto& [key, entry] : entries_) {
        const bool active = entry.state == ObjectState::EvQueued ||
                            entry.state == ObjectState::EvProcessing ||
                            entry.state == ObjectState::OcrQueued ||
                            entry.state == ObjectState::OcrProcessing;
        if (active && nowEpochMs - entry.firstSeenEpochMs >= objectTtlMs_) {
            entry.state = ObjectState::Failed;
            entry.lastUpdatedEpochMs = nowEpochMs;
            expired.push_back({key, entry.eventId, entry.duplicateCount,
                               "entrance plate processing TTL expired"});
        }
    }
    eraseExpiredTerminalLocked(nowEpochMs);
    return expired;
}

std::size_t EntranceBestShotCoordinator::trackedCount() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::optional<ObjectState> EntranceBestShotCoordinator::state(
    const ObjectKey& key) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return std::nullopt;
    return found->second.state;
}

std::size_t EntranceBestShotCoordinator::duplicateCount(
    const ObjectKey& key) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    return found == entries_.end() ? 0 : found->second.duplicateCount;
}

void EntranceBestShotCoordinator::eraseExpiredTerminalLocked(
    const std::int64_t nowEpochMs) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const bool terminal = it->second.state == ObjectState::Completed ||
                              it->second.state == ObjectState::Failed ||
                              it->second.state == ObjectState::Duplicate;
        if (terminal && nowEpochMs - it->second.lastUpdatedEpochMs >=
                            objectTtlMs_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace entrance

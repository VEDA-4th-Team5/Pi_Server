#pragma once

#include "entrance/EntranceTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace entrance {

/**
 * 입구 Plate BestShot 객체의 수명과 EV 판정/OCR 단일 실행 권한을 관리한다.
 * 네트워크, 파일, SQLite 작업은 하지 않아 동시성 정책을 독립적으로 검증할 수 있다.
 */
class EntranceBestShotCoordinator {
public:
    EntranceBestShotCoordinator(std::chrono::milliseconds objectTtl,
                                std::size_t capacity);

    ObserveResult observe(const BestShotEvent& event);
    bool bindEventId(const ObjectKey& key, std::int64_t eventId);
    [[nodiscard]] std::optional<std::int64_t> eventId(
        const ObjectKey& key) const;
    bool markEvProcessing(const ObjectKey& key);
    bool markOcrQueued(const ObjectKey& key);
    bool markOcrProcessing(const ObjectKey& key);
    bool beginFinalization(const ObjectKey& key);
    bool complete(const ObjectKey& key);
    bool fail(const ObjectKey& key);
    bool markImageDuplicate(const ObjectKey& key);
    std::vector<ExpiredObject> sweep(std::int64_t nowEpochMs);

    [[nodiscard]] std::size_t trackedCount() const;
    [[nodiscard]] std::optional<ObjectState> state(
        const ObjectKey& key) const;
    [[nodiscard]] std::size_t duplicateCount(const ObjectKey& key) const;

private:
    struct Entry {
        ObjectState state{ObjectState::EvQueued};
        std::int64_t eventId{-1};
        std::int64_t firstSeenEpochMs{};
        std::int64_t lastUpdatedEpochMs{};
        std::size_t duplicateCount{};
    };

    void eraseExpiredTerminalLocked(std::int64_t nowEpochMs);

    const std::int64_t objectTtlMs_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<ObjectKey, Entry, ObjectKeyHash> entries_;
};

}  // namespace entrance

#pragma once

#include "parking_timer/EventDatabase.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace parking_timer {

/** @brief deadline을 정상 반영한 뒤 외부 publisher에 전달하는 이벤트다. */
struct ViolationEvent {
    std::int64_t log_id{};
    std::string slot_id;
    std::string car_number;
    std::string violation_at;
    std::string image_path_2;
};

/** @brief worker에서 격리한 DB/callback 오류와 관련 세션 문맥이다. */
struct TimerError {
    std::int64_t log_id{};
    std::string slot_id;
    std::string car_number;
    std::string message;
};

/** @brief 최소 우선순위 큐와 worker thread로 장기 점유 deadline을 관리한다. */
class TimerManager {
public:
    using ViolationCallback = std::function<void(const ViolationEvent&)>;
    using ErrorCallback = std::function<void(const TimerError&)>;
    using EvidenceProvider = std::function<std::string(
        std::int64_t, const std::string&, const std::string&)>;

    TimerManager(EventDatabase& database,
                 ViolationCallback callback,
                 ErrorCallback error_callback = {},
                 std::mutex* transition_mutex = nullptr,
                 EvidenceProvider evidence_provider = {});
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    /** @brief 불변 session ID의 위반 deadline을 큐에 등록하고 worker를 깨운다. */
    void schedule(std::int64_t log_id,
                  std::string slot_id,
                  std::string car_number,
                  std::chrono::milliseconds delay);

    /** @brief 아직 worker가 소비하지 않은 큐 항목 수를 반환한다. */
    std::size_t pendingCount() const;

private:
    using Clock = std::chrono::steady_clock;

    struct TimerItem {
        Clock::time_point deadline;
        std::uint64_t sequence{};
        std::int64_t log_id{};
        std::string slot_id;
        std::string car_number;
        std::uint32_t retry_count{};
        // 최초 steady_clock deadline 도달 때 캡처하며 DB retry에서도 보존한다.
        std::string violation_at;
    };

    struct LaterDeadline {
        bool operator()(const TimerItem& left, const TimerItem& right) const noexcept;
    };

    void run();
    void processExpired(TimerItem item);
    void retryAfterDatabaseError(TimerItem item, std::string message) noexcept;
    void reportError(const TimerItem& item, std::string message) noexcept;

    EventDatabase& database_;
    ViolationCallback callback_;
    ErrorCallback error_callback_;
    EvidenceProvider evidence_provider_;
    std::mutex* transition_mutex_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<TimerItem, std::vector<TimerItem>, LaterDeadline> queue_;
    bool stopping_{};
    std::uint64_t next_sequence_{};
    // 반드시 모든 worker 상태 멤버보다 뒤에 선언하고 ctor body에서 시작한다.
    std::thread worker_;
};

}  // namespace parking_timer

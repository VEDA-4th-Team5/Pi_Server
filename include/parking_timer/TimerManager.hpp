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

struct ViolationEvent {
    std::int64_t log_id{};
    int zone_id{};
    std::string car_number;
    std::string violation_at;
    std::string image_path_2;
};

struct TimerError {
    std::int64_t log_id{};
    int zone_id{};
    std::string car_number;
    std::string message;
};

class TimerManager {
public:
    using ViolationCallback = std::function<void(const ViolationEvent&)>;
    using ErrorCallback = std::function<void(const TimerError&)>;

    TimerManager(EventDatabase& database,
                 ViolationCallback callback,
                 ErrorCallback error_callback = {},
                 std::mutex* transition_mutex = nullptr);
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    void schedule(std::int64_t log_id,
                  int zone_id,
                  std::string car_number,
                  std::chrono::milliseconds delay);

    std::size_t pendingCount() const;

private:
    using Clock = std::chrono::steady_clock;

    struct TimerItem {
        Clock::time_point deadline;
        std::uint64_t sequence{};
        std::int64_t log_id{};
        int zone_id{};
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

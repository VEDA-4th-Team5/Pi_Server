#pragma once

#include "parking_timer/EventDatabase.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/TimerManager.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace parking_timer {

class ParkingSlotManager {
public:
    ParkingSlotManager(EventDatabase& database,
                       EventManager& events,
                       std::chrono::milliseconds parking_timeout);

    EntryResult handleEntry(int zone_id,
                            const std::string& car_number,
                            const std::string& image_path_1 = {});

    std::optional<LogRecord> handleExit(int zone_id);
    std::size_t pendingTimerCount() const;

private:
    EventDatabase& database_;
    EventManager& events_;
    std::chrono::milliseconds parking_timeout_;
    // 입차/출차/만료의 DB 변경과 이벤트 발행 순서를 직렬화한다.
    std::mutex transition_mutex_;
    TimerManager timers_;
};

}  // namespace parking_timer

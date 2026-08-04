#pragma once

#include "parking_timer/EventDatabase.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/TimerManager.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>

namespace parking_timer {

/** @brief 차량 분류·DB 세션·장기 점유 TimerManager를 조정하는 application service다. */
class ParkingSlotManager {
public:
    ParkingSlotManager(EventDatabase& database,
                       EventManager& events,
                       std::chrono::milliseconds parking_timeout,
                       TimerManager::EvidenceProvider evidence_provider = {});

    /** @brief EV/PHEV 입차만 세션과 타이머로 등록하고 중복 입차를 거부한다. */
    EntryResult handleEntry(const std::string& slot_id,
                            const std::string& car_number,
                            const std::string& image_path_1 = {});

    /** @brief 카메라 흐름이 이미 만든 세션을 중복 INSERT 없이 타이머에 등록한다. */
    EntryResult handleRecognizedSession(std::int64_t session_id,
                                        const std::string& slot_id,
                                        const std::string& car_number);

    /** @brief 서버 재시작 시 DB의 EV/PHEV 활성 세션을 타이머 큐에 복구한다. */
    std::size_t restoreActiveSessions();

    /** @brief 활성 세션을 출차 처리하며 없으면 nullopt를 반환한다. */
    std::optional<LogRecord> handleExit(const std::string& slot_id);
    /** @brief lazy-canceled 항목을 포함한 현재 우선순위 큐 크기를 반환한다. */
    std::size_t pendingTimerCount() const;

private:
    EventDatabase& database_;
    EventManager& events_;
    std::chrono::milliseconds parking_timeout_;
    // 입차/출차/만료의 DB 변경과 이벤트 발행 순서를 직렬화한다.
    std::mutex transition_mutex_;
    std::unordered_set<std::int64_t> scheduled_session_ids_;
    TimerManager timers_;
};

}  // namespace parking_timer

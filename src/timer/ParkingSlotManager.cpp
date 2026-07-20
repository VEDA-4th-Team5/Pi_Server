#include "parking_timer/ParkingSlotManager.hpp"

#include "parking_timer/Types.hpp"

#include <stdexcept>
#include <utility>

namespace parking_timer {

/**
 * @brief 입·출차 상태 전이와 EV 점유 타이머를 조정하는 관리자를 생성한다.
 *
 * @param[in,out] database 차량 조회와 세션 로그를 저장할 SQLite 접근 객체.
 * @param[in,out] events 관제 이벤트를 발행할 이벤트 관리자.
 * @param[in] parking_timeout EV/PHEV 장기점유 판정까지의 대기시간.
 * @throws std::invalid_argument 제한시간이 0 이하인 경우.
 * @throws std::system_error 타이머 worker 스레드를 생성할 수 없는 경우.
 * @note 타이머 callback과 출차 처리는 같은 `transition_mutex_`를 사용하므로 DB 상태와
 *       이벤트 발행 순서가 서로 뒤집히지 않는다.
 */
ParkingSlotManager::ParkingSlotManager(EventDatabase& database,
                                       EventManager& events,
                                       const std::chrono::milliseconds parking_timeout)
    : database_(database),
      events_(events),
      parking_timeout_(parking_timeout),
      timers_(database_, [this](const ViolationEvent& event) {
          // 현재는 JSON stdout이지만 향후 MQTT/HTTP publisher로 교체할 통합 지점이다.
          events_.publish("VIOLATION_TRIGGERED", event.zone_id, event.car_number,
                          event.violation_at, event.image_path_2);
      }, [this](const TimerError& error) {
          // worker 오류도 정상 이벤트와 같은 JSON 경로로 보내 관제 측에서 확인하게 한다.
          events_.publish("TIMER_ERROR", error.zone_id, error.car_number, utcNow(),
                          error.message);
      }, &transition_mutex_) {
    if (parking_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("parking timeout must be positive");
    }
}

/**
 * @brief 홀센서 OFF→ON과 번호판 인식 결과에 해당하는 입차 이벤트를 처리한다.
 *
 * @param[in] zone_id 차량이 들어온 충전구역 ID. 1 이상의 값이어야 한다.
 * @param[in] car_number 차량 마스터에서 조회할 번호판 문자열.
 * @param[in] image_path_1 최초 주차 증거 이미지 경로. 비어 있으면 기본 경로를 만든다.
 * @return 타이머 등록 여부, 차량 분류, 생성된 로그 ID와 설명을 담은 결과.
 * @throws std::invalid_argument 구역 ID가 잘못됐거나 차량번호가 비어 있는 경우.
 * @throws std::runtime_error SQLite 처리 또는 타이머 등록에 실패한 경우.
 */
EntryResult ParkingSlotManager::handleEntry(const int zone_id,
                                            const std::string& car_number,
                                            const std::string& image_path_1) {
    if (zone_id <= 0) {
        throw std::invalid_argument("zone_id must be greater than zero");
    }
    if (car_number.empty()) {
        throw std::invalid_argument("car_number must not be empty");
    }

    // 입차가 만료/출차 처리와 섞여 같은 구역에 모순된 상태를 만들지 않도록 직렬화한다.
    std::lock_guard transition_lock(transition_mutex_);
    const auto category = database_.classifyVehicle(car_number);
    const auto now = utcNow();
    // 미등록 차량은 정책이 정해지지 않았으므로 자동 타이머 대신 관리자 확인으로 보낸다.
    if (category == VehicleCategory::Unknown) {
        events_.publish("UNKNOWN_VEHICLE", zone_id, car_number, now,
                        "manual confirmation required");
        return {false, category, std::nullopt, "vehicle is not registered"};
    }
    if (category == VehicleCategory::NonEv) {
        events_.publish("NON_EV_ALERT", zone_id, car_number, now,
                        "not scheduled in the EV overtime timer");
        return {false, category, std::nullopt, "non-EV vehicle"};
    }

    // 부분 unique index에 맡기기 전에 의미 있는 이벤트와 오류 메시지를 제공한다.
    if (database_.findActiveByZone(zone_id).has_value()) {
        events_.publish("ENTRY_REJECTED", zone_id, car_number, now,
                        "zone already has an active session");
        return {false, category, std::nullopt, "zone is already occupied"};
    }

    const auto first_image = image_path_1.empty()
                                 ? "snapshots/" + car_number + "_parked.jpg"
                                 : image_path_1;
    // DB 세션 ID를 먼저 얻어 큐 노드가 zone_id가 아닌 불변 log_id를 참조하게 한다.
    const auto log_id = database_.insertParked(car_number, zone_id, now, first_image);
    try {
        timers_.schedule(log_id, zone_id, car_number, parking_timeout_);
    } catch (...) {
        // INSERT 뒤 메모리 큐 등록이 실패하면 활성 PARKED 고아 행이 남지 않도록
        // 보상 UPDATE를 최선 노력으로 수행한 후 원래 오류를 전달한다.
        try {
            database_.cancelUnscheduled(log_id, utcNow());
        } catch (...) {
        }
        throw;
    }
    events_.publish("ARRIVAL", zone_id, car_number, now,
                    "timer_log inserted; overtime timer started");
    return {true, category, log_id, "timer started"};
}

/**
 * @brief 홀센서 ON→OFF에 해당하는 출차 이벤트를 처리한다.
 *
 * @param[in] zone_id 차량이 빠져나온 충전구역 ID.
 * @return 갱신된 로그. 활성 세션이 없으면 `std::nullopt`.
 * @throws std::invalid_argument 구역 ID가 0 이하인 경우.
 * @throws std::runtime_error SQLite 트랜잭션 처리에 실패한 경우.
 * @note 우선순위 큐에서 임의 원소를 제거하지 않고 DB의 `is_canceled`를 표시한다.
 */
std::optional<LogRecord> ParkingSlotManager::handleExit(const int zone_id) {
    if (zone_id <= 0) {
        throw std::invalid_argument("zone_id must be greater than zero");
    }

    // 만료 worker와 같은 mutex를 사용한다. 먼저 DB를 바꾼 전이가 승리한다.
    std::lock_guard transition_lock(transition_mutex_);
    const auto now = utcNow();
    auto record = database_.departActiveByZone(zone_id, now);
    if (!record.has_value()) {
        events_.publish("EXIT_IGNORED", zone_id, "", now, "no active session");
        return std::nullopt;
    }

    events_.publish("DEPARTURE", zone_id, record->car_number, now,
                    "timer_log updated; queued timer lazily canceled");
    return record;
}

/**
 * @brief 큐에 남아 있는 타이머 노드 수를 조회한다.
 *
 * @return 만료 전 활성 노드와 아직 top에 오지 않은 lazy-canceled 노드의 합계.
 */
std::size_t ParkingSlotManager::pendingTimerCount() const {
    return timers_.pendingCount();
}

}  // namespace parking_timer

#include "parking_timer/EventDatabase.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "parking_timer/TimerManager.hpp"
#include "parking_timer/Types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef PARKING_TIMER_TEST_SQL_DIR
#error PARKING_TIMER_TEST_SQL_DIR must be defined
#endif

namespace {

using namespace std::chrono_literals;
using parking_timer::EventDatabase;

/**
 * @brief 테스트 조건을 검사하고 실패를 예외로 보고한다.
 *
 * @param[in] condition 통과해야 하는 조건.
 * @param[in] message 실패 시 표시할 진단 메시지.
 * @throws std::runtime_error 조건이 `false`인 경우.
 */
void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/**
 * @brief 테스트별로 충돌 가능성이 낮은 임시 SQLite 경로를 만든다.
 *
 * @param[in] label 파일명에서 테스트를 구분할 짧은 라벨.
 * @return 시스템 임시 디렉터리 아래의 고유 DB 파일 경로.
 */
std::filesystem::path temporaryDatabase(const std::string& label) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("parking_timer_" + label + "_" + std::to_string(unique) + ".sqlite3");
}

/**
 * @brief 테스트 SQLite 본 파일과 WAL 보조 파일을 최선 노력으로 제거한다.
 *
 * @param[in] database 제거할 DB 본 파일 경로.
 * @note cleanup 실패가 원래 테스트 결과를 가리지 않도록 오류 코드는 의도적으로 무시한다.
 */
void removeDatabaseFiles(const std::filesystem::path& database) {
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.string() + "-wal", ignored);
    std::filesystem::remove(database.string() + "-shm", ignored);
}

/**
 * @brief 비동기 조건이 참이 될 때까지 짧은 간격으로 기다린다.
 *
 * @tparam Predicate 인자 없이 호출해 bool로 평가할 수 있는 조건 함수 타입.
 * @param[in] predicate 완료 여부를 검사할 함수 객체.
 * @param[in] timeout 기다릴 최대 시간.
 * @return 제한시간 안 또는 마지막 검사에서 조건이 참이면 `true`.
 */
template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/**
 * @brief 빌드 디렉터리에 복사된 schema와 seed로 테스트 DB를 초기화한다.
 *
 * @param[in,out] database 초기화할 열린 DB.
 * @throws std::runtime_error SQL 파일 읽기 또는 실행이 실패한 경우.
 */
void initialize(EventDatabase& database) {
    const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");
}

/**
 * @brief 차량 분류부터 입차·만료·출차·lazy deletion까지 전체 수명주기를 검증한다.
 *
 * @throws std::runtime_error 기대한 분류나 DB 상태 전이가 하나라도 맞지 않는 경우.
 */
void testEntryViolationAndExit() {
    const auto path = temporaryDatabase("lifecycle");
    {
        EventDatabase database(path);
        initialize(database);
        require(database.classifyVehicle("123가4567") ==
                    parking_timer::VehicleCategory::Ev,
                "EV seed classification failed");
        require(database.classifyVehicle("234나5678") ==
                    parking_timer::VehicleCategory::Phev,
                "PHEV seed classification failed");
        require(database.classifyVehicle("345다6789") ==
                    parking_timer::VehicleCategory::NonEv,
                "non-EV seed classification failed");
        require(database.classifyVehicle("999호9999") ==
                    parking_timer::VehicleCategory::Unknown,
                "unknown classification failed");

        parking_timer::EventManager events;
        parking_timer::ParkingSlotManager slots(database, events, 120ms);

        // 정상 EV는 PARKED 한 행을 만들고 타이머 큐에 등록돼야 한다.
        const auto entry = slots.handleEntry("EV01", "123가4567");
        require(entry.accepted && entry.log_id.has_value(), "EV entry was not accepted");
        const auto parked = database.findLogById(*entry.log_id);
        require(parked.has_value() && parked->slot_id == "EV01",
                "EV01 slot_id was not preserved in PARKING_SESSION");
        require(parked->status == "PARKED",
                "entry did not INSERT a PARKED row");

        // 중복 구역, 일반차, 미등록 차량은 EV 점유 타이머에 들어가면 안 된다.
        const auto duplicate = slots.handleEntry("EV01", "234나5678");
        require(!duplicate.accepted, "duplicate active slot was accepted");
        require(!slots.handleEntry("EV03", "345다6789").accepted,
                "non-EV was scheduled");
        require(!slots.handleEntry("EV04", "999호9999").accepted,
                "unknown vehicle was scheduled");

        // worker가 실제로 deadline에 깨어 VIOLATION을 기록할 때까지 기다린다.
        require(waitUntil(
                    [&] {
                        const auto log = database.findLogById(*entry.log_id);
                        return log.has_value() && log->status == "VIOLATION";
                    },
                    1s),
                "EV timer did not expire");
        const auto violated = database.findLogById(*entry.log_id);
        require(violated->violation_at.has_value(), "violation_at was not recorded");
        require(violated->image_path_2.has_value(), "second image path was not recorded");

        const auto departed = slots.handleExit("EV01");
        require(departed.has_value() && departed->status == "DEPARTS",
                "exit did not UPDATE status to DEPARTS");
        require(departed->departed_at.has_value() && departed->is_canceled,
                "exit did not set departed_at/is_canceled");
        require(departed->violation_at.has_value(),
                "exit erased the earlier violation timestamp");

        // PHEV를 즉시 출차시켜 큐에 남은 노드가 만료 시 조용히 폐기되는지 검증한다.
        const auto early_entry = slots.handleEntry("EV02", "234나5678");
        require(early_entry.accepted, "PHEV entry was not accepted");
        const auto early_departure = slots.handleExit("EV02");
        require(early_departure.has_value(), "early exit failed");
        std::this_thread::sleep_for(180ms);
        const auto canceled = database.findLogById(*early_entry.log_id);
        require(canceled->status == "DEPARTS" && !canceled->violation_at.has_value(),
                "lazy-canceled timer became a violation");
        require(slots.pendingTimerCount() == 0,
                "expired lazy-canceled node was not removed from the queue");
        require(database.listLogs().size() == 2, "unexpected timer_log row count");
    }
    removeDatabaseFiles(path);
}

/**
 * @brief 기존 대기보다 빠른 새 deadline이 worker를 깨우고 먼저 처리되는지 검증한다.
 *
 * @throws std::runtime_error callback이 오지 않거나 최소 힙 순서가 잘못된 경우.
 */
void testEarlierDeadlineWakesWorker() {
    const auto path = temporaryDatabase("priority");
    {
        EventDatabase database(path);
        initialize(database);
        const auto first_id = database.insertParked(
            "123가4567", "EV01", parking_timer::utcNow(), "first.jpg");
        const auto second_id = database.insertParked(
            "234나5678", "EV02", parking_timer::utcNow(), "second.jpg");

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<std::int64_t> expired_order;
        parking_timer::TimerManager timers(
            database, [&](const parking_timer::ViolationEvent& event) {
                {
                    std::lock_guard lock(mutex);
                    expired_order.push_back(event.log_id);
                }
                condition.notify_one();
            });

        // worker가 300ms를 기다리기 시작한 뒤 50ms 타이머를 넣어 notify/re-wait 경로를 탄다.
        timers.schedule(first_id, "EV01", "123가4567", 300ms);
        std::this_thread::sleep_for(20ms);
        timers.schedule(second_id, "EV02", "234나5678", 50ms);

        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 500ms,
                                   [&] { return !expired_order.empty(); }),
                "priority queue worker did not wake for an earlier timer");
        require(expired_order.front() == second_id,
                "min-priority queue expired the later deadline first");
    }
    removeDatabaseFiles(path);
}

/**
 * @brief 위반 publisher callback 예외가 worker 밖으로 전파되지 않는지 검증한다.
 *
 * @throws std::runtime_error 오류 callback이 호출되지 않거나 DB 위반 기록이 사라진 경우.
 */
void testWorkerContainsCallbackExceptions() {
    const auto path = temporaryDatabase("callback_error");
    {
        EventDatabase database(path);
        initialize(database);
        const auto log_id = database.insertParked(
            "123가4567", "EV01", parking_timer::utcNow(), "callback.jpg");

        std::mutex mutex;
        std::condition_variable condition;
        bool error_reported{};
        parking_timer::TimerManager timers(
            database,
            [](const parking_timer::ViolationEvent&) {
                throw std::runtime_error("simulated publisher failure");
            },
            [&](const parking_timer::TimerError& error) {
                {
                    std::lock_guard lock(mutex);
                    error_reported = error.message == "simulated publisher failure";
                }
                condition.notify_one();
            });

        timers.schedule(log_id, "EV01", "123가4567", 20ms);
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 500ms, [&] { return error_reported; }),
                "worker did not contain/report a callback exception");
        lock.unlock();
        require(database.findLogById(log_id)->status == "VIOLATION",
                "publisher failure rolled back a committed violation");
    }
    removeDatabaseFiles(path);
}

}  // namespace

/**
 * @brief 모든 타이머 단위/통합 테스트를 실행한다.
 *
 * @return 모든 테스트 통과 시 0, 첫 실패를 포착하면 1.
 */
int main() {
    try {
        testEntryViolationAndExit();
        testEarlierDeadlineWakesWorker();
        testWorkerContainsCallbackExceptions();
        std::cout << "All parking timer tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}

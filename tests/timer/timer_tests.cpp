#include "parking_timer/EventDatabase.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "parking_timer/TimerManager.hpp"
#include "parking_timer/Types.hpp"

#include <atomic>
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
                    parking_timer::VehicleCategory::Ev,
                "legacy PHEV seed was not folded into EV");
        require(database.classifyVehicle("345다6789") ==
                    parking_timer::VehicleCategory::NonEv,
                "non-EV seed classification failed");
        require(database.classifyVehicle("999호9999") ==
                    parking_timer::VehicleCategory::Unknown,
                "unknown classification failed");

        parking_timer::EventManager events;
        parking_timer::ParkingSlotManager slots(database, events, 120ms);
        require(slots.start(), "parking timer worker did not start");

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

        // 기존 PHEV 등록 차량도 EV로 흡수된 뒤 같은 타이머 정책을 사용한다.
        const auto early_entry = slots.handleEntry("EV02", "234나5678");
        require(early_entry.accepted, "folded EV entry was not accepted");
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

void testExistingCameraSessionScheduling() {
    const auto path = temporaryDatabase("existing_session");
    {
        EventDatabase database(path);
        initialize(database);
        int session_id = -1;
        require(database.createEntryWithBestShot(
                    "EV01", "camera_vehicle.jpg", "object-1", &session_id),
                "camera session setup failed");
        database.applyPlateOcr(session_id, "EV01", "camera_vehicle.jpg",
                               "123가4567", 0.95);

        parking_timer::EventManager events;
        parking_timer::ParkingSlotManager slots(database, events, 5s);
        require(slots.start(), "parking timer worker did not start");
        const auto scheduled = slots.handleRecognizedSession(
            session_id, "EV01", "123가4567");
        require(scheduled.accepted && scheduled.log_id == session_id,
                "existing camera session was not scheduled");
        require(slots.pendingTimerCount() == 1,
                "existing session did not create exactly one timer");
        const auto duplicate = slots.handleRecognizedSession(
            session_id, "EV01", "123가4567");
        require(!duplicate.accepted && slots.pendingTimerCount() == 1,
                "duplicate OCR result scheduled a second timer");
        int folded_ev_session_id = -1;
        require(database.createEntryWithBestShot(
                    "EV02", "camera_folded_ev.jpg", "object-2",
                    &folded_ev_session_id),
                "folded EV camera session setup failed");
        const auto folded_ev_classification = database.applyPlateOcr(
            folded_ev_session_id, "EV02", "camera_folded_ev.jpg",
            "234나5678", 0.93);
        require(folded_ev_classification == "EV",
                "OCR DB result did not fold the legacy PHEV into EV");
        require(slots.handleRecognizedSession(
                    folded_ev_session_id, "EV02", "234나5678").accepted,
                "existing folded EV camera session was not scheduled");
        require(slots.pendingTimerCount() == 2,
                "both EV timers were not retained");
        require(database.listLogs().size() == 2,
                "timer integration inserted a duplicate parking session");
    }
    removeDatabaseFiles(path);
}

/**
 * @brief runtime guard 이전에는 timer 작업을 받지 않고 explicit start가 멱등인지 검증한다.
 *
 * @throws std::runtime_error 시작 전 작업이 수락되거나 worker 시작에 실패한 경우.
 */
void testTimerRequiresExplicitStart() {
    const auto path = temporaryDatabase("explicit_start");
    {
        EventDatabase database(path);
        initialize(database);
        const auto log_id = database.insertParked(
            "123가4567", "EV01", parking_timer::utcNow(), "entry.jpg");
        parking_timer::TimerManager timers(
            database, [](const parking_timer::ViolationEvent&) {});
        bool rejected{};
        try {
            timers.schedule(log_id, "EV01", "123가4567", 1s);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected,
                "timer accepted work before the runtime guard could start it");
        require(timers.start() && timers.start(),
                "timer explicit start was not successful and idempotent");
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
        require(timers.start(), "timer worker did not start");

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
        require(timers.start(), "timer worker did not start");

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

void testSnapshotFailureStillMarksViolation() {
    const auto path = temporaryDatabase("snapshot_error");
    {
        EventDatabase database(path);
        initialize(database);
        const auto log_id = database.insertParked(
            "123가4567", "EV01", parking_timer::utcNow(), "entry.jpg");
        std::mutex mutex;
        bool snapshot_error_reported{};
        parking_timer::TimerManager timers(
            database,
            [](const parking_timer::ViolationEvent&) {},
            [&](const parking_timer::TimerError& error) {
                std::lock_guard lock(mutex);
                snapshot_error_reported =
                    error.message.find("violation Snapshot failed") != std::string::npos;
            },
            nullptr,
            [](std::int64_t, const std::string&, const std::string&) -> std::string {
                throw std::runtime_error("simulated camera failure");
            });
        require(timers.start(), "timer worker did not start");
        timers.schedule(log_id, "EV01", "123가4567", 20ms);
        require(waitUntil([&] {
                    const auto record = database.findLogById(log_id);
                    return record && record->status == "VIOLATION";
                }, 500ms),
                "Snapshot failure prevented the DB violation transition");
        std::lock_guard lock(mutex);
        require(snapshot_error_reported, "Snapshot failure was not reported");
    }
    removeDatabaseFiles(path);
}

void testPendingEvidenceRetriesBeforeViolation() {
    const auto path = temporaryDatabase("evidence_pending");
    {
        EventDatabase database(path);
        initialize(database);
        const auto log_id = database.insertParked(
            "123가4567", "EV01", parking_timer::utcNow(), "entry.jpg");
        std::atomic<int> provider_calls{};
        std::mutex mutex;
        std::condition_variable condition;
        std::string violation_image;
        parking_timer::TimerManager timers(
            database,
            [&](const parking_timer::ViolationEvent& event) {
                {
                    std::lock_guard lock(mutex);
                    violation_image = event.image_path_2;
                }
                condition.notify_one();
            }, {}, nullptr,
            [&](std::int64_t, const std::string&,
                const std::string&) -> std::string {
                return provider_calls.fetch_add(1) == 0
                    ? std::string{} : "overstay-restored.jpg";
            });
        require(timers.start(), "timer worker did not start");
        timers.schedule(log_id, "EV01", "123가4567", 20ms);
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 1500ms,
                                   [&] { return !violation_image.empty(); }),
                "pending evidence was not retried before violation");
        require(violation_image == "overstay-restored.jpg",
                "violation used an empty or unexpected evidence path");
        require(provider_calls.load() >= 2,
                "evidence provider was not called again");
    }
    removeDatabaseFiles(path);
}

void testActiveSessionThresholdReschedule() {
    const auto path = temporaryDatabase("threshold_reschedule");
    {
        EventDatabase database(path);
        initialize(database);
        parking_timer::EventManager events;
        parking_timer::ParkingSlotManager slots(database, events, 500ms);
        require(slots.start(), "parking timer worker did not start");
        const auto entry = slots.handleEntry("EV01", "123가4567");
        require(entry.accepted && entry.log_id.has_value(),
                "reschedule fixture entry failed");
        std::this_thread::sleep_for(30ms);
        require(slots.updateParkingTimeout(120ms) == 1,
                "active session was not rescheduled");
        require(slots.pendingTimerCount() == 1,
                "old timer generation remained logically active");
        require(waitUntil([&] {
                    const auto record = database.findLogById(*entry.log_id);
                    return record && record->status == "VIOLATION";
                }, 500ms),
                "shortened threshold did not expire active session");

        const auto second = slots.handleEntry("EV02", "234나5678");
        require(second.accepted && second.log_id.has_value(),
                "raised threshold fixture entry failed");
        require(slots.updateParkingTimeout(350ms) == 1,
                "raised threshold was not applied");
        std::this_thread::sleep_for(170ms);
        require(database.findLogById(*second.log_id)->status == "PARKED",
                "stale earlier timer violated session after threshold increase");
        require(waitUntil([&] {
                    const auto record = database.findLogById(*second.log_id);
                    return record && record->status == "VIOLATION";
                }, 500ms),
                "raised threshold replacement never expired");
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
        testExistingCameraSessionScheduling();
        testTimerRequiresExplicitStart();
        testEarlierDeadlineWakesWorker();
        testWorkerContainsCallbackExceptions();
        testSnapshotFailureStillMarksViolation();
        testPendingEvidenceRetriesBeforeViolation();
        testActiveSessionThresholdReschedule();
        std::cout << "All parking timer tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}

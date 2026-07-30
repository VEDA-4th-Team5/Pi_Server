/** @file HallTimerIntegrationTest.cpp @brief 가짜 프레임과 센서로 실제 서버 주차 흐름을 검증한다. */
#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/SystemEventReporter.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "sensor/HallParkingService.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>
#include <sqlite3.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
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

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryFiles {
    std::filesystem::path database;
    std::filesystem::path snapshots;
    ~TemporaryFiles() {
        std::error_code ignored;
        std::filesystem::remove(database, ignored);
        std::filesystem::remove(database.string() + "-wal", ignored);
        std::filesystem::remove(database.string() + "-shm", ignored);
        std::filesystem::remove_all(snapshots, ignored);
    }
};

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

int countEventType(const std::filesystem::path& database_path,
                   const std::string& event_type) {
    sqlite3* connection = nullptr;
    if (sqlite3_open_v2(database_path.c_str(), &connection,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (connection != nullptr) sqlite3_close(connection);
        return -1;
    }
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT COUNT(*) FROM EVENT_LOG WHERE event_type = ?;";
    int count = -1;
    if (sqlite3_prepare_v2(connection, sql, -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_bind_text(statement, 1, event_type.c_str(), -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        count = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    return count;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("hall_flow_" + std::to_string(unique));
    TemporaryFiles temporary{root / "parking.sqlite3", root / "snapshots"};

    try {
        require(argc >= 2, "parking slot fixture path is required");
        auto configs = parking::ParkingSlotConfigLoader::loadFromFile(argv[1]);

        // bounded queue 정책은 같은 슬롯의 최신 상태를 병합하고, 다른 슬롯이
        // capacity를 넘을 때만 거부해야 한다.
        sensor::HallParkingWorkQueue bounded_queue(1);
        sensor::HallParkingWorkItem occupied;
        occupied.event.slotId = "EV01";
        occupied.event.state = parking::ParkingSensorState::Occupied;
        require(bounded_queue.push(occupied) ==
                    sensor::HallParkingWorkQueue::PushResult::Added,
                "first hall work item was not queued");
        auto vacant = occupied;
        vacant.event.state = parking::ParkingSensorState::Vacant;
        require(bounded_queue.push(vacant) ==
                    sensor::HallParkingWorkQueue::PushResult::Coalesced &&
                    bounded_queue.size() == 1,
                "same-slot latest state was not coalesced");
        auto other_slot = occupied;
        other_slot.event.slotId = "EV02";
        require(bounded_queue.push(other_slot) ==
                    sensor::HallParkingWorkQueue::PushResult::Full &&
                    bounded_queue.size() == bounded_queue.capacity(),
                "distinct-slot overflow exceeded bounded capacity");
        const auto latest = bounded_queue.pop();
        require(latest && latest->event.state ==
                              parking::ParkingSensorState::Vacant,
                "coalesced queue did not retain the latest VACANT state");

        database::EventDatabase database(temporary.database);
        const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");

        app::AppConfig app_config{};
        app_config.parking_occupancy_confirm_ms = 40;
        app_config.iva_areas.push_back(
            {"EV01", "EV01", "ch01", 0.0, 0.0, 1.0, 1.0});
        auto channel = std::make_shared<camera::CameraChannel>();
        channel->camera_id = "mock-camera";
        channel->channel_id = "ch01";
        channel->latest_full_frame = cv::Mat(
            720, 1280, CV_8UC3, cv::Scalar(20, 80, 160));
        std::vector<std::shared_ptr<camera::CameraChannel>> channels{channel};
        std::atomic<bool> running{true};
        snapshot::SnapshotStorage snapshots(
            temporary.snapshots.string(), 100, running);

        parking_timer::EventManager events;
        std::mutex event_mutex;
        std::vector<std::string> published_events;
        events.setPublisher([&](std::string_view type, std::int64_t,
                                std::string_view, std::string_view,
                                std::string_view, std::string_view) {
            std::lock_guard lock(event_mutex);
            published_events.emplace_back(type);
        });
        parking_timer::ParkingSlotManager timer_manager(
            database, events, 80ms,
            [&](std::int64_t session_id, const std::string&,
                const std::string&) {
                return database.findEvidenceImagePath(
                           session_id, "OVERSTAY_EVIDENCE")
                    .value_or("");
            });

        event::SystemEventReporter::Config reporter_config;
        reporter_config.duplicate_window = 1s;
        reporter_config.sink_retry_delay = 10ms;
        event::SystemEventReporter system_events(
            [&](const event::SystemEvent& system_event,
                const std::string& message) {
                return database.insertSystemEvent(
                    event::toString(system_event.code), system_event.slot_id,
                    message);
            }, reporter_config);
        require(system_events.start(), "system event reporter did not start");
        event::SystemEvent overflow_event;
        overflow_event.source = event::SystemEventSource::HallSensor;
        overflow_event.code = event::SystemEventCode::HallWorkQueueOverflow;
        overflow_event.severity = event::SystemEventSeverity::Error;
        overflow_event.slot_id = "EV02";
        overflow_event.transport = "test";
        overflow_event.message = "bounded hall work queue full";
        system_events.report(std::move(overflow_event));
        require(waitUntil([&] {
                    return countEventType(
                               temporary.database,
                               "HALL_WORK_QUEUE_OVERFLOW") == 1;
                }, 1s),
                "hall queue overflow was not persisted to EVENT_LOG");

        std::atomic<int> enqueued_session{-1};
        std::atomic<int> canceled_session{-1};
        std::mutex transition_mutex;
        std::vector<parking::ParkingTransitionResult> capture_transitions;
        parking::EvidenceCaptureWorker::Config evidence_config;
        evidence_config.overstayDelay = 80ms;
        parking::EvidenceCaptureWorker evidence_worker(
            snapshots, database, evidence_config,
            [&](const parking::EvidenceCaptureResult& result) {
                if (result.stored &&
                    result.reason == parking::EvidenceReason::OccupancyStart) {
                    enqueued_session.store(static_cast<int>(result.sessionId));
                }
            });
        require(evidence_worker.start(), "evidence worker did not start");
        sensor::HallParkingService service(
            std::move(configs), app_config, channels, database,
            [&](int session_id) { canceled_session.store(session_id); },
            timer_manager, events, evidence_worker, &system_events,
            [&](const parking::ParkingTransitionResult& transition) {
                std::lock_guard lock(transition_mutex);
                capture_transitions.push_back(transition);
            });

        require(!service.handleLine("BROKEN:SENSOR:MESSAGE", "uart"),
                "malformed sensor message was accepted");
        require(waitUntil([&] {
                    return countEventType(
                               temporary.database,
                               "SENSOR_MESSAGE_INVALID") == 1;
                }, 1s),
                "malformed sensor error was not saved to EVENT_LOG");
        require(!service.handleLine("BROKEN:SENSOR:MESSAGE", "uart"),
                "duplicate malformed sensor message was accepted");
        std::this_thread::sleep_for(30ms);
        require(countEventType(temporary.database, "SENSOR_MESSAGE_INVALID") == 1,
                "duplicate sensor errors bypassed suppression window");

        require(service.handleLine("SENSOR:HALL01:OCCUPIED:1"),
                "fake OCCUPIED was rejected");
        require(!database.findActiveBySlot("EV01").has_value(),
                "confirmation gate created a DB session immediately");
        require(waitUntil([&] {
                    return database.findActiveBySlot("EV01").has_value();
                }, 1s),
                "single OCCUPIED was not auto-confirmed after threshold");
        auto first = database.findActiveBySlot("EV01");
        require(first.has_value(), "OCCUPIED did not create database session");
        require(waitUntil([&] {
                    return first->id == enqueued_session.load();
                }, 1s),
                "OCCUPIED evidence was not asynchronously enqueued");
        {
            std::lock_guard lock(transition_mutex);
            require(capture_transitions.size() == 1 &&
                        capture_transitions.front().sessionId ==
                            std::to_string(first->id),
                    "capture scheduler did not receive SQLite session_id");
        }
        std::vector<database::ImageView> first_images;
        require(waitUntil([&] {
                    first_images.clear();
                    return database.listSessionImages(
                               static_cast<int>(first->id), first_images) &&
                           first_images.size() == 1;
                }, 1s),
                "entry Snapshot was not written to IMAGE_LOG");
        const std::string first_path = first_images.front().original_path;
        require(first_images.front().evidence_reason ==
                    "OCCUPANCY_START_EVIDENCE",
                "entry image has wrong evidence_reason");
        require(std::filesystem::exists(first_path),
                "entry Snapshot file was not created from latest frame");

        require(service.handleLine("SENSOR:HALL01:OCCUPIED:2"),
                "duplicate OCCUPIED transport failed");
        require(database.listLogs().size() == 1,
                "duplicate OCCUPIED created another session");
        first_images.clear();
        require(database.listSessionImages(static_cast<int>(first->id),
                                           first_images) &&
                    std::count_if(first_images.begin(), first_images.end(),
                        [](const database::ImageView& image) {
                            return image.evidence_reason ==
                                   "OCCUPANCY_START_EVIDENCE";
                        }) == 1,
                "duplicate OCCUPIED created duplicate start evidence");

        require(service.handleLine("SENSOR:HALL01:VACANT:3"),
                "early VACANT was rejected");
        require(waitUntil([&] {
                    return canceled_session.load() == first->id;
                }, 1s),
                "early departure did not cancel pending OCR");
        {
            std::lock_guard lock(transition_mutex);
            require(capture_transitions.size() == 2 &&
                        capture_transitions.back().code ==
                            parking::ParkingTransitionCode::SessionCompleted &&
                        capture_transitions.back().sessionId ==
                            std::to_string(first->id),
                    "departure did not cancel the SQLite capture schedule");
        }
        first_images.clear();
        require(waitUntil([&] {
                    first_images.clear();
                    return !std::filesystem::exists(first_path) &&
                           database.listSessionImages(
                               static_cast<int>(first->id), first_images) &&
                           first_images.empty();
                }, 1s),
                "early departure did not remove Snapshot/IMAGE_LOG data");
        std::this_thread::sleep_for(100ms);
        require(database.listSessionImages(static_cast<int>(first->id), first_images) &&
                    first_images.empty(),
                "canceled overstay evidence was created after VACANT");
        database::ParkingSlotView slot;
        require(database.getParkingSlot("EV01", slot) &&
                    slot.parking_status == "VACANT",
                "early departure did not restore PARKING_SLOT to VACANT");

        require(service.handleLine("SENSOR:HALL01:OCCUPIED:4"),
                "second OCCUPIED was rejected");
        require(waitUntil([&] {
                    return database.findActiveBySlot("EV01").has_value();
                }, 1s),
                "second OCCUPIED was not auto-confirmed");
        auto second = database.findActiveBySlot("EV01");
        require(second.has_value() && second->id != first->id,
                "second parking session was not created");
        require(waitUntil([&] {
                    const auto refreshed = database.findLogById(second->id);
                    if (!refreshed || !refreshed->image_path_1.has_value())
                        return false;
                    second = refreshed;
                    return true;
                }, 1s),
                "second start evidence was not ready for OCR");
        require(database.applyPlateOcr(
                    static_cast<int>(second->id), "EV01",
                    second->image_path_1.value_or(""), "123가4567", 0.99) == "EV",
                "mock OCR did not classify the seeded EV");
        require(timer_manager.handleRecognizedSession(
                    second->id, "EV01", "123가4567").accepted,
                "EV timer did not start");
        require(waitUntil([&] {
                    const auto record = database.findLogById(second->id);
                    return record && record->status == "VIOLATION";
                }, 1s),
                "timer did not mark the session as VIOLATION");

        std::vector<database::ImageView> violation_images;
        require(waitUntil([&] {
                    violation_images.clear();
                    return database.listSessionImages(
                               static_cast<int>(second->id), violation_images) &&
                           violation_images.size() == 2;
                }, 1s),
                "violation latest-frame Snapshot was not added to IMAGE_LOG");
        require(std::count_if(
                    violation_images.begin(), violation_images.end(),
                    [](const database::ImageView& image) {
                        return image.evidence_reason ==
                               "OCCUPANCY_START_EVIDENCE";
                    }) == 1 &&
                    std::count_if(
                        violation_images.begin(), violation_images.end(),
                        [](const database::ImageView& image) {
                            return image.evidence_reason ==
                                   "OVERSTAY_EVIDENCE";
                        }) == 1,
                "evidence reasons are missing or duplicated");
        for (const auto& image : violation_images)
            require(std::filesystem::exists(image.original_path),
                    "violation evidence path does not contain a real file");
        require(waitUntil([&] {
                    std::lock_guard lock(event_mutex);
                    return std::find(published_events.begin(),
                                     published_events.end(),
                                     "VIOLATION_TRIGGERED") !=
                           published_events.end();
                }, 1s),
                "violation event was not sent to the external publisher");

        require(service.handleLine("SENSOR:HALL01:VACANT:5"),
                "post-violation VACANT was rejected");
        violation_images.clear();
        require(database.listSessionImages(
                    static_cast<int>(second->id), violation_images) &&
                    violation_images.size() == 2,
                "violation evidence was incorrectly deleted on departure");
        require(waitUntil([&] {
                    return !database.findActiveBySlot("EV01").has_value();
                }, 1s),
                "violating session departure did not finish before next entry");

        // 일반 차량은 OCR 직후 즉시 위반이며, 출차해도 시작 증거를 보존한다.
        require(service.handleLine("SENSOR:HALL01:OCCUPIED:6"),
                "NON_EV OCCUPIED was rejected");
        require(waitUntil([&] {
                    return database.findActiveBySlot("EV01").has_value();
                }, 1s),
                "NON_EV session was not created");
        auto non_ev = database.findActiveBySlot("EV01");
        require(non_ev.has_value(), "NON_EV active session is missing");
        std::vector<database::ImageView> non_ev_images;
        require(waitUntil([&] {
                    non_ev_images.clear();
                    return database.listSessionImages(
                               static_cast<int>(non_ev->id), non_ev_images) &&
                           non_ev_images.size() == 1;
                }, 1s),
                "NON_EV start evidence was not stored");
        const std::string non_ev_path = non_ev_images.front().original_path;
        require(database.applyPlateOcr(
                    static_cast<int>(non_ev->id), "EV01", non_ev_path,
                    "345다6789", 0.98) == "NON_EV",
                "seeded general vehicle was not classified as NON_EV");
        const auto non_ev_result = timer_manager.handleRecognizedSession(
            non_ev->id, "EV01", "345다6789");
        require(!non_ev_result.accepted &&
                    non_ev_result.category ==
                        parking_timer::VehicleCategory::NonEv,
                "NON_EV was incorrectly registered in overtime timer");
        const auto non_ev_violation = database.findLogById(non_ev->id);
        require(non_ev_violation &&
                    non_ev_violation->status == "VIOLATION" &&
                    non_ev_violation->violation_at.has_value(),
                "NON_EV did not become an immediate violation");
        {
            std::lock_guard lock(event_mutex);
            require(std::find(published_events.begin(), published_events.end(),
                              "NON_EV_ALERT") != published_events.end(),
                    "NON_EV alert was not sent to the external publisher");
        }
        require(service.handleLine("SENSOR:HALL01:VACANT:7"),
                "NON_EV VACANT was rejected");
        non_ev_images.clear();
        require(database.listSessionImages(
                    static_cast<int>(non_ev->id), non_ev_images) &&
                    non_ev_images.size() == 1 &&
                    std::filesystem::exists(non_ev_path),
                "NON_EV evidence was incorrectly deleted on departure");

        // 재시작을 모사해 메모리 상태에는 없고 DB에만 남은 ACTIVE를 만든다.
        const auto adopted_id = database.createHallSession(
            "EV02", "HALL02", parking_timer::utcNow());
        const auto count_before_adopt = database.listLogs().size();
        require(service.handleLine("SENSOR:HALL02:OCCUPIED:1"),
                "recovery OCCUPIED was rejected");
        require(waitUntil([&] {
                    const auto active = database.findActiveBySlot("EV02");
                    return active && active->id == adopted_id;
                }, 1s),
                "existing ACTIVE session was not adopted");
        std::this_thread::sleep_for(80ms);
        require(database.listLogs().size() == count_before_adopt,
                "adopting an ACTIVE session created a duplicate row");
        require(service.handleLine("SENSOR:HALL02:VACANT:2"),
                "adopted session VACANT was rejected");
        require(waitUntil([&] {
                    return !database.findActiveBySlot("EV02").has_value();
                }, 1s),
                "adopted ACTIVE session was not closed by VACANT");

        const auto stale_id = database.createHallSession(
            "EV03", "HALL03", parking_timer::utcNow());
        require(stale_id > 0, "stale session fixture was not created");
        require(service.handleLine("SENSOR:HALL03:VACANT:1"),
                "startup VACANT reconciliation was rejected");
        require(waitUntil([&] {
                    return !database.findActiveBySlot("EV03").has_value();
                }, 1s),
                "VACANT did not close a DB-only stale ACTIVE session");

        system_events.stop();
        std::cout << "[PASS] hall→snapshot→timer→violation→retention flow\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] hall flow: " << exception.what() << '\n';
        return 1;
    }
}

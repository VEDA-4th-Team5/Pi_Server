/** @file HallTimerIntegrationTest.cpp @brief 가짜 프레임과 센서로 실제 서버 주차 흐름을 검증한다. */
#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/SystemEventReporter.hpp"
#include "parking/ParkingSlotConfig.hpp"
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

        database::EventDatabase database(temporary.database);
        const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");

        app::AppConfig app_config{};
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
            [&](std::int64_t, const std::string& slot_id, const std::string&) {
                return snapshots.saveIvaAreaSnapshot(
                    channel, slot_id, {0.0, 0.0, 1.0, 1.0});
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

        int enqueued_session = -1;
        int canceled_session = -1;
        sensor::HallParkingService service(
            std::move(configs), app_config, channels, snapshots, database,
            [&](int session_id, const std::string&, const std::string&) {
                enqueued_session = session_id;
            },
            [&](int session_id) { canceled_session = session_id; },
            timer_manager, events, &system_events);

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
        auto first = database.findActiveBySlot("EV01");
        require(first.has_value() && first->id == enqueued_session,
                "OCCUPIED did not create/enqueue one database session");
        std::vector<database::ImageView> first_images;
        require(database.listSessionImages(static_cast<int>(first->id), first_images) &&
                    first_images.size() == 1,
                "entry Snapshot was not written to IMAGE_LOG");
        const std::string first_path = first_images.front().original_path;
        require(std::filesystem::exists(first_path),
                "entry Snapshot file was not created from latest frame");

        require(service.handleLine("SENSOR:HALL01:OCCUPIED:2"),
                "duplicate OCCUPIED transport failed");
        require(database.listLogs().size() == 1,
                "duplicate OCCUPIED created another session");

        require(service.handleLine("SENSOR:HALL01:VACANT:3"),
                "early VACANT was rejected");
        require(canceled_session == first->id,
                "early departure did not cancel pending OCR");
        require(!std::filesystem::exists(first_path),
                "early departure did not remove Snapshot file");
        first_images.clear();
        require(database.listSessionImages(static_cast<int>(first->id), first_images) &&
                    first_images.empty(),
                "early departure did not remove IMAGE_LOG rows");
        database::ParkingSlotView slot;
        require(database.getParkingSlot("EV01", slot) &&
                    slot.parking_status == "VACANT",
                "early departure did not restore PARKING_SLOT to VACANT");

        require(service.handleLine("SENSOR:HALL01:OCCUPIED:4"),
                "second OCCUPIED was rejected");
        auto second = database.findActiveBySlot("EV01");
        require(second.has_value() && second->id != first->id,
                "second parking session was not created");
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
        require(database.listSessionImages(
                    static_cast<int>(second->id), violation_images) &&
                    violation_images.size() == 2,
                "violation latest-frame Snapshot was not added to IMAGE_LOG");
        for (const auto& image : violation_images)
            require(std::filesystem::exists(image.original_path),
                    "violation evidence path does not contain a real file");
        {
            std::lock_guard lock(event_mutex);
            require(std::find(published_events.begin(), published_events.end(),
                              "VIOLATION_TRIGGERED") != published_events.end(),
                    "violation event was not sent to the external publisher");
        }

        require(service.handleLine("SENSOR:HALL01:VACANT:5"),
                "post-violation VACANT was rejected");
        violation_images.clear();
        require(database.listSessionImages(
                    static_cast<int>(second->id), violation_images) &&
                    violation_images.size() == 2,
                "violation evidence was incorrectly deleted on departure");

        system_events.stop();
        std::cout << "[PASS] hall→snapshot→timer→violation→retention flow\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] hall flow: " << exception.what() << '\n';
        return 1;
    }
}

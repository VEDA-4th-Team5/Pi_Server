#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "sensor/HallParkingService.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
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

template <typename Predicate>
bool waitUntil(Predicate predicate,
               const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

struct TemporaryRoot {
    std::filesystem::path path;
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryRoot temporary{
        std::filesystem::temp_directory_path() /
        ("camera_iva_occupancy_" + std::to_string(unique))};

    try {
        require(argc >= 2, "parking slot fixture path is required");
        auto slots = parking::ParkingSlotConfigLoader::loadFromFile(argv[1]);

        database::EventDatabase database(temporary.path / "parking.sqlite3");
        const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");

        app::AppConfig config{};
        config.parking_occupancy_source = "CAMERA_IVA";
        config.camera_iva_exit_confirm_ms = 50;
        config.parking_occupancy_confirm_ms = 0;
        config.parking_hall_work_queue_capacity = 100;
        config.iva_areas.push_back(
            {"EV01", "name1", "ch01", 0.0, 0.0, 1.0, 1.0});

        auto channel = std::make_shared<camera::CameraChannel>();
        channel->camera_id = "cam01";
        channel->channel_id = "ch01";
        channel->latest_full_frame = cv::Mat(
            360, 640, CV_8UC3, cv::Scalar(30, 90, 170));
        std::vector<std::shared_ptr<camera::CameraChannel>> channels{channel};
        std::atomic<bool> running{true};
        snapshot::SnapshotStorage storage(
            (temporary.path / "snapshots").string(), 100, running);

        parking_timer::EventManager events;
        parking_timer::ParkingSlotManager timer(
            database, events, 10s,
            [&database](const std::int64_t sessionId, const std::string&,
                        const std::string&) {
                return database.findEvidenceImagePath(
                           sessionId, "OVERSTAY_EVIDENCE")
                    .value_or("");
            });
        parking::EvidenceCaptureWorker::Config evidenceConfig;
        evidenceConfig.overstayDelay = 10s;
        parking::EvidenceCaptureWorker evidence(
            storage, database, evidenceConfig);
        require(evidence.start(), "evidence worker did not start");

        {
            std::atomic<int> canceledSession{-1};
            sensor::HallParkingService service(
                std::move(slots), config, channels, database,
                [&canceledSession](const int sessionId) {
                    canceledSession.store(sessionId);
                },
                timer, events, evidence);

            require(service.handleCameraOccupancy("EV01", true),
                    "IVA ENTER was rejected");
            require(waitUntil([&] {
                        return database.findActiveBySlot("EV01").has_value();
                    }),
                    "IVA ENTER did not create an ACTIVE session");
            const auto first = database.findActiveBySlot("EV01");
            require(first.has_value(), "first IVA session is missing");

            std::vector<database::ImageView> images;
            require(waitUntil([&] {
                        images.clear();
                        return database.listSessionImages(
                                   static_cast<int>(first->id), images) &&
                               images.size() == 1;
                    }),
                    "IVA ENTER did not store start evidence");
            const std::string firstPath = images.front().original_path;
            const std::string expectedDirectory =
                "/EV01/session_" + std::to_string(first->id) +
                "/occupancy_start/";
            require(firstPath.find(expectedDirectory) != std::string::npos,
                    "start evidence did not use the session directory");

            require(service.handleCameraOccupancy("EV01", true),
                    "duplicate IVA ENTER was rejected");
            require(database.listLogs().size() == 1,
                    "duplicate IVA ENTER created another session");

            require(service.handleCameraOccupancy("EV01", false),
                    "IVA EXIT was rejected");
            std::this_thread::sleep_for(20ms);
            require(database.findActiveBySlot("EV01").has_value(),
                    "IVA EXIT closed the session before confirmation");
            require(service.handleCameraOccupancy("EV01", true),
                    "ENTER did not cancel pending EXIT");
            std::this_thread::sleep_for(80ms);
            require(database.findActiveBySlot("EV01").has_value(),
                    "canceled IVA EXIT still closed the session");

            require(service.handleCameraOccupancy("EV01", false),
                    "confirmed IVA EXIT was rejected");
            require(service.handleCameraOccupancy("EV01", false),
                    "duplicate pending IVA EXIT was rejected");
            require(waitUntil([&] {
                        return !database.findActiveBySlot("EV01").has_value();
                    }),
                    "confirmed IVA EXIT did not close the session");
            images.clear();
            require(waitUntil([&] {
                        images.clear();
                        return !std::filesystem::exists(firstPath) &&
                               database.listSessionImages(
                                   static_cast<int>(first->id), images) &&
                               images.empty();
                    }),
                    "early IVA EXIT did not delete image and IMAGE_LOG");
            require(canceledSession.load() == first->id,
                    "early IVA EXIT did not cancel OCR");

            require(service.handleCameraOccupancy("EV01", true),
                    "second IVA ENTER was rejected");
            require(waitUntil([&] {
                        return database.findActiveBySlot("EV01").has_value();
                    }),
                    "second IVA session was not created");
            const auto second = database.findActiveBySlot("EV01");
            require(second && second->id != first->id,
                    "second IVA session reused the previous ID");
            std::vector<database::ImageView> violationImages;
            require(waitUntil([&] {
                        violationImages.clear();
                        return database.listSessionImages(
                                   static_cast<int>(second->id),
                                   violationImages) &&
                               violationImages.size() == 1;
                    }),
                    "second IVA start evidence is missing");
            const std::string violationPath =
                violationImages.front().original_path;
            require(database.applyPlateOcr(
                        static_cast<int>(second->id), "EV01", violationPath,
                        "345다6789", 0.98) == "NON_EV",
                    "NON_EV test vehicle was not classified");
            const auto nonEv = timer.handleRecognizedSession(
                second->id, "EV01", "345다6789");
            require(!nonEv.accepted, "NON_EV was accepted by timer");
            const auto violated = database.findLogById(second->id);
            require(violated && violated->violation_at.has_value(),
                    "NON_EV did not set violation_at");

            require(service.handleCameraOccupancy("EV01", false),
                    "violating IVA EXIT was rejected");
            require(waitUntil([&] {
                        return !database.findActiveBySlot("EV01").has_value();
                    }),
                    "violating IVA session did not close");
            violationImages.clear();
            require(database.listSessionImages(
                        static_cast<int>(second->id), violationImages) &&
                        violationImages.size() == 1 &&
                        std::filesystem::exists(violationPath),
                    "violation evidence was deleted by IVA EXIT");
        }

        evidence.stop();
        std::cout << "[PASS] camera IVA ENTER/EXIT/session cleanup flow\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] camera IVA occupancy: " << error.what() << '\n';
        return 1;
    }
}

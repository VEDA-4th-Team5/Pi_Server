#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
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

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::size_t countReason(const std::vector<database::ImageView>& images,
                        const std::string& reason) {
    std::size_t count{};
    for (const auto& image : images)
        if (image.evidence_reason == reason) ++count;
    return count;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("evidence_worker_" + std::to_string(unique));
    const fs::path db_path = root / "parking.sqlite3";
    const fs::path snapshots_path = root / "snapshots";
    try {
        database::EventDatabase database(db_path);
        const fs::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");
        database.migrateRuntimeSchema();
        database.migrateRuntimeSchema();

        std::atomic<bool> running{true};
        snapshot::SnapshotStorage storage(snapshots_path.string(), 50, running);
        auto channel = std::make_shared<camera::CameraChannel>();
        channel->camera_id = "mock-camera";
        channel->channel_id = "ch01";
        channel->latest_full_frame = cv::Mat(
            240, 320, CV_8UC3, cv::Scalar(10, 90, 180));

        std::mutex result_mutex;
        std::vector<parking::EvidenceCaptureResult> results;
        parking::EvidenceCaptureWorker::Config config;
        config.overstayDelay = 60ms;
        parking::EvidenceCaptureWorker worker(
            storage, database, config,
            [&](const parking::EvidenceCaptureResult& result) {
                std::lock_guard lock(result_mutex);
                results.push_back(result);
            });
        require(worker.start(), "worker did not start");

        const auto session1 = database.createHallSession(
            "EV01", "HALL01", "2026-07-27T09:00:00");
        require(worker.scheduleSession({session1, "EV01", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "session1 schedule failed");
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(session1), images) &&
                           countReason(images, "OCCUPANCY_START_EVIDENCE") == 1 &&
                           countReason(images, "OVERSTAY_EVIDENCE") == 1;
                }, 1s), "start/overstay evidence was not stored");
        require(worker.scheduleSession({session1, "EV01", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "idempotent duplicate schedule should be accepted");
        std::this_thread::sleep_for(80ms);
        std::vector<database::ImageView> images1;
        require(database.listSessionImages(static_cast<int>(session1), images1) &&
                    images1.size() == 2,
                "duplicate schedule created extra evidence");

        const auto session2 = database.createHallSession(
            "EV02", "HALL02", "2026-07-27T09:10:00");
        require(worker.scheduleSession({session2, "EV02", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "session2 schedule failed");
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(session2), images) &&
                           countReason(images, "OCCUPANCY_START_EVIDENCE") == 1;
                }, 1s), "session2 start evidence missing");
        worker.cancelSession(session2);
        std::this_thread::sleep_for(100ms);
        std::vector<database::ImageView> images2;
        require(database.listSessionImages(static_cast<int>(session2), images2) &&
                    countReason(images2, "OVERSTAY_EVIDENCE") == 0,
                "canceled overstay evidence was stored");

        const auto session3 = database.createHallSession(
            "EV03", "HALL03", "2026-07-27T09:20:00");
        require(worker.scheduleSession({session3, "EV03", channel,
                    {0.0, 0.0, 0.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "invalid ROI test schedule failed");
        require(waitUntil([&] {
                    std::lock_guard lock(result_mutex);
                    for (const auto& result : results)
                        if (result.sessionId == session3 && !result.stored)
                            return true;
                    return false;
                }, 1s), "invalid ROI failure was not reported");
        std::vector<database::ImageView> images3;
        require(database.listSessionImages(static_cast<int>(session3), images3) &&
                    images3.empty(),
                "file failure created a fake DB row");
        worker.cancelSession(session3);

        const auto session4 = database.createHallSession(
            "EV04", "HALL04", "2026-07-27T09:30:00");
        database.close();
        require(worker.scheduleSession({session4, "EV04", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "closed DB test schedule failed");
        require(waitUntil([&] {
                    std::lock_guard lock(result_mutex);
                    for (const auto& result : results)
                        if (result.sessionId == session4 && !result.stored &&
                            result.message.find("closed database") !=
                                std::string::npos)
                            return true;
                    return false;
                }, 1s), "DB failure was not isolated/reported");
        worker.stop();
        running.store(false);

        for (const auto& entry : fs::recursive_directory_iterator(snapshots_path)) {
            require(entry.path().filename().string().find(
                        "session_" + std::to_string(session4) + "_") ==
                        std::string::npos,
                    "DB failure left an orphan evidence file");
        }
        fs::remove_all(root);
        return 0;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        throw;
    }
}

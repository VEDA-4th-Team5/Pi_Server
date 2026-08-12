#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

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
        std::vector<unsigned char> originalJpeg;
        std::vector<unsigned char> enhancedJpeg;
        require(cv::imencode(".jpg", channel->latest_full_frame, originalJpeg),
                "API fixture original JPEG encode failed");
        const cv::Mat enhancedFrame(
            240, 320, CV_8UC3, cv::Scalar(25, 120, 220));
        require(cv::imencode(".jpg", enhancedFrame, enhancedJpeg),
                "API fixture enhanced JPEG encode failed");
        const auto croppedPair = storage.saveCameraApiHallCapture(
            "ch01", 999, "EV01", "HALL_30S",
            {0.25, 0.25, 0.5, 0.5}, originalJpeg, enhancedJpeg);
        require(!croppedPair.originalPath.empty() &&
                    !croppedPair.enhancedPath.empty(),
                "camera API ROI crop fixture was not stored");
        require(fs::path(croppedPair.originalPath).parent_path().filename() ==
                        "original" &&
                    fs::path(croppedPair.enhancedPath).parent_path().filename() ==
                        "enhanced",
                "camera API original/enhanced directories were not separated");
        const cv::Mat croppedOriginal = cv::imread(croppedPair.originalPath);
        const cv::Mat croppedEnhanced = cv::imread(croppedPair.enhancedPath);
        require(croppedOriginal.cols == 160 && croppedOriginal.rows == 120 &&
                    croppedEnhanced.cols == 160 &&
                    croppedEnhanced.rows == 120,
                "camera API original/enhanced did not use the same ROI");
        const auto tinyCrop = storage.saveCameraApiHallCapture(
            "ch01", 999, "EV01", "HALL_60S",
            {0.0, 0.0, 0.001, 0.001}, originalJpeg, enhancedJpeg);
        require(tinyCrop.originalPath.empty() && tinyCrop.enhancedPath.empty(),
                "sub-8px ROI must be rejected before OCR storage");
        fs::remove(croppedPair.originalPath);
        fs::remove(croppedPair.enhancedPath);

        std::mutex result_mutex;
        std::vector<parking::EvidenceCaptureResult> results;
        std::mutex roi_mutex;
        parking::AppliedParkingRoi current_roi{
            "EV01", {0.25, 0.25, 0.5, 0.5}, 7};
        parking::EvidenceCaptureWorker::Config config;
        config.overstayDelay = 60ms;
        config.maxPendingJobs = 2;
        parking::EvidenceCaptureWorker worker(
            storage, database, config,
            [&](const parking::EvidenceCaptureResult& result) {
                std::lock_guard lock(result_mutex);
                results.push_back(result);
            },
            [&](const parking::EvidenceCaptureRequest& request,
                const parking::EvidenceReason reason) {
                if (request.roi.width <= 0.0 || request.roi.height <= 0.0)
                    return snapshot::StoredImagePair{};
                return storage.saveCameraApiHallCapture(
                    request.channel->channel_id, request.sessionId,
                    request.slotId, parking::toString(reason), request.roi,
                    originalJpeg, enhancedJpeg);
            },
            [&roi_mutex, &current_roi](const std::string& slot_id)
                -> std::optional<parking::AppliedParkingRoi> {
                std::lock_guard lock(roi_mutex);
                if (slot_id == "EV04") return std::nullopt;
                auto applied = current_roi;
                applied.slotId = slot_id;
                return applied;
            });
        require(worker.start(), "worker did not start");

        const auto session1 = database.createHallSession(
            "EV01", "HALL01", "2026-07-27T09:00:00");
        require(worker.scheduleSession({session1, "EV01", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "session1 schedule failed");
        require(waitUntil([&] {
                    std::lock_guard lock(result_mutex);
                    for (const auto& result : results) {
                        if (result.sessionId == session1 && result.stored &&
                            result.reason ==
                                parking::EvidenceReason::OccupancyStart) {
                            return true;
                        }
                    }
                    return false;
                }, 1s), "session1 start evidence was not stored");
        {
            std::lock_guard lock(roi_mutex);
            current_roi = {"EV01", {0.0, 0.0, 0.25, 0.25}, 8};
        }
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(session1), images) &&
                           countReason(images, "OCCUPANCY_START_EVIDENCE") ==
                               1 &&
                           countReason(images, "OVERSTAY_EVIDENCE") == 1;
                }, 1s), "session1 overstay evidence was not stored");
        {
            std::lock_guard lock(result_mutex);
            bool start_revision_verified{};
            bool overstay_revision_verified{};
            for (const auto& result : results) {
                if (result.sessionId != session1 || !result.stored) continue;
                if (result.reason == parking::EvidenceReason::OccupancyStart) {
                    start_revision_verified = result.roiRevision == 7 &&
                        result.roi.x == 0.25 && result.roi.width == 0.5;
                } else if (result.reason == parking::EvidenceReason::Overstay) {
                    overstay_revision_verified = result.roiRevision == 8 &&
                        result.roi.x == 0.0 && result.roi.width == 0.25;
                }
            }
            require(start_revision_verified && overstay_revision_verified,
                    "evidence results did not preserve applied ROI revisions");
        }
        require(worker.scheduleSession({session1, "EV01", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "idempotent duplicate schedule should be accepted");
        std::this_thread::sleep_for(80ms);
        std::vector<database::ImageView> images1;
        require(database.listSessionImages(static_cast<int>(session1), images1) &&
                    images1.size() == 2,
                "duplicate schedule created extra evidence");
        for (const auto& image : images1) {
            require(!image.enhanced_path.empty() &&
                        fs::is_regular_file(image.enhanced_path),
                    "camera API enhanced evidence was not persisted");
            const std::string stage = image.evidence_reason ==
                    "OCCUPANCY_START_EVIDENCE"
                ? "occupancy_start" : "overstay";
            require(image.original_path.find(
                        "/EV01/" + stage + "/") != std::string::npos,
                    "camera API evidence directory layout mismatch");
            require(image.original_path.find(
                        "session_" + std::to_string(session1) +
                        "_slot_EV01_") != std::string::npos,
                    "camera API evidence filename lost session id");
            const cv::Mat stored = cv::imread(image.original_path);
            if (image.evidence_reason == "OCCUPANCY_START_EVIDENCE") {
                require(stored.cols == 160 && stored.rows == 120,
                        "start evidence did not use revision 7 ROI");
            } else {
                require(stored.cols == 80 && stored.rows == 60,
                        "overstay evidence did not use revision 8 ROI");
            }
        }

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
        require(worker.pendingCount() == 0,
                "canceled overstay job did not release queue capacity");
        std::this_thread::sleep_for(100ms);
        std::vector<database::ImageView> images2;
        require(database.listSessionImages(static_cast<int>(session2), images2) &&
                    countReason(images2, "OVERSTAY_EVIDENCE") == 0,
                "canceled overstay evidence was stored");
        require(database.departActiveBySlot("EV02", "2026-07-27T09:11:00")
                    .has_value(),
                "canceled fixture session was not closed");

        require(worker.updateOverstayDelay(400ms) == 0,
                "empty evidence queue unexpectedly changed");
        const auto rescheduled_session = database.createHallSession(
            "EV02", "HALL02", "2026-07-27T09:12:00");
        require(worker.scheduleSession({rescheduled_session, "EV02", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now()}),
                "evidence reschedule fixture failed");
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(rescheduled_session), images) &&
                           countReason(images,
                               "OCCUPANCY_START_EVIDENCE") == 1;
                }, 1s), "reschedule start evidence missing");
        require(worker.updateOverstayDelay(80ms) == 1,
                "active overstay evidence deadline was not replaced");
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(rescheduled_session), images) &&
                           countReason(images, "OVERSTAY_EVIDENCE") == 1;
                }, 500ms), "shortened evidence delay did not take effect");
        require(database.departActiveBySlot("EV02", "2026-07-27T09:13:00")
                    .has_value(), "reschedule fixture session was not closed");

        // 재시작 복원: DB에 시작 증거만 남은 ACTIVE 세션은 원래 T0에서
        // 계산한 남은 시간 뒤 초과 증거만 한 번 예약해야 한다.
        const auto restored_session = database.createHallSession(
            "EV03", "HALL03", "2026-07-27T09:15:00");
        const std::string restored_start_path = storage.saveEvidenceSnapshot(
            channel, restored_session, "EV03", "OCCUPANCY_START_EVIDENCE",
            {0.0, 0.0, 1.0, 1.0});
        require(!restored_start_path.empty(),
                "restore fixture start image was not created");
        require(database.insertEvidenceImage(
                    restored_session, restored_start_path,
                    "OCCUPANCY_START_EVIDENCE", "2026-07-27T09:15:00") ==
                    database::EvidenceInsertResult::Inserted,
                "restore fixture start image DB row was not created");
        require(worker.restoreSession({
                    restored_session, "EV03", channel,
                    {0.0, 0.0, 1.0, 1.0},
                    std::chrono::steady_clock::now() - 40ms}),
                "restored session schedule failed");
        require(waitUntil([&] {
                    std::vector<database::ImageView> images;
                    return database.listSessionImages(
                               static_cast<int>(restored_session), images) &&
                           countReason(images,
                               "OCCUPANCY_START_EVIDENCE") == 1 &&
                           countReason(images, "OVERSTAY_EVIDENCE") == 1;
                }, 1s),
                "restored T0 did not produce one overstay evidence image");
        require(database.departActiveBySlot("EV03", "2026-07-27T09:16:00")
                    .has_value(),
                "restored fixture session was not closed");

        const auto session3 = database.createHallSession(
            "EV04", "HALL04", "2026-07-27T09:20:00");
        require(worker.scheduleSession({session3, "EV04", channel,
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
            "EV03", "HALL03", "2026-07-27T09:30:00");
        database.close();
        require(worker.scheduleSession({session4, "EV03", channel,
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

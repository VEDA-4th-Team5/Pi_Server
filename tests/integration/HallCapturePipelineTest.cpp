#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "parking/HallCaptureExecutor.hpp"
#include "parking_timer/Types.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PARKING_TIMER_TEST_SQL_DIR
#error PARKING_TIMER_TEST_SQL_DIR must be defined
#endif

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::ParkingTransitionResult transition(
    const parking::ParkingTransitionCode code, const std::int64_t sessionId) {
    parking::ParkingTransitionResult result;
    result.code = code;
    result.sessionId = std::to_string(sessionId);
    result.slotId = "EV01";
    return result;
}

parking::CaptureRequest request(const std::int64_t sessionId,
                                const parking::CaptureReason reason) {
    parking::CaptureRequest value;
    value.sessionId = std::to_string(sessionId);
    value.slotId = "EV01";
    value.sensorId = "HALL01";
    value.target = {"mock-camera", "ch01", "EV01", 0.0, 0.0, 1.0, 1.0};
    value.reason = reason;
    value.sessionStartedAt = std::chrono::system_clock::now();
    value.scheduledFor = std::chrono::steady_clock::now();
    return value;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("hall_capture_pipeline_" + std::to_string(unique));
    try {
        database::EventDatabase database(root / "parking.sqlite3");
        const fs::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
        database.migrateRuntimeSchema();

        std::atomic<bool> running{true};
        snapshot::SnapshotStorage storage(
            (root / "snapshots").string(), 50, running);
        auto channel = std::make_shared<camera::CameraChannel>();
        channel->camera_id = "mock-camera";
        channel->channel_id = "ch01";
        channel->latest_full_frame = cv::Mat(
            240, 320, CV_8UC3, cv::Scalar(20, 80, 160));
        std::vector<std::shared_ptr<camera::CameraChannel>> channels{channel};

        std::vector<std::int64_t> ocrSessions;
        parking::HallCapturePorts ports;
        ports.writeImageLog = [&database](const parking::CapturedImage& image) {
            try {
                const auto result = database.insertHallCaptureImage(
                    image.sessionId, image.originalPath, image.enhancedPath,
                    parking::toEnhancementType(image.stage),
                    parking_timer::utcNow());
                if (result == database::EvidenceInsertResult::Inserted)
                    return parking::ImageStoreResult::Inserted;
                if (result == database::EvidenceInsertResult::Duplicate)
                    return parking::ImageStoreResult::Duplicate;
                return parking::ImageStoreResult::InactiveSession;
            } catch (...) {
                return parking::ImageStoreResult::Failed;
            }
        };
        ports.submitOcr = [&ocrSessions](const parking::CapturedImage& image) {
            ocrSessions.push_back(image.sessionId);
        };
        parking::HallCaptureCoordinator coordinator(std::move(ports));

        int draftPublishes{};
        parking::HallCaptureExecutor executor(
            channels, storage, coordinator,
            [&draftPublishes](const parking::CaptureRequest&) {
                ++draftPublishes;
                return false;  // MQTT 실패와 실제 로컬 촬영 성공은 독립이다.
            });

        const auto sessionId = database.createHallSession(
            "EV01", "HALL01", "2026-07-27T12:00:00");
        coordinator.onTransition(transition(
            parking::ParkingTransitionCode::SessionStarted, sessionId));
        require(executor.execute(request(
                    sessionId, parking::CaptureReason::HallOccupied30s)),
                "local 30s capture failed when MQTT draft publish failed");
        require(executor.execute(request(
                    sessionId, parking::CaptureReason::HallOccupied60s)),
                "local 60s capture failed");
        require(draftPublishes == 2, "MQTT draft publisher was not invoked");

        std::vector<database::ImageView> images;
        require(database.listSessionImages(static_cast<int>(sessionId), images),
                "session images query failed");
        require(images.size() == 2, "30s/60s images were not both stored");
        require(images[0].session_id == sessionId &&
                    images[1].session_id == sessionId,
                "capture images did not reuse the SQLite session_id");
        require(images[0].enhancement_type == "HALL_30S" &&
                    images[1].enhancement_type == "HALL_60S",
                "capture stages were not stored in order");
        require(ocrSessions == std::vector<std::int64_t>{sessionId},
                "60s image must wait while 30s OCR is in flight");
        for (const auto& image : images)
            require(fs::is_regular_file(image.original_path),
                    "IMAGE_LOG points to a missing capture file");

        // 같은 단계 재실행은 DB 중복으로 접히고 새로 쓴 파일은 Executor가 지운다.
        require(executor.execute(request(
                    sessionId, parking::CaptureReason::HallOccupied30s)),
                "duplicate stage should be an idempotent success");
        images.clear();
        require(database.listSessionImages(static_cast<int>(sessionId), images) &&
                    images.size() == 2,
                "duplicate stage created another IMAGE_LOG row");

        coordinator.onTransition(transition(
            parking::ParkingTransitionCode::SessionCompleted, sessionId));
        (void)database.departActiveBySlot("EV01", parking_timer::utcNow());
        require(executor.execute(request(
                    sessionId, parking::CaptureReason::HallOccupied60s)),
                "late inactive capture should be discarded without retry");
        images.clear();
        require(database.listSessionImages(static_cast<int>(sessionId), images) &&
                    images.size() == 2,
                "late capture created an IMAGE_LOG row");

        running.store(false);
        database.close();
        fs::remove_all(root);
        return 0;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        throw;
    }
}

#include "entrance/EntranceVehicleService.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Recorder {
    int downloads{};
    int ocrQueued{};
    int created{};
    int images{};
    int finished{};
    int evQueued{};
    int evSaved{};
    int duplicatesUpdated{};
    int artifactsDeleted{};
    bool downloadSucceeds{true};
    std::int64_t duplicateEventId{-1};
    std::int64_t nextEventId{77};
    std::string ocrPath;
    std::string evPath;
    std::string finishedPlate;
    std::string finishedError;
    ocr::GenericOcrCallback callback;
    entrance::EntranceEvCallback evCallback;

    entrance::EntranceVehiclePorts ports() {
        entrance::EntranceVehiclePorts value;
        value.downloadImage = [this](const std::string&, const std::string&,
                                     const std::string& destination) {
            ++downloads;
            const cv::Mat image(80, 160, CV_8UC3, cv::Scalar(20, 80, 180));
            const bool written = cv::imwrite(destination, image);
            return written && downloadSucceeds;
        };
        value.enqueueOcr = [this](const std::string&, const std::string& path,
                                  ocr::GenericOcrCallback next) {
            ++ocrQueued;
            ocrPath = path;
            callback = std::move(next);
            return true;
        };
        value.enqueueEvAnalysis =
            [this](entrance::EntranceEvTask task,
                   entrance::EntranceEvCallback next) {
                ++evQueued;
                evPath = std::move(task.imagePath);
                evCallback = std::move(next);
                return true;
            };
        value.createEvent = [this](const entrance::ObjectKey&, std::int64_t) {
            ++created;
            return nextEventId++;
        };
        value.saveImagePath = [this](std::int64_t, entrance::BestShotKind,
                                     const std::string&) {
            ++images;
            return true;
        };
        value.finishEvent = [this](std::int64_t, const std::string& plate,
                                   double, int, const std::string& error) {
            ++finished;
            finishedPlate = plate;
            finishedError = error;
            return entrance::EntrancePersistenceResult{
                true, plate.empty() ? "OCR_FAILED" : "EV"};
        };
        value.saveEvAnalysis =
            [this](std::int64_t, const entrance::EntranceEvResult&) {
                ++evSaved;
                return true;
            };
        value.incrementDuplicateCount = [this](const std::int64_t eventId) {
            ++duplicatesUpdated;
            duplicateEventId = eventId;
            return true;
        };
        value.markArtifactsDeleted = [this](std::int64_t) {
            ++artifactsDeleted;
            return true;
        };
        value.listArtifactsForCleanup = [](std::int64_t) {
            return std::vector<database::EntranceArtifactRecord>{};
        };
        return value;
    }
};

entrance::BestShotEvent makeEvent(const entrance::BestShotKind kind,
                                  const std::int64_t at,
                                  std::string objectId = "123") {
    return {{"cam01", "ch02", std::move(objectId)}, kind,
            kind == entrance::BestShotKind::Plate ? "/plate" : "/vehicle",
            "rtsp://camera/profile1", at};
}

void plateAloneRunsEvThenOcrOnce() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-vehicle-service-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    Recorder recorder;
    entrance::EntranceVehicleService service(
        coordinator, recorder.ports(), root.string());

    require(service.handle(makeEvent(entrance::BestShotKind::Vehicle, 1000)),
            "vehicle metadata was not consumed");
    require(recorder.created == 0 && recorder.downloads == 0 &&
                recorder.evQueued == 0 && recorder.ocrQueued == 0,
            "vehicle metadata created an entrance pipeline");
    require(service.handle(makeEvent(entrance::BestShotKind::Plate, 1100)),
            "plate metadata was not consumed");
    for (int i = 0; i < 8; ++i)
        (void)service.handle(makeEvent(entrance::BestShotKind::Plate, 1200 + i));
    require(recorder.created == 1 && recorder.downloads == 1 &&
                recorder.images == 1 && recorder.ocrQueued == 0 &&
                recorder.evQueued == 1,
            "plate did not start exactly one EV task before OCR");
    require(static_cast<bool>(recorder.evCallback) &&
                std::filesystem::path(recorder.evPath).filename() == "plate.jpg",
            "plate BestShot was not selected for EV analysis");
    entrance::EntranceEvResult evResult;
    evResult.requestId = "cam01|ch02|123";
    evResult.isEv = true;
    evResult.decision = "EV_CANDIDATE";
    evResult.runtimeSucceeded = true;
    recorder.evCallback(evResult);
    require(recorder.evSaved == 1, "EV analysis was not persisted exactly once");
    require(recorder.ocrQueued == 1 && static_cast<bool>(recorder.callback) &&
                std::filesystem::path(recorder.ocrPath).filename() == "plate.jpg",
            "Gemini OCR was not queued after the EV decision");
    ocr::GenericOcrResult result;
    result.task_id = "cam01|ch02|123";
    result.recognized = true;
    result.ocr_succeeded = true;
    result.plate_number = "12가3456";
    result.confidence = 0.95;
    result.attempts = 1;
    recorder.callback(result);
    require(recorder.finished == 1,
            "completed OCR was not persisted exactly once");
    require(recorder.finishedPlate == "12가3456",
            "recognized plate was not used for vehicle finalization");
    require(coordinator.state({"cam01", "ch02", "123"}) ==
                entrance::ObjectState::Completed,
            "entrance object did not reach COMPLETED");
    require(recorder.artifactsDeleted == 1,
            "successful entrance artifacts were not marked deleted");
    require(!std::filesystem::exists(
                std::filesystem::path(recorder.ocrPath).parent_path()),
            "successful entrance work directory was not deleted");
    std::filesystem::remove_all(root, ignored);
}

void sameImageWithDifferentObjectIdRunsPipelineOnce() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-image-dedup-service-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    Recorder recorder;
    entrance::EntranceVehicleService service(
        coordinator, recorder.ports(), root.string(), 20s, 10, 64, true, 24h);

    require(service.handle(makeEvent(entrance::BestShotKind::Plate, 1000,
                                     "camera-object-1")),
            "first plate metadata was not consumed");
    require(service.handle(makeEvent(entrance::BestShotKind::Plate, 1100,
                                     "camera-object-2")),
            "duplicate image metadata was not consumed");

    require(recorder.downloads == 2,
            "different object IDs were not fingerprinted independently");
    require(recorder.created == 1 && recorder.evQueued == 1,
            "same image created more than one entrance/OCR pipeline");
    require(recorder.duplicatesUpdated == 1 && recorder.duplicateEventId == 77,
            "duplicate image was not accumulated on the canonical DB event");
    require(coordinator.state({"cam01", "ch02", "camera-object-2"}) ==
                entrance::ObjectState::Duplicate,
            "duplicate object did not reach the terminal duplicate state");

    std::filesystem::remove_all(root, ignored);
}

void failedDownloadLeavesNoOrphanFile() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-download-failure-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    Recorder recorder;
    recorder.downloadSucceeds = false;
    entrance::EntranceVehicleService service(
        coordinator, recorder.ports(), root.string());

    require(service.handle(makeEvent(entrance::BestShotKind::Plate, 3000,
                                     "download-failure")),
            "failed download metadata was not consumed");
    require(recorder.created == 0 && recorder.evQueued == 0,
            "failed download created an entrance pipeline");
    std::size_t files{};
    if (std::filesystem::exists(root)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file()) ++files;
        }
    }
    require(files == 0, "failed download left an orphan entrance file");
    std::filesystem::remove_all(root, ignored);
}

void reviewStopsBeforeOcrAndVehicleCreation() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-plate-first-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    Recorder recorder;
    entrance::EntranceVehicleService service(
        coordinator, recorder.ports(), root.string());
    require(service.handle(makeEvent(entrance::BestShotKind::Plate, 2000)),
            "plate-first metadata was not consumed");
    entrance::EntranceEvResult review;
    review.requestId = "cam01|ch02|123";
    review.decision = "REVIEW";
    review.reason = "low observability";
    review.runtimeSucceeded = true;
    recorder.evCallback(review);
    require(recorder.evSaved == 1 && recorder.ocrQueued == 0,
            "REVIEW result incorrectly queued Gemini OCR");
    require(recorder.finished == 1 && recorder.finishedPlate.empty(),
            "REVIEW result incorrectly finalized a vehicle");
    require(coordinator.state({"cam01", "ch02", "123"}) ==
                entrance::ObjectState::Failed,
            "REVIEW result did not fail the entrance object");
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    try {
        plateAloneRunsEvThenOcrOnce();
        sameImageWithDifferentObjectIdRunsPipelineOnce();
        failedDownloadLeavesNoOrphanFile();
        reviewStopsBeforeOcrAndVehicleCreation();
        std::cout << "EntranceVehicleServiceTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceVehicleServiceTest failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

#include "entrance/EntranceVehicleService.hpp"

#include "util/Logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace {

std::string safeComponent(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '-' || character == '_';
        result.push_back(allowed ? static_cast<char>(character) : '_');
    }
    return result.empty() ? "unknown" : result;
}

bool shouldLogDuplicate(const std::size_t count) {
    return count == 1 || (count != 0 && (count & (count - 1)) == 0);
}

bool isInside(const fs::path& root, const fs::path& candidate) {
    const fs::path normalizedRoot = fs::absolute(root).lexically_normal();
    const fs::path normalizedCandidate =
        fs::absolute(candidate).lexically_normal();
    auto rootPart = normalizedRoot.begin();
    auto candidatePart = normalizedCandidate.begin();
    for (; rootPart != normalizedRoot.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == normalizedCandidate.end() ||
            *rootPart != *candidatePart) {
            return false;
        }
    }
    return candidatePart != normalizedCandidate.end();
}

}  // namespace

namespace entrance {

EntranceVehicleService::EntranceVehicleService(
    EntranceBestShotCoordinator& coordinator,
    EntranceVehiclePorts ports,
    std::string outputRoot,
    const std::chrono::milliseconds dedupWindow,
    const int dedupHammingThreshold,
    const std::size_t dedupCapacity,
    const bool deleteArtifactsOnSuccess,
    const std::chrono::hours failureRetention)
    : coordinator_(coordinator),
      ports_(std::move(ports)),
      outputRoot_(std::move(outputRoot)),
      imageDeduplicator_(dedupWindow, dedupHammingThreshold, dedupCapacity),
      deleteArtifactsOnSuccess_(deleteArtifactsOnSuccess),
      failureRetentionMs_(std::max<std::int64_t>(
          1, std::chrono::duration_cast<std::chrono::milliseconds>(
                 failureRetention).count())) {}

EntranceVehicleService::~EntranceVehicleService() { stop(); }

void EntranceVehicleService::start() {
    if (running_.exchange(true)) return;
    cleanupDueArtifacts();
    cleanupWorker_ = std::thread(&EntranceVehicleService::cleanupLoop, this);
}

void EntranceVehicleService::stop() {
    running_.store(false);
    if (cleanupWorker_.joinable()) cleanupWorker_.join();
}

bool EntranceVehicleService::handle(const BestShotEvent& event) {
    std::lock_guard ingressLock(ingressMutex_);
    // 입구 판정은 Plate BestShot 하나만으로 시작한다. Vehicle 메타데이터는
    // 소비만 하고 DB 행이나 대기 상태를 만들지 않는다.
    if (event.kind != BestShotKind::Plate) return true;

    const ObserveResult observed = coordinator_.observe(event);
    if (observed.code == ObserveCode::Invalid) return false;
    if (observed.code == ObserveCode::Ignored) return true;
    if (observed.code == ObserveCode::CapacityRejected) {
        util::logWarn("Entrance BestShot capacity reached: camera=" +
                      event.key.cameraId + " channel=" + event.key.channelId);
        return true;
    }
    if (observed.code == ObserveCode::DuplicateSuppressed) {
        if (observed.duplicateCount == 1) {
            util::logLine("ENTRANCE_OCR",
                          "duplicate suppressed camera=" + event.key.cameraId +
                          " channel=" + event.key.channelId + " object=" +
                          event.key.objectId);
        }
        return true;
    }

    if (!observed.newObject) return true;

    const std::string destination = imagePath(event);
    const std::string workDirectory = fs::path(destination).parent_path().string();
    std::error_code directoryError;
    fs::create_directories(fs::path(destination).parent_path(), directoryError);
    const bool downloaded = !directoryError && ports_.downloadImage &&
                            ports_.downloadImage(event.rtspUrl, event.imageRef,
                                                 destination);
    if (!downloaded) {
        (void)coordinator_.fail(event.key);
        std::error_code ignored;
        fs::remove_all(workDirectory, ignored);
        util::logError("Entrance BestShot download failed: camera=" +
                       event.key.cameraId + " channel=" + event.key.channelId +
                       " object=" + event.key.objectId);
        return true;
    }

    const ImageDedupResult dedup = imageDeduplicator_.observe(
        event.key, destination, event.receivedAtEpochMs);
    if (!dedup.fingerprinted) {
        util::logWarn("Entrance image fingerprint failed; processing continues: "
                      "camera=" + event.key.cameraId + " channel=" +
                      event.key.channelId + " object=" + event.key.objectId);
    } else if (dedup.duplicate) {
        (void)coordinator_.markImageDuplicate(event.key);
        std::error_code ignored;
        fs::remove_all(workDirectory, ignored);
        if (dedup.canonicalEventId.has_value() &&
            ports_.incrementDuplicateCount) {
            (void)ports_.incrementDuplicateCount(*dedup.canonicalEventId);
        }
        if (shouldLogDuplicate(dedup.duplicateCount)) {
            util::logLine(
                "ENTRANCE_BESTSHOT",
                "image duplicate suppressed camera=" + event.key.cameraId +
                    " channel=" + event.key.channelId + " object=" +
                    event.key.objectId + " canonical_object=" +
                    dedup.canonicalKey.objectId + " phash_distance=" +
                    std::to_string(dedup.hammingDistance) + " count=" +
                    std::to_string(dedup.duplicateCount));
        }
        return true;
    }

    const std::int64_t eventId = ports_.createEvent
        ? ports_.createEvent(event.key, event.receivedAtEpochMs) : -1;
    if (eventId < 0) {
        (void)coordinator_.fail(event.key);
        std::error_code ignored;
        fs::remove_all(workDirectory, ignored);
        util::logError("Entrance event DB creation failed: camera=" +
                       event.key.cameraId + " channel=" + event.key.channelId +
                       " object=" + event.key.objectId);
        return true;
    }

    if (!coordinator_.bindEventId(event.key, eventId) ||
        (dedup.fingerprinted &&
         !imageDeduplicator_.bindEventId(event.key, eventId))) {
        failEvent(eventId, event.key, "Entrance event binding failed");
        std::error_code ignored;
        fs::remove_all(workDirectory, ignored);
        util::logError("Entrance event binding failed: camera=" +
                       event.key.cameraId + " channel=" +
                       event.key.channelId + " object=" + event.key.objectId);
        return true;
    }
    util::logLine("ENTRANCE_BESTSHOT",
                  "collecting camera=" + event.key.cameraId + " channel=" +
                  event.key.channelId + " object=" + event.key.objectId);

    if (ports_.saveImagePath &&
        !ports_.saveImagePath(eventId, event.kind, destination)) {
        failEvent(eventId, event.key, "Plate image path persistence failed");
        std::error_code ignored;
        fs::remove_all(workDirectory, ignored);
        return true;
    }

    util::logLine("ENTRANCE_BESTSHOT",
                  "plate selected camera=" + event.key.cameraId +
                  " channel=" + event.key.channelId + " object=" +
                  event.key.objectId);
    const std::string taskId = event.key.text();
    if (!coordinator_.markEvProcessing(event.key)) {
        failEvent(eventId, event.key, "EV processing state transition failed");
        return true;
    }

    EntranceEvTask evTask;
    evTask.requestId = taskId;
    evTask.imagePath = destination;
    evTask.outputDirectory =
        (fs::path(destination).parent_path() / "ev_analysis").string();
    const bool queued = ports_.enqueueEvAnalysis && ports_.enqueueEvAnalysis(
        std::move(evTask),
        [this, eventId, key = event.key, destination, workDirectory](
            const EntranceEvResult& result) {
            handleEvResult(eventId, key, destination, workDirectory, result);
        });
    if (!queued) {
        failEvent(eventId, event.key, "EV analysis queue rejected");
        util::logWarn("Entrance EV queue rejected: camera=" +
                      event.key.cameraId + " channel=" + event.key.channelId +
                      " object=" + event.key.objectId);
        return true;
    }
    util::logLine("ENTRANCE_EV",
                  "queued camera=" + event.key.cameraId + " channel=" +
                  event.key.channelId + " object=" + event.key.objectId);
    return true;
}

void EntranceVehicleService::failEvent(const std::int64_t eventId,
                                       const ObjectKey& key,
                                       const std::string& reason) {
    if (!coordinator_.fail(key)) return;
    if (eventId >= 0 && ports_.finishEvent)
        (void)ports_.finishEvent(eventId, {}, 0.0, 0, reason);
}

void EntranceVehicleService::handleEvResult(
    const std::int64_t eventId, const ObjectKey& key,
    const std::string& plateImagePath,
    const std::string& artifactDirectory,
    const EntranceEvResult& result) {
    // TTL 만료나 선행 실패 뒤 도착한 결과는 DB와 OCR을 건드리지 않는다.
    if (!coordinator_.markOcrQueued(key)) return;
    const bool persisted = ports_.saveEvAnalysis &&
                           ports_.saveEvAnalysis(eventId, result);
    if (!persisted) {
        failEvent(eventId, key, "EV result persistence failed");
        util::logError("Entrance EV result persistence failed: camera=" +
                       key.cameraId + " channel=" + key.channelId +
                       " object=" + key.objectId);
        return;
    }
    if (!result.runtimeSucceeded || !result.isEv.has_value()) {
        const std::string reason = result.error.empty()
            ? "EV icon decision unavailable" : result.error;
        failEvent(eventId, key, reason);
        util::logWarn("Entrance EV classification failed: camera=" +
                      key.cameraId + " channel=" + key.channelId + " object=" +
                      key.objectId + " decision=" + result.decision);
        return;
    }
    util::logLine("ENTRANCE_EV",
                  "completed camera=" + key.cameraId + " channel=" +
                  key.channelId + " object=" + key.objectId + " decision=" +
                  result.decision + " mode=authoritative" +
                  " processing_ms=" + std::to_string(result.processingMs));

    if (!coordinator_.markOcrProcessing(key)) {
        failEvent(eventId, key, "OCR processing state transition failed");
        return;
    }
    const bool queued = ports_.enqueueOcr && ports_.enqueueOcr(
        key.text(), plateImagePath,
        [this, key, artifactDirectory](const ocr::GenericOcrResult& ocrResult) {
            handleOcrResult(key, artifactDirectory, ocrResult);
        });
    if (!queued) {
        failEvent(eventId, key, "OCR queue rejected entrance object");
        util::logWarn("Entrance OCR queue rejected: camera=" + key.cameraId +
                      " channel=" + key.channelId + " object=" + key.objectId);
        return;
    }
    util::logLine("ENTRANCE_OCR",
                  "queued camera=" + key.cameraId + " channel=" +
                  key.channelId + " object=" + key.objectId);
}

void EntranceVehicleService::cleanupLoop() {
    auto nextArtifactCleanup = std::chrono::steady_clock::now();
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const auto& expired : coordinator_.sweep(nowEpochMs())) {
            if (expired.eventId >= 0 && ports_.finishEvent)
                (void)ports_.finishEvent(expired.eventId, {}, 0.0, 0,
                                         expired.reason);
            util::logWarn("Entrance object expired: camera=" +
                          expired.key.cameraId + " channel=" +
                          expired.key.channelId + " object=" +
                              expired.key.objectId + " reason=" + expired.reason);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextArtifactCleanup) {
            cleanupDueArtifacts();
            nextArtifactCleanup = now + std::chrono::seconds(10);
        }
    }
}

void EntranceVehicleService::handleOcrResult(
    const ObjectKey& key, const std::string& artifactDirectory,
    const ocr::GenericOcrResult& result) {
    const auto eventId = coordinator_.eventId(key);
    if (!eventId) return;
    if (!result.recognized) {
        const std::string reason = result.error_message.empty()
            ? "Gemini OCR did not recognize a plate" : result.error_message;
        failEvent(*eventId, key, reason);
        util::logWarn("Entrance OCR failed: camera=" + key.cameraId +
                      " channel=" + key.channelId + " object=" + key.objectId +
                      " attempts=" + std::to_string(result.attempts));
        return;
    }
    // DB 확정 중 TTL 정리와 경쟁하지 않도록 OCR 결과 소유권을 먼저 획득한다.
    if (!coordinator_.beginFinalization(key)) return;
    const auto persisted = ports_.finishEvent
        ? ports_.finishEvent(*eventId, result.plate_number, result.confidence,
                             result.attempts, result.error_message)
        : EntrancePersistenceResult{};
    const std::size_t duplicates = coordinator_.duplicateCount(key);
    if (persisted.success && coordinator_.complete(key)) {
        util::logLine("ENTRANCE_OCR",
                      "completed camera=" + key.cameraId + " channel=" +
                      key.channelId + " object=" + key.objectId +
                      " plate=" + result.plate_number + " class=" +
                      persisted.classification +
                      " masked_duplicates=" + std::to_string(duplicates));
        if (deleteArtifactsOnSuccess_ &&
            !cleanupArtifacts(*eventId, artifactDirectory)) {
            util::logWarn("Entrance artifacts retained after cleanup failure: "
                          "event=" + std::to_string(*eventId));
        }
    } else {
        failEvent(*eventId, key, "Entrance vehicle DB finalization failed");
        util::logError("Entrance vehicle DB finalization failed: camera=" +
                       key.cameraId + " channel=" + key.channelId + " object=" +
                       key.objectId);
    }
}

std::string EntranceVehicleService::imagePath(const BestShotEvent& event) const {
    const std::string taskDirectory =
        safeComponent(event.key.cameraId) + "_" +
        safeComponent(event.key.objectId) + "_" +
        std::to_string(event.receivedAtEpochMs);
    return (fs::path(outputRoot_) / ".work" /
            safeComponent(event.key.channelId) / taskDirectory /
            "plate.jpg").string();
}

bool EntranceVehicleService::cleanupArtifacts(
    const std::int64_t eventId, const std::string& artifactDirectory) {
    std::lock_guard cleanupLock(artifactCleanupMutex_);
    if (eventId < 0 || !ports_.markArtifactsDeleted) return false;
    if (artifactDirectory.empty())
        return ports_.markArtifactsDeleted(eventId);

    const fs::path root = fs::path(outputRoot_);
    const fs::path source = fs::path(artifactDirectory);
    if (!isInside(root, source)) {
        util::logError("Entrance artifact cleanup rejected path outside root: " +
                       source.string());
        return false;
    }
    std::error_code error;
    if (!fs::exists(source, error))
        return !error && ports_.markArtifactsDeleted(eventId);

    const fs::path trashRoot = root / ".trash";
    fs::create_directories(trashRoot, error);
    if (error) return false;
    const fs::path trash = trashRoot /
        ("event_" + std::to_string(eventId) + "_" +
         std::to_string(nowEpochMs()));
    fs::rename(source, trash, error);
    if (error) return false;

    if (!ports_.markArtifactsDeleted(eventId)) {
        std::error_code rollbackError;
        fs::rename(trash, source, rollbackError);
        return false;
    }
    fs::remove_all(trash, error);
    if (error) {
        util::logWarn("Entrance trash removal deferred: path=" + trash.string() +
                      " error=" + error.message());
    }
    return true;
}

void EntranceVehicleService::cleanupDueArtifacts() {
    if (!ports_.listArtifactsForCleanup) return;
    const std::int64_t failedBefore = nowEpochMs() - failureRetentionMs_;
    for (const auto& record : ports_.listArtifactsForCleanup(failedBefore)) {
        if (record.terminal_state == "COMPLETED" &&
            !deleteArtifactsOnSuccess_) {
            continue;
        }
        (void)cleanupArtifacts(record.event_id, artifactDirectory(record));
    }

    std::error_code ignored;
    const fs::path trashRoot = fs::path(outputRoot_) / ".trash";
    if (fs::exists(trashRoot, ignored)) {
        for (const auto& entry : fs::directory_iterator(trashRoot, ignored)) {
            fs::remove_all(entry.path(), ignored);
        }
    }
}

std::string EntranceVehicleService::artifactDirectory(
    const database::EntranceArtifactRecord& record) const {
    if (!record.plate_image_path.empty())
        return fs::path(record.plate_image_path).parent_path().string();
    if (!record.vision_result_path.empty()) {
        const fs::path result(record.vision_result_path);
        const fs::path parent = result.parent_path();
        return parent.filename() == "ev_analysis"
            ? parent.parent_path().string() : parent.string();
    }
    return {};
}

std::int64_t EntranceVehicleService::nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace entrance

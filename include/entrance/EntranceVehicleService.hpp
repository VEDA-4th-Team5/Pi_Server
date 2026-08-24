#pragma once

#include "entrance/EntranceBestShotCoordinator.hpp"
#include "entrance/EntranceEvWorker.hpp"
#include "entrance/EntranceImageDeduplicator.hpp"
#include "ocr/OcrWorker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace entrance {

struct EntrancePersistenceResult {
    bool success{};
    std::string classification;
};

struct EntranceVehiclePorts {
    std::function<bool(const std::string&, const std::string&,
                       const std::string&)> downloadImage;
    std::function<bool(const std::string&, const std::string&,
                       ocr::GenericOcrCallback)> enqueueOcr;
    std::function<bool(EntranceEvTask, EntranceEvCallback)> enqueueEvAnalysis;
    std::function<std::int64_t(const ObjectKey&, std::int64_t)> createEvent;
    std::function<bool(std::int64_t, BestShotKind, const std::string&)>
        saveImagePath;
    std::function<EntrancePersistenceResult(
        std::int64_t, const std::string&, double, int,
        const std::string&)> finishEvent;
    std::function<bool(std::int64_t, const EntranceEvResult&)>
        saveEvAnalysis;
    std::function<bool(std::int64_t)> incrementDuplicateCount;
    std::function<bool(std::int64_t)> markArtifactsDeleted;
    std::function<std::vector<database::EntranceArtifactRecord>(std::int64_t)>
        listArtifactsForCleanup;
};

/** 입구 객체의 이미지 저장, OCR 요청, DB 결과 반영을 조정한다. */
class EntranceVehicleService {
public:
    EntranceVehicleService(EntranceBestShotCoordinator& coordinator,
                           EntranceVehiclePorts ports,
                           std::string outputRoot,
                           std::chrono::milliseconds dedupWindow =
                               std::chrono::seconds(20),
                           int dedupHammingThreshold = 10,
                           std::size_t dedupCapacity = 64,
                           bool deleteArtifactsOnSuccess = true,
                           std::chrono::hours failureRetention =
                               std::chrono::hours(24));
    ~EntranceVehicleService();

    void start();
    void stop();
    bool handle(const BestShotEvent& event);

private:
    void cleanupLoop();
    void failEvent(std::int64_t eventId, const ObjectKey& key,
                   const std::string& reason);
    void handleOcrResult(const ObjectKey& key,
                         const std::string& artifactDirectory,
                         const ocr::GenericOcrResult& result);
    void handleEvResult(std::int64_t eventId, const ObjectKey& key,
                        const std::string& plateImagePath,
                        const std::string& artifactDirectory,
                        const EntranceEvResult& result);
    std::string imagePath(const BestShotEvent& event) const;
    bool cleanupArtifacts(std::int64_t eventId,
                          const std::string& artifactDirectory);
    void cleanupDueArtifacts();
    std::string artifactDirectory(
        const database::EntranceArtifactRecord& record) const;
    static std::int64_t nowEpochMs();

    EntranceBestShotCoordinator& coordinator_;
    EntranceVehiclePorts ports_;
    std::string outputRoot_;
    EntranceImageDeduplicator imageDeduplicator_;
    bool deleteArtifactsOnSuccess_;
    std::int64_t failureRetentionMs_;
    std::atomic<bool> running_{false};
    std::thread cleanupWorker_;
    std::mutex ingressMutex_;
    std::mutex artifactCleanupMutex_;
};

}  // namespace entrance

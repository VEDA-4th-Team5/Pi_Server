#pragma once

#include "bestshot/BestShotMetadata.hpp"
#include "camera/CameraChannel.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bestshot {

/**
 * Receives camera metadata and delegates all session authority to a durable
 * correlation lease.  No channel-local active-session cache is maintained.
 */
class BestShotReceiver {
public:
    using DownloadCallback = std::function<bool(
        const std::string& rtsp_url,
        const std::string& image_ref,
        const std::string& destination)>;
    using OcrCallback = std::function<void(
        int session_id,
        const std::string& slot_id,
        const std::string& image_path)>;

    BestShotReceiver(
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        parking::ParkingTriggerCoordinator& trigger_coordinator,
        ocr::OcrWorker& ocr_worker,
        std::atomic<bool>& running,
        std::string output_root = "data/bestshots",
        DownloadCallback downloader = {},
        OcrCallback ocr_callback = {},
        std::size_t pending_capacity = 64);
    ~BestShotReceiver();

    void start();
    void stop();

    /**
     * Testable production metadata boundary.  The RTSP receiver calls this
     * exact function after reconstructing one MetadataStream document.
     */
    std::vector<BestShotProcessResult> processMetadataDocument(
        const std::string& camera_id,
        const std::string& channel_id,
        const std::string& xml,
        const std::string& rtsp_url);

    /** Re-resolve bounded Pending metadata without camera/network input. */
    std::vector<BestShotProcessResult> reconcilePending();

private:
    struct PendingMetadata {
        std::string evidenceIdentity;
        std::string exactIdentityKey;
        std::string cameraId;
        std::string channelId;
        std::string objectId;
        std::string imageRef;
        std::string plateText;
        std::string rtspUrl;
        parking::BestShotEvidenceKind kind{
            parking::BestShotEvidenceKind::Vehicle};
        std::int64_t createdAtEpochMs{};
    };

    void receiveLoop(const std::shared_ptr<camera::CameraChannel>& channel);
    void pendingLoop();
    BestShotProcessResult processOne(PendingMetadata metadata,
                                     bool may_requeue);
    bool enqueuePending(PendingMetadata metadata);
    bool downloadImage(const std::string& rtsp_url,
                       const std::string& image_ref,
                       const std::string& destination);

    static std::string evidenceIdentity(const PendingMetadata& metadata);
    static std::string exactIdentityKey(const PendingMetadata& metadata);
    static std::string safePathComponent(const std::string& value);
    static std::string stableDigest(const std::string& value);

    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    parking::ParkingTriggerCoordinator& trigger_coordinator_;
    std::atomic<bool>& running_;
    std::string output_root_;
    DownloadCallback downloader_;
    OcrCallback ocr_callback_;
    std::size_t pending_capacity_;

    std::vector<std::thread> workers_;
    std::thread pending_worker_;
    std::atomic<bool> pending_worker_stop_{true};
    std::mutex lifecycle_mutex_;

    std::mutex pending_mutex_;
    std::condition_variable pending_condition_;
    std::deque<PendingMetadata> pending_metadata_;
    std::unordered_set<std::string> pending_identity_index_;
    std::unordered_set<std::string> in_flight_identity_index_;
};

}  // namespace bestshot

#pragma once

#include "camera/CameraChannel.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace snapshot {

struct NormalizedRoi {
    double x;
    double y;
    double width;
    double height;
};

struct StoredImagePair {
    std::string originalPath;
    std::string enhancedPath;
};

class SnapshotStorage {
public:
    SnapshotStorage(
        const std::string& snapshot_dir,
        int snapshot_frame_wait_ms,
        std::atomic<bool>& running
    );

    std::string saveFullSizeSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel
    );

    // 세션 없는 IVA 진단 ROI를 snapshots/<channel>/<slot>/events/iva에 저장한다.
    std::string saveIvaAreaSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel,
        const std::string& slot_id,
        const NormalizedRoi& roi
    );

    /** @brief 세션·증거 종류가 포함된 이름으로 최신 ROI 원본을 저장한다. */
    std::string saveEvidenceSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel,
        std::int64_t session_id,
        const std::string& slot_id,
        const std::string& evidence_reason,
        const NormalizedRoi& roi
    );

    /** @brief 30/60초 홀 촬영본을 실제 DB 세션 ID가 포함된 이름으로 저장한다. */
    std::string saveHallCaptureSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel,
        std::int64_t session_id,
        const std::string& slot_id,
        const std::string& capture_stage,
        const NormalizedRoi& roi
    );

    /**
     * @brief 카메라 CAP 전체 original/enhanced JPEG를 세션·단계별로 저장한다.
     * @note 실제 슬롯 좌표가 확정되기 전에는 ROI crop을 수행하지 않는다.
     */
    StoredImagePair saveCameraApiHallCapture(
        const std::string& channel_id,
        std::int64_t session_id,
        const std::string& slot_id,
        const std::string& capture_stage,
        const std::vector<unsigned char>& original_jpeg,
        const std::vector<unsigned char>& enhanced_jpeg
    );

private:
    std::string saveAreaSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel,
        const std::string& slot_id,
        const NormalizedRoi& roi,
        const std::string& filename_prefix,
        std::int64_t session_id = -1,
        const std::string& stage_directory = {});
    cv::Mat waitForFullFrame(const std::shared_ptr<camera::CameraChannel>& channel);
    std::string snapshot_dir_;
    int snapshot_frame_wait_ms_;
    std::atomic<bool>& running_;
    std::atomic<std::uint64_t> next_file_sequence_{0};
};

}

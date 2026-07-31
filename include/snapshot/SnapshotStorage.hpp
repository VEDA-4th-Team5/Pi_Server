#pragma once

#include "camera/CameraChannel.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace snapshot {

struct NormalizedRoi {
    double x;
    double y;
    double width;
    double height;
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

    // 최신 원본 프레임에서 IVA ROI를 잘라 snapshots/<channel>/<slot>/scene에 저장한다.
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

private:
    std::string saveAreaSnapshot(
        const std::shared_ptr<camera::CameraChannel>& channel,
        const std::string& slot_id,
        const NormalizedRoi& roi,
        const std::string& filename_prefix);
    cv::Mat waitForFullFrame(const std::shared_ptr<camera::CameraChannel>& channel);
    std::string snapshot_dir_;
    int snapshot_frame_wait_ms_;
    std::atomic<bool>& running_;
};

}

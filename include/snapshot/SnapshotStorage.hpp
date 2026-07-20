#pragma once

#include "camera/CameraChannel.hpp"

#include <atomic>
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

private:
    cv::Mat waitForFullFrame(const std::shared_ptr<camera::CameraChannel>& channel);
    std::string snapshot_dir_;
    int snapshot_frame_wait_ms_;
    std::atomic<bool>& running_;
};

}

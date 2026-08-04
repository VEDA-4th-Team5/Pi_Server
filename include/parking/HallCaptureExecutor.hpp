#pragma once

#include "camera/CameraChannel.hpp"
#include "camera/CameraSnapshotApiClient.hpp"
#include "parking/CaptureRequest.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace parking {

/** @brief MQTT 초안 발행과 실제 RTSP ROI 저장 결과를 분리하는 촬영 실행기다. */
class HallCaptureExecutor {
public:
    using DraftPublisher = std::function<bool(const CaptureRequest&)>;

    HallCaptureExecutor(
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        snapshot::SnapshotStorage& storage,
        HallCaptureCoordinator& coordinator,
        DraftPublisher draftPublisher,
        camera::CameraSnapshotApiClient* snapshotApiClient = nullptr,
        bool rtspFallback = false);

    /** @return 실제 RTSP 파일·DB 처리가 성공했거나 더 이상 재시도할 필요가 없으면 true. */
    bool execute(const CaptureRequest& request) noexcept;

private:
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    snapshot::SnapshotStorage& storage_;
    HallCaptureCoordinator& coordinator_;
    DraftPublisher draftPublisher_;
    camera::CameraSnapshotApiClient* snapshotApiClient_{};
    bool rtspFallback_{};
};

}  // namespace parking

#pragma once

#include "camera/CameraChannel.hpp"
#include "camera/CameraSnapshotApiClient.hpp"
#include "parking/CaptureRequest.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "parking/PlateIlluminator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace parking {

/** @brief MQTT 초안 발행과 실제 RTSP ROI 저장 결과를 분리하는 촬영 실행기다. */
class HallCaptureExecutor {
public:
    using DraftPublisher = std::function<bool(const CaptureRequest&)>;
    using RoiResolver = std::function<std::optional<snapshot::NormalizedRoi>(
        const std::string& slot_id)>;

    HallCaptureExecutor(
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        snapshot::SnapshotStorage& storage,
        HallCaptureCoordinator& coordinator,
        DraftPublisher draftPublisher,
        camera::CameraSnapshotApiClient* snapshotApiClient = nullptr,
        bool rtspFallback = false,
        PlateIlluminator* illuminator = nullptr,
        RoiResolver roiResolver = {});

    /** @return 실제 RTSP 파일·DB 처리가 성공했거나 더 이상 재시도할 필요가 없으면 true. */
    bool execute(const CaptureRequest& request) noexcept;

private:
    // 조명이 없거나 필요없으면 아무것도 하지 않는 빈 가드를 돌려준다.
    [[nodiscard]] PlateIlluminator::Scope illuminate(
        const CaptureRequest& request);

    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    snapshot::SnapshotStorage& storage_;
    HallCaptureCoordinator& coordinator_;
    DraftPublisher draftPublisher_;
    camera::CameraSnapshotApiClient* snapshotApiClient_{};
    bool rtspFallback_{};
    // 노출 구간에만 번호판 조명을 켠다. 없으면 조명 없이 촬영한다.
    PlateIlluminator* illuminator_{};
    RoiResolver roi_resolver_;
};

}  // namespace parking

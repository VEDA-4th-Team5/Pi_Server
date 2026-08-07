#include "parking/HallCaptureExecutor.hpp"

#include "util/Logger.hpp"

#include <filesystem>
#include <stdexcept>

namespace parking {
namespace {

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channelId) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channelId) return channel;
    return nullptr;
}

}  // namespace

HallCaptureExecutor::HallCaptureExecutor(
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    snapshot::SnapshotStorage& storage,
    HallCaptureCoordinator& coordinator,
    DraftPublisher draftPublisher,
    camera::CameraSnapshotApiClient* snapshotApiClient,
    const bool rtspFallback, PlateIlluminator* illuminator)
    : channels_(channels), storage_(storage), coordinator_(coordinator),
      draftPublisher_(std::move(draftPublisher)),
      snapshotApiClient_(snapshotApiClient), rtspFallback_(rtspFallback),
      illuminator_(illuminator) {}

PlateIlluminator::Scope HallCaptureExecutor::illuminate(
    const CaptureRequest& request) {
    if (illuminator_ == nullptr) return {};
    return illuminator_->illuminate(request);
}

bool HallCaptureExecutor::execute(const CaptureRequest& request) noexcept {
    try {
        std::size_t consumed{};
        const std::int64_t sessionId = std::stoll(request.sessionId, &consumed);
        if (consumed != request.sessionId.size() || sessionId < 0) {
            util::logError("invalid SQLite capture session id: " +
                           request.sessionId);
            return false;
        }
        const auto channel = findChannel(channels_, request.target.channelId);
        if (!channel) {
            util::logError("capture channel is not configured: " +
                           request.target.channelId);
            return false;
        }
        const CaptureStage stage = toStage(request.reason);

        if (snapshotApiClient_ != nullptr) {
            camera::CameraGeneratedImages generated;
            bool captured = false;
            {
                // 저장·DB 기록은 조명이 필요없고, 길어지면 STM32 페일세이프
                // 타이머보다 점등이 오래 걸린다. 노출 호출만 감싼다.
                const auto lamp = illuminate(request);
                captured = snapshotApiClient_->generate(
                    request.target.snapshotApiChannel, generated);
            }
            if (captured) {
                const auto paths = storage_.saveCameraApiHallCapture(
                    request.target.channelId, sessionId, request.slotId,
                    toEnhancementType(stage), generated.originalJpeg,
                    generated.enhancedJpeg);
                if (paths.originalPath.empty() || paths.enhancedPath.empty())
                    return false;

                const auto result = coordinator_.onCaptureImage(
                    {sessionId, request.slotId, stage, paths.originalPath,
                     paths.enhancedPath});
                if (result == CaptureImageResult::Stored) {
                    util::logLine(
                        "CAMERA_SNAPSHOT_API",
                        "capture stored slot=" + request.slotId +
                        " session=" + request.sessionId + " channel=" +
                        std::to_string(request.target.snapshotApiChannel) +
                        " run_id=" + generated.runId + " environment=" +
                        generated.detectedEnvironment + " filter=" +
                        generated.autoFilter);
                    return true;
                }

                std::error_code ignored;
                std::filesystem::remove(paths.originalPath, ignored);
                std::filesystem::remove(paths.enhancedPath, ignored);
                return result == CaptureImageResult::Duplicate ||
                       result == CaptureImageResult::InactiveSession;
            }
            util::logError(
                "camera snapshot API capture failed slot=" + request.slotId +
                " session=" + request.sessionId + " channel=" +
                std::to_string(request.target.snapshotApiChannel) +
                " error=" + snapshotApiClient_->lastError());
            if (!rtspFallback_) return false;
            util::logWarn("falling back to RTSP hall capture slot=" +
                          request.slotId + " session=" + request.sessionId);
        }

        const bool mqttPublished = draftPublisher_ && draftPublisher_(request);
        if (!mqttPublished) {
            util::logWarn("capture MQTT draft publish failed; local RTSP "
                          "capture continues slot=" + request.slotId +
                          " session=" + request.sessionId);
        }
        const snapshot::NormalizedRoi roi{
            request.target.roiX, request.target.roiY,
            request.target.roiWidth, request.target.roiHeight};
        std::string path;
        {
            const auto lamp = illuminate(request);
            path = storage_.saveHallCaptureSnapshot(
                channel, sessionId, request.slotId, toEnhancementType(stage),
                roi);
        }
        if (path.empty()) return false;

        const auto result = coordinator_.onCaptureImage(
            {sessionId, request.slotId, stage, path, {}});
        if (result == CaptureImageResult::Stored) return true;

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return result == CaptureImageResult::Duplicate ||
               result == CaptureImageResult::InactiveSession;
    } catch (const std::exception& error) {
        util::logError("hall capture execution failed: " +
                       std::string(error.what()));
        return false;
    } catch (...) {
        util::logError("hall capture execution failed: unknown error");
        return false;
    }
}

}  // namespace parking

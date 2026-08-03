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
    DraftPublisher draftPublisher)
    : channels_(channels), storage_(storage), coordinator_(coordinator),
      draftPublisher_(std::move(draftPublisher)) {}

bool HallCaptureExecutor::execute(const CaptureRequest& request) noexcept {
    try {
        const bool mqttPublished = draftPublisher_ && draftPublisher_(request);
        if (!mqttPublished) {
            util::logWarn("capture MQTT draft publish failed; local RTSP "
                          "capture continues slot=" + request.slotId +
                          " session=" + request.sessionId);
        }

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
        const snapshot::NormalizedRoi roi{
            request.target.roiX, request.target.roiY,
            request.target.roiWidth, request.target.roiHeight};
        std::string path = storage_.saveHallCaptureSnapshot(
            channel, sessionId, request.slotId, toEnhancementType(stage), roi);
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

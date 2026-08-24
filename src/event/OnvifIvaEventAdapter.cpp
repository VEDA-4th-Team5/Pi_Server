#include "event/OnvifIvaEventAdapter.hpp"

#include "event/CameraEvent.hpp"
#include "event/IvaEventResolver.hpp"
#include "util/TimeUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace event {
namespace {

std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}

std::string stableHash(const std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

void setError(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

}  // namespace

std::optional<IvaOccupancySignal> OnvifIvaEventAdapter::adapt(
    const camera::OnvifIvaEvent& source,
    const app::AppConfig& config,
    const std::vector<parking::ParkingSlotConfig>& slots,
    std::string* error) {
    const std::string action = upperCopy(source.action);
    IvaOccupancyAction occupancyAction{IvaOccupancyAction::Unsupported};
    if (action == "INTRUSION") occupancyAction = IvaOccupancyAction::Intrusion;
    else if (action == "EXIT") occupancyAction = IvaOccupancyAction::Exit;
    else {
        setError(error, "ONVIF IvaArea action is not INTRUSION or EXIT");
        return std::nullopt;
    }
    if (source.utcTime.empty()) {
        setError(error, "ONVIF IvaArea UtcTime is missing");
        return std::nullopt;
    }
    const auto occurredAt = util::parseIso8601Utc(source.utcTime);
    if (!occurredAt) {
        setError(error, "ONVIF IvaArea UtcTime is invalid");
        return std::nullopt;
    }

    CameraEvent cameraEvent;
    cameraEvent.raw_topic =
        "onvif-pullpoint/OpenApp/WiseAI/IvaArea/&" +
        source.videoSourceToken + "/" + source.ruleName;
    cameraEvent.timestamp = source.utcTime;
    cameraEvent.timestamp_from_source = true;
    cameraEvent.video_source_token = source.videoSourceToken;
    cameraEvent.rule_name = source.ruleName;
    cameraEvent.action = action;
    cameraEvent.object_id = source.objectId;
    cameraEvent.is_iva_area_event = true;
    cameraEvent.is_active = action == "INTRUSION";

    std::string mappingError;
    const auto target = IvaEventResolver::resolve(
        config.camera_id, cameraEvent, slots, config.iva_areas, &mappingError);
    if (!target) {
        setError(error, "ONVIF IvaArea mapping failed: " + mappingError);
        return std::nullopt;
    }

    const bool cameraAuthoritative =
        config.parking_occupancy_source == "CAMERA_IVA" ||
        config.parking_occupancy_source == "HYBRID_OR";
    if (!cameraAuthoritative) {
        if (config.parking_occupancy_source != "HALL" ||
            occupancyAction != IvaOccupancyAction::Intrusion ||
            source.objectId.empty()) {
            setError(error,
                     "ONVIF IvaArea is not accepted by occupancy policy");
            return std::nullopt;
        }
    }

    IvaOccupancySignal signal;
    signal.slotId = target->slotId;
    signal.cameraId = config.camera_id;
    signal.videoSourceToken = target->observationVideoSourceToken;
    signal.ruleName = target->ruleName;
    signal.objectId = source.objectId;
    signal.action = occupancyAction;
    signal.authoritativeExit = cameraAuthoritative;
    signal.occurredAtFromSource = true;
    signal.occurredAt = *occurredAt;
    signal.channelId = target->channelId;
    signal.occupancyAuthority = cameraAuthoritative;
    signal.sourceTransport = "camera-onvif";
    signal.sourceIdentity = "camera-onvif:" + stableHash(
        config.camera_id + "|" + source.videoSourceToken + "|" +
        source.ruleName + "|" + action + "|" + source.utcTime + "|" +
        source.objectId);
    return signal;
}

}  // namespace event

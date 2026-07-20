#include "event/EventPayloadBuilder.hpp"

#include "util/StringUtil.hpp"

#include <sstream>

namespace event {

std::string EventPayloadBuilder::buildJson(
    const std::string& camera_id,
    const std::string& channel_id,
    const CameraEvent& event,
    const std::string& snapshot_path
) {
    std::ostringstream oss;

    oss << "{";
    oss << "\"camera_id\":\"" << util::jsonEscape(camera_id) << "\",";
    oss << "\"channel_id\":\"" << util::jsonEscape(channel_id) << "\",";
    oss << "\"event_channel_id\":\"" << util::jsonEscape(event.event_channel_id) << "\",";
    oss << "\"source_type\":\"" << util::jsonEscape(event.source_type) << "\",";
    oss << "\"source_id\":\"" << util::jsonEscape(event.source_id) << "\",";
    oss << "\"event_type\":\"" << util::jsonEscape(event.event_type) << "\",";
    oss << "\"severity\":\"" << util::jsonEscape(event.severity) << "\",";
    oss << "\"active\":" << (event.is_active ? "true" : "false") << ",";
    oss << "\"iva_area_id\":\"" << util::jsonEscape(event.iva_area_id) << "\",";
    oss << "\"slot_id\":\"" << util::jsonEscape(event.slot_id) << "\",";
    oss << "\"snapshot_mode\":\""
        << (event.is_iva_area_event ? "iva_area_roi" : "all_channels_full_size")
        << "\",";
    oss << "\"snapshot_path\":\"" << util::jsonEscape(snapshot_path) << "\",";
    oss << "\"clip_path\":\"\",";
    oss << "\"ack_state\":\"unacked\",";
    oss << "\"timestamp\":\"" << util::jsonEscape(event.timestamp) << "\",";
    oss << "\"raw_topic\":\"" << util::jsonEscape(event.raw_topic) << "\",";
    oss << "\"raw_payload\":\"" << util::jsonEscape(event.raw_payload) << "\"";
    oss << "}";

    return oss.str();
}

}

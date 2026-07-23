#include "event/EventPayloadBuilder.hpp"

#include "util/StringUtil.hpp"
#include "util/TimeUtil.hpp"

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

std::string EventPayloadBuilder::buildFireJson(
    const std::string& camera_id,
    const std::string& channel_id,
    const std::string& slot_id,
    const FireSignal& signal
) {
    std::ostringstream oss;

    // 카메라 이벤트가 아니므로 camera/IVA 관련 필드는 빈 값으로 둔다.
    // Qt 파서가 필드 존재 자체를 기대하므로 필드를 빼지는 않는다.
    oss << "{";
    oss << "\"camera_id\":\"" << util::jsonEscape(camera_id) << "\",";
    oss << "\"channel_id\":\"" << util::jsonEscape(channel_id) << "\",";
    oss << "\"event_channel_id\":\"\",";
    oss << "\"source_type\":\"sensor_uart\",";
    oss << "\"source_id\":\"" << util::jsonEscape(signal.sensorId) << "\",";
    oss << "\"event_type\":\""
        << (signal.detected ? "sensor_fire_suspected"
                            : "sensor_fire_cleared")
        << "\",";
    // 화재는 확정이 아니라 후보다. 확정은 관제실이 한다.
    oss << "\"severity\":\"" << (signal.detected ? "critical" : "info")
        << "\",";
    oss << "\"active\":" << (signal.detected ? "true" : "false") << ",";
    oss << "\"iva_area_id\":\"\",";
    oss << "\"slot_id\":\"" << util::jsonEscape(slot_id) << "\",";
    oss << "\"snapshot_mode\":\"none\",";
    oss << "\"snapshot_path\":\"\",";
    oss << "\"clip_path\":\"\",";
    oss << "\"ack_state\":\"unacked\",";
    oss << "\"timestamp\":\""
        << util::jsonEscape(util::isoString(signal.occurredAt)) << "\",";
    oss << "\"raw_topic\":\"" << util::jsonEscape(signal.sourceTransport)
        << "\",";
    oss << "\"raw_payload\":\"" << util::jsonEscape(signal.rawPayload) << "\"";
    oss << "}";

    return oss.str();
}

}

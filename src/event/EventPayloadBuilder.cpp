#include "event/EventPayloadBuilder.hpp"

#include "util/StringUtil.hpp"
#include "util/TimeUtil.hpp"

#include <sstream>

namespace event {
namespace {

std::string buildFireEventId(const FireSignal& signal) {
    std::ostringstream output;
    output << "fire-" << signal.sensorId << '-';
    if (signal.sourceSequence) {
        output << *signal.sourceSequence;
    } else {
        const auto epoch_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            signal.occurredAt.time_since_epoch()).count();
        output << (signal.detected ? "DETECTED-" : "CLEARED-") << epoch_ms;
    }
    return output.str();
}

}  // namespace

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
    const FireSignal& signal
) {
    std::ostringstream oss;

    // 시연에서는 FLAME01~04 가짜 입력을 ch01~04 독립 상태로 연결한다.
    oss << "{";
    oss << "\"event_id\":\""
        << util::jsonEscape(buildFireEventId(signal)) << "\",";
    oss << "\"camera_id\":\"" << util::jsonEscape(camera_id) << "\",";
    oss << "\"channel_id\":\"" << util::jsonEscape(channel_id) << "\",";
    oss << "\"event_channel_id\":\"\",";
    oss << "\"source_type\":\"sensor_uart\",";
    oss << "\"source_id\":\"" << util::jsonEscape(signal.sensorId) << "\",";
    oss << "\"event_type\":\""
        << (signal.detected ? "FIRE_SUSPECTED" : "FIRE_CLEARED")
        << "\",";
    oss << "\"alarm_kind\":\""
        << (signal.detected ? "FIRE_SUSPECTED" : "NONE") << "\",";
    // Qt의 기존 alarm 필드와 신규 alarm_kind 필드를 함께 제공한다.
    oss << "\"alarm\":\""
        << (signal.detected ? "FIRE_SUSPECTED" : "NONE") << "\",";
    oss << "\"alarm_state\":\""
        << (signal.detected ? "OPEN" : "RESOLVED") << "\",";
    // 화재는 확정이 아니라 후보다. 확정은 관제실이 한다.
    oss << "\"severity\":\"" << (signal.detected ? "critical" : "info")
        << "\",";
    oss << "\"active\":" << (signal.detected ? "true" : "false") << ",";
    oss << "\"scope\":\"CAMERA_CHANNEL\",";
    oss << "\"zone_id\":\"\",";
    oss << "\"iva_area_id\":\"\",";
    oss << "\"slot_id\":\"\",";
    oss << "\"snapshot_mode\":\"none\",";
    oss << "\"snapshot_path\":\"\",";
    oss << "\"clip_path\":\"\",";
    oss << "\"ack_state\":\""
        << (signal.detected ? "unacked" : "resolved") << "\",";
    oss << "\"timestamp\":\""
        << util::jsonEscape(util::isoString(signal.occurredAt)) << "\",";
    oss << "\"raw_topic\":\"" << util::jsonEscape(signal.sourceTransport)
        << "\",";
    oss << "\"raw_payload\":\"" << util::jsonEscape(signal.rawPayload) << "\"";
    oss << "}";

    return oss.str();
}

}

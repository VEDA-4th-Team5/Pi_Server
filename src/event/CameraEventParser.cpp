#include "event/CameraEventParser.hpp"

#include "util/TimeUtil.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace {

std::string lowerCopy(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool parseActiveState(const std::string& payload) {
    const std::string value = lowerCopy(payload);
    // ONVIF XML과 JSON 양쪽의 대표적인 비활성 표현을 처리한다.
    return value.find("value=\"false\"") == std::string::npos &&
           value.find("value='false'") == std::string::npos &&
           value.find("\"active\":false") == std::string::npos &&
           value.find("\"state\":false") == std::string::npos &&
           value.find(">false<") == std::string::npos;
}

}

namespace event {

CameraEvent CameraEventParser::parse(
    const std::string& raw_topic,
    const std::string& raw_payload,
    const std::string& default_channel_id
) {
    CameraEvent event;

    event.raw_topic = raw_topic;
    event.raw_payload = raw_payload;
    event.timestamp = util::nowIsoString();

    // payload보다 ONVIF topic 경로에 이벤트 종류와 채널 정보가 들어 있다.
    event.event_channel_id = parseEventChannelId(raw_topic, default_channel_id);
    event.source_type = "camera_mqtt";
    event.source_id = parseSourceId(raw_topic);

    event.event_type = parseEventType(raw_topic, raw_payload);
    event.is_active = parseActiveState(raw_payload);
    event.is_iva_area_event = event.event_type == "camera_iva_area_enter" ||
                              event.event_type == "camera_iva_area_intrusion" ||
                              event.event_type == "camera_iva_area_occupied";
    event.severity = parseSeverity(event.event_type);

    return event;
}

std::string CameraEventParser::parseSourceId(const std::string& topic) {
    std::size_t slash = topic.find('/');

    if (slash == std::string::npos) {
        return topic;
    }

    return topic.substr(0, slash);
}

std::string CameraEventParser::parseEventChannelId(
    const std::string& topic,
    const std::string& default_channel_id
) {
    std::string key = "VideoSourceToken-";
    std::size_t pos = topic.find(key);

    if (pos == std::string::npos) {
        return default_channel_id;
    }

    std::size_t idx_pos = pos + key.size();

    if (idx_pos >= topic.size() || !std::isdigit(static_cast<unsigned char>(topic[idx_pos]))) {
        return default_channel_id;
    }

    // ONVIF token은 0부터, 서버 채널 표기는 1부터 시작한다(0 -> ch01).
    int token_index = topic[idx_pos] - '0';
    int channel_number = token_index + 1;

    std::ostringstream oss;
    oss << "ch" << std::setw(2) << std::setfill('0') << channel_number;

    return oss.str();
}

std::string CameraEventParser::parseEventType(const std::string& topic,
                                              const std::string& payload) {
    const std::string combined = lowerCopy(topic + "\n" + payload);

    // Firmware에 따라 topic 또는 payload에 IVA/VirtualArea 정보가 들어갈 수 있다.
    const bool area = combined.find("virtualarea") != std::string::npos ||
                      combined.find("virtual area") != std::string::npos ||
                      combined.find("iva") != std::string::npos ||
                      combined.find("fielddetector") != std::string::npos ||
                      combined.find("objectsinside") != std::string::npos ||
                      combined.find("regiondetector") != std::string::npos;
    if (area && combined.find("enter") != std::string::npos)
        return "camera_iva_area_enter";
    if (area && combined.find("intrusion") != std::string::npos)
        return "camera_iva_area_intrusion";
    if (area && (combined.find("objectsinside") != std::string::npos ||
                 combined.find("inside") != std::string::npos))
        return "camera_iva_area_occupied";
    // WiseAI firmware는 상태를 payload에 두고 topic은 IvaArea/name1처럼만 보낸다.
    // 활성/비활성 구분은 parseActiveState()가 담당하므로 이 topic도 IVA로 정규화한다.
    if (combined.find("ivaarea") != std::string::npos)
        return "camera_iva_area_occupied";

    if (topic.find("MotionAlarm") != std::string::npos) {
        return "camera_motion_alarm";
    }

    if (topic.find("MotionDetection") != std::string::npos) {
        return "camera_motion_detection";
    }

    if (topic.find("ObjectDetection") != std::string::npos) {
        return "camera_object_detection";
    }

    if (topic.find("DigitalInput") != std::string::npos) {
        return "camera_digital_input";
    }

    if (topic.find("Relay") != std::string::npos) {
        return "camera_relay";
    }

    if (topic.find("ImageTooBlurry") != std::string::npos) {
        return "camera_image_too_blurry";
    }

    if (topic.find("Tampering") != std::string::npos) {
        return "camera_tampering";
    }

    return "camera_event";
}

std::string CameraEventParser::parseSeverity(const std::string& event_type) {
    if (event_type == "camera_tampering") {
        return "warning";
    }

    if (event_type == "camera_digital_input") {
        return "warning";
    }

    if (event_type == "camera_image_too_blurry") {
        return "warning";
    }

    return "info";
}

}

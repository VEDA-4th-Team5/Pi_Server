#include "event/CameraEventParser.hpp"

#include "util/TimeUtil.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <vector>

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

std::string canonicalVideoSourceToken(std::string token) {
    token = lowerCopy(std::move(token));
    constexpr const char* longPrefix = "videosourcetoken-";
    if (token.rfind(longPrefix, 0) == 0)
        token = "vs-" + token.substr(std::char_traits<char>::length(longPrefix));
    if (token.rfind("vs-", 0) != 0 || token.size() <= 3) return {};
    for (std::size_t index = 3; index < token.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(token[index]))) return {};
    }
    return token;
}

std::string upperCopy(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}

void parseSmartParkingIva(const std::string& payload,
                          event::CameraEvent& event) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }
    if (!body.is_object() ||
        body.value("schema", std::string{}) != "smart-parking-iva-v1") {
        return;
    }

    event.is_smart_parking_iva = true;
    auto reject = [&event](std::string message) {
        event.protocol_valid = false;
        event.protocol_error = std::move(message);
    };
    for (const auto* key : {"camera_id", "video_source_token", "rule_name",
                            "slot_id", "event_type", "action"}) {
        if (!body.contains(key) || !body[key].is_string() ||
            body[key].get_ref<const std::string&>().empty()) {
            reject(std::string("missing or invalid ") + key);
            return;
        }
    }
    if (!body.contains("active") || !body["active"].is_boolean()) {
        reject("missing or invalid active");
        return;
    }

    event.declared_camera_id = body["camera_id"].get<std::string>();
    event.rule_name = body["rule_name"].get<std::string>();
    event.slot_id = body["slot_id"].get<std::string>();
    event.action = upperCopy(body["action"].get<std::string>());
    if (body.contains("object_id")) {
        if (body["object_id"].is_string()) {
            event.object_id = body["object_id"].get<std::string>();
        } else if (body["object_id"].is_number_integer() ||
                   body["object_id"].is_number_unsigned()) {
            event.object_id = body["object_id"].dump();
        }
    }
    event.is_active = body["active"].get<bool>();
    const std::string declaredToken = canonicalVideoSourceToken(
        body["video_source_token"].get<std::string>());
    if (declaredToken.empty()) {
        reject("invalid video_source_token");
        return;
    }
    if (!event.video_source_token.empty() &&
        event.video_source_token != declaredToken) {
        reject("topic/payload video_source_token mismatch");
        return;
    }
    event.video_source_token = declaredToken;

    if (upperCopy(body["event_type"].get<std::string>()) != "IVA_AREA") {
        reject("unsupported event_type");
        return;
    }
    if (event.action == "ENTER" && event.is_active) {
        event.event_type = "camera_iva_area_enter";
    } else if (event.action == "INTRUSION" && event.is_active) {
        event.event_type = "camera_iva_area_intrusion";
    } else if (event.action == "EXIT" && !event.is_active) {
        event.event_type = "camera_iva_area_exit";
    } else {
        reject("action and active are inconsistent");
    }
}

void parseWiseAiIvaJson(const std::string& topic,
                        const std::string& payload,
                        event::CameraEvent& event) {
    if (lowerCopy(topic).find("ivaarea") == std::string::npos) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }
    if (!body.is_object() || !body.contains("Source") ||
        !body["Source"].is_object() || !body.contains("Data") ||
        !body["Data"].is_object()) {
        return;
    }

    const auto& source = body["Source"];
    const auto& data = body["Data"];
    if (!data.contains("Action") || !data["Action"].is_string()) return;

    const std::string action = upperCopy(data["Action"].get<std::string>());
    if (action != "ENTER" && action != "INTRUSION" && action != "EXIT") {
        return;
    }

    event.action = action;
    if (source.contains("RuleName") && source["RuleName"].is_string()) {
        event.rule_name = source["RuleName"].get<std::string>();
    }
    if (source.contains("VideoSourceToken") &&
        source["VideoSourceToken"].is_string()) {
        const std::string payloadToken = canonicalVideoSourceToken(
            source["VideoSourceToken"].get<std::string>());
        if (!payloadToken.empty() && !event.video_source_token.empty() &&
            payloadToken != event.video_source_token) {
            event.protocol_valid = false;
            event.protocol_error =
                "topic/payload video_source_token mismatch";
            return;
        }
        if (!payloadToken.empty()) event.video_source_token = payloadToken;
    }
    if (data.contains("ObjectId")) {
        if (data["ObjectId"].is_string()) {
            event.object_id = data["ObjectId"].get<std::string>();
        } else if (data["ObjectId"].is_number_integer() ||
                   data["ObjectId"].is_number_unsigned()) {
            event.object_id = data["ObjectId"].dump();
        }
    }
    if (body.contains("UtcTime") && body["UtcTime"].is_string()) {
        event.timestamp = body["UtcTime"].get<std::string>();
        event.timestamp_from_source = true;
    }

    if (action == "INTRUSION") {
        event.event_type = "camera_iva_area_intrusion";
        event.is_active = true;
    } else if (action == "EXIT") {
        // WiseAI의 State=true는 Exit 이벤트가 발생했다는 뜻이지 슬롯이
        // 활성이라는 뜻이 아니다. 점유 상태는 Action으로 정규화한다.
        event.event_type = "camera_iva_area_exit";
        event.is_active = false;
    } else {
        event.event_type = "camera_iva_area_enter";
        event.is_active = true;
    }
}

void inferIvaAction(event::CameraEvent& event) {
    if (!event.action.empty()) return;
    if (event.event_type == "camera_iva_area_intrusion") {
        event.action = "INTRUSION";
    } else if (event.event_type == "camera_iva_area_exit") {
        event.action = "EXIT";
    } else if (event.event_type == "camera_iva_area_enter") {
        event.action = "ENTER";
    }
}

std::vector<std::string> splitNotificationMessages(
    const std::string& payload) {
    std::vector<std::string> messages;
    const std::string lower = lowerCopy(payload);
    std::size_t searchPosition{};

    while (searchPosition < lower.size()) {
        const std::size_t namePosition =
            lower.find("notificationmessage", searchPosition);
        if (namePosition == std::string::npos) break;

        const std::size_t tagStart = lower.rfind('<', namePosition);
        if (tagStart == std::string::npos || tagStart + 1 >= lower.size() ||
            lower[tagStart + 1] == '/') {
            searchPosition = namePosition + 1;
            continue;
        }

        std::size_t tagEnd = tagStart + 1;
        while (tagEnd < lower.size() &&
               !std::isspace(static_cast<unsigned char>(lower[tagEnd])) &&
               lower[tagEnd] != '>' && lower[tagEnd] != '/') {
            ++tagEnd;
        }
        if (tagEnd == tagStart + 1) {
            searchPosition = namePosition + 1;
            continue;
        }

        const std::string tagName = lower.substr(tagStart + 1,
                                                 tagEnd - tagStart - 1);
        const std::string closingTag = "</" + tagName + ">";
        const std::size_t closingStart = lower.find(closingTag, tagEnd);
        if (closingStart == std::string::npos) break;

        const std::size_t messageEnd = closingStart + closingTag.size();
        messages.push_back(payload.substr(tagStart, messageEnd - tagStart));
        searchPosition = messageEnd;
    }
    return messages;
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
    event.video_source_token = parseVideoSourceToken(raw_topic);
    event.event_channel_id = parseEventChannelId(raw_topic, default_channel_id);
    event.source_type = "camera_mqtt";
    event.source_id = parseSourceId(raw_topic);

    event.event_type = parseEventType(raw_topic, raw_payload);
    event.is_active = parseActiveState(raw_payload);
    parseSmartParkingIva(raw_payload, event);
    if (!event.is_smart_parking_iva) {
        parseWiseAiIvaJson(raw_topic, raw_payload, event);
    }
    inferIvaAction(event);
    event.is_iva_area_event = event.event_type == "camera_iva_area_enter" ||
                              event.event_type == "camera_iva_area_exit" ||
                              event.event_type == "camera_iva_area_intrusion" ||
                              event.event_type == "camera_iva_area_occupied";
    event.severity = parseSeverity(event.event_type);

    return event;
}

std::vector<CameraEvent> CameraEventParser::parseMany(
    const std::string& raw_topic,
    const std::string& raw_payload,
    const std::string& default_channel_id) {
    const auto notificationMessages =
        splitNotificationMessages(raw_payload);
    if (notificationMessages.empty()) {
        return {parse(raw_topic, raw_payload, default_channel_id)};
    }

    std::vector<CameraEvent> events;
    events.reserve(notificationMessages.size());
    for (const auto& notification : notificationMessages) {
        events.push_back(parse(raw_topic, notification, default_channel_id));
    }
    return events;
}

std::string CameraEventParser::parseVideoSourceToken(
    const std::string& topic) {
    const std::string lower = lowerCopy(topic);
    const std::string longKey = "videosourcetoken-";
    const std::string shortKey = "vs-";
    std::size_t position = lower.find(longKey);
    std::size_t indexPosition{};
    if (position != std::string::npos) {
        indexPosition = position + longKey.size();
    } else {
        position = lower.find(shortKey);
        if (position == std::string::npos) return {};
        indexPosition = position + shortKey.size();
    }

    if (indexPosition >= lower.size() ||
        !std::isdigit(static_cast<unsigned char>(lower[indexPosition]))) {
        return {};
    }
    std::size_t end = indexPosition;
    while (end < lower.size() &&
           std::isdigit(static_cast<unsigned char>(lower[end]))) {
        ++end;
    }
    return "vs-" + lower.substr(indexPosition, end - indexPosition);
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
    const std::string token = parseVideoSourceToken(topic);
    if (token.empty()) return default_channel_id;

    // ONVIF token은 0부터, 서버 채널 표기는 1부터 시작한다(0 -> ch01).
    int token_index{};
    try {
        token_index = std::stoi(token.substr(3));
    } catch (...) {
        return default_channel_id;
    }
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
    if (area && combined.find("exit") != std::string::npos)
        return "camera_iva_area_exit";
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

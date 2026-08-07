#pragma once

#include "event/CameraEvent.hpp"

#include <string>
#include <vector>

namespace event {

class CameraEventParser {
public:
    static CameraEvent parse(
        const std::string& raw_topic,
        const std::string& raw_payload,
        const std::string& default_channel_id
    );

    /** @brief 한 MQTT payload의 NotificationMessage들을 개별 이벤트로 분리한다. */
    static std::vector<CameraEvent> parseMany(
        const std::string& raw_topic,
        const std::string& raw_payload,
        const std::string& default_channel_id
    );

private:
    static std::string parseSourceId(const std::string& topic);
    static std::string parseVideoSourceToken(const std::string& topic);
    static std::string parseEventChannelId(const std::string& topic, const std::string& default_channel_id);
    static std::string parseEventType(const std::string& topic, const std::string& payload);
    static std::string parseSeverity(const std::string& event_type);
};

}

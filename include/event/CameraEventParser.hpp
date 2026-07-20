#pragma once

#include "event/CameraEvent.hpp"

#include <string>

namespace event {

class CameraEventParser {
public:
    static CameraEvent parse(
        const std::string& raw_topic,
        const std::string& raw_payload,
        const std::string& default_channel_id
    );

private:
    static std::string parseSourceId(const std::string& topic);
    static std::string parseEventChannelId(const std::string& topic, const std::string& default_channel_id);
    static std::string parseEventType(const std::string& topic, const std::string& payload);
    static std::string parseSeverity(const std::string& event_type);
};

}

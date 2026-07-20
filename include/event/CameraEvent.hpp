#pragma once

#include <string>

namespace event {

struct CameraEvent {
    std::string raw_topic;
    std::string raw_payload;

    std::string timestamp;

    std::string event_channel_id;
    std::string source_type;
    std::string source_id;

    std::string event_type;
    std::string severity;
    bool is_iva_area_event{false};
    bool is_active{true};
    std::string iva_area_id;
    std::string slot_id;
};

}

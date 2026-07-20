#pragma once

#include "event/CameraEvent.hpp"

#include <string>

namespace event {

class EventPayloadBuilder {
public:
    static std::string buildJson(
        const std::string& camera_id,
        const std::string& channel_id,
        const CameraEvent& event,
        const std::string& snapshot_path
    );
};

}

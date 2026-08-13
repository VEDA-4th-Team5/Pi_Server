#pragma once

#include "event/CameraEvent.hpp"
#include "event/FireAlarmEvent.hpp"
#include "parking/AppliedParkingRoi.hpp"

#include <cstdint>
#include <string>

namespace event {

class EventPayloadBuilder {
public:
    static std::string buildJson(
        const std::string& camera_id,
        const std::string& channel_id,
        const CameraEvent& event,
        const std::string& snapshot_path,
        const parking::AppliedParkingRoi* applied_roi = nullptr
    );

    // 화재 후보도 Qt 가 이미 파싱 중인 카메라 이벤트와 같은 필드를 쓴다.
    // 필드가 갈라지면 Qt 쪽 파서가 두 벌이 되므로 여기 한 곳에서만 만든다.
    static std::string buildFireJson(
        const std::string& camera_id,
        const std::string& channel_id,
        const FireSignal& signal,
        FireAlarmLifecycle lifecycle,
        const std::string& event_id,
        const std::string& alarm_id,
        std::uint64_t fire_revision,
        const std::string& delivery_id
    );

    static std::string buildFireEventId(const FireSignal& signal);
};

}

#pragma once

#include <string>

namespace event {

struct CameraEvent {
    std::string raw_topic;
    std::string raw_payload;

    std::string timestamp;

    std::string event_channel_id;
    // 카메라 MQTT topic에서 읽은 canonical token("vs-0" 형식)이다.
    // 토큰을 읽지 못한 이벤트는 IVA 슬롯 매핑에 사용하지 않는다.
    std::string video_source_token;
    // parking_slots.json의 camera binding으로 확정된 IVA Rule 이름이다.
    std::string rule_name;
    // smart-parking-iva-v1 고정 Publication payload에서 선언한 값이다.
    std::string declared_camera_id;
    std::string action;
    // 동일 차량의 WiseAI Exit/Intrusion 순서를 상관관계로 추적할 때 사용한다.
    std::string object_id;
    bool is_smart_parking_iva{false};
    bool protocol_valid{true};
    std::string protocol_error;
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

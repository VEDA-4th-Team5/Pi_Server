#pragma once

#include "parking_timer/Types.hpp"

#include <mutex>
#include <string>
#include <string_view>

namespace parking_timer {

// 실제 통합 시 이 클래스의 출력부를 MQTT/HTTP/WebSocket 어댑터로 교체한다.
class EventManager {
public:
    void publish(std::string_view event_type,
                 std::string_view slot_id,
                 std::string_view car_number,
                 std::string_view occurred_at,
                 std::string_view detail = {});

private:
    std::mutex output_mutex_;
};

}  // namespace parking_timer

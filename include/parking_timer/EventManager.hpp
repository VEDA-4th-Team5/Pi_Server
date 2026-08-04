#pragma once

#include "parking_timer/Types.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace parking_timer {

/** @brief 타이머 상태 전이를 thread-safe 한 줄 JSON으로 발행한다. */
class EventManager {
public:
    using Publisher = std::function<void(
        std::string_view, std::int64_t, std::string_view, std::string_view,
        std::string_view, std::string_view)>;

    /** @brief JSON 로그 외에 MQTT 등 외부 전달 callback을 설정한다. */
    void setPublisher(Publisher publisher);

    /** @brief slot_id, 차량, 시각과 상세 근거를 JSON 이벤트로 출력한다. */
    void publish(std::string_view event_type,
                 std::string_view slot_id,
                 std::string_view car_number,
                 std::string_view occurred_at,
                 std::string_view detail = {},
                 std::int64_t session_id = -1);

private:
    std::mutex output_mutex_;
    Publisher publisher_;
};

}  // namespace parking_timer

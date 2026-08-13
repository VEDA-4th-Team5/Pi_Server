#include "parking_timer/EventManager.hpp"

#include "util/Logger.hpp"

#include <iostream>
#include <sstream>

namespace parking_timer {
namespace {

/**
 * @brief 문자열을 JSON 문자열 값에 안전하게 포함할 수 있도록 이스케이프한다.
 *
 * @param[in] input 이스케이프할 UTF-8 문자열.
 * @return 따옴표, 역슬래시, 제어문자가 JSON 규칙에 맞게 변환된 문자열.
 * @note 한글을 포함한 일반 UTF-8 바이트는 그대로 보존한다.
 */
std::string jsonEscape(const std::string_view input) {
    std::ostringstream output;
    for (const unsigned char value : input) {
        switch (value) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            output << static_cast<char>(value);
            break;
        }
    }
    return output.str();
}

}  // namespace

void EventManager::setPublisher(Publisher publisher) {
    std::lock_guard lock(output_mutex_);
    publisher_ = std::move(publisher);
}

/**
 * @brief 관제 시스템으로 보낼 이벤트를 한 줄 JSON 형태로 표준 출력에 발행한다.
 *
 * @param[in] event_type `ARRIVAL`, `DEPARTURE`, `VIOLATION_TRIGGERED` 등의 이벤트 종류.
 * @param[in] slot_id 이벤트가 발생한 주차면 ID.
 * @param[in] car_number 인식된 차량번호. 차량이 없으면 빈 문자열일 수 있다.
 * @param[in] occurred_at 이벤트 발생 UTC 시각.
 * @param[in] detail 선택적인 상세 메시지 또는 증거 이미지 경로.
 * @note 여러 스레드에서 호출되어도 JSON 한 줄이 서로 섞이지 않도록 출력 mutex를 잡는다.
 */
bool EventManager::publish(const std::string_view event_type,
                           const std::string_view slot_id,
                           const std::string_view car_number,
                           const std::string_view occurred_at,
                           const std::string_view detail,
                           const std::int64_t session_id) {
    // 타이머 worker와 CLI 스레드가 동시에 이벤트를 출력할 수 있으므로 한 줄 전체를 잠근다.
    Publisher publisher;
    {
        std::lock_guard lock(output_mutex_);
        // 표준출력 JSON은 사람이 보는 용도다. LOG_EVENT_JSON=false 로 꺼도
        // 아래 publisher(MQTT 발행)는 그대로 동작해야 하므로 출력만 건너뛴다.
        // EXIT_IGNORED는 빈 슬롯이 VACANT를 재전송할 때마다(초당) 찍혀서
        // 콘솔을 도배하므로 LOG_EXIT_IGNORED로 따로 끌 수 있게 한다
        // (MQTT 발행은 이 스위치와 무관하게 그대로 나간다).
        const bool show_event = event_type != "EXIT_IGNORED" ||
                                util::logEnabled("EXIT_IGNORED");
        if (show_event && util::logEnabled("EVENT_JSON")) {
            std::cout << "{\"event_type\":\"" << jsonEscape(event_type)
                      << "\",\"slot_id\":\"" << jsonEscape(slot_id)
                      << "\",\"car_number\":\""
                      << jsonEscape(car_number) << "\",\"occurred_at\":\""
                      << jsonEscape(occurred_at) << '"';
            if (!detail.empty()) {
                std::cout << ",\"detail\":\"" << jsonEscape(detail) << '"';
            }
            std::cout << "}" << std::endl;
        }
        publisher = publisher_;
    }
    if (publisher) {
        return publisher(event_type, session_id, slot_id, car_number,
                         occurred_at, detail);
    }
    return false;
}

}  // namespace parking_timer

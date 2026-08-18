#pragma once

#include "notification/TelegramMessage.hpp"

#include <string>

namespace notification {

class TelegramMessageFormatter {
public:
    /** 운영자에게 전달할 가치가 있는 경보 이벤트인지 판정한다. */
    bool shouldNotify(const TelegramMessage& message) const;

    /** 동일 경보 재발행을 억제할 안정적인 event_id를 반환한다. */
    std::string eventId(const TelegramMessage& message) const;

    /** MQTT 내부 필드를 숨기고 사람이 읽을 수 있는 경보 문장으로 변환한다. */
    std::string format(const TelegramMessage& message) const;

private:
    static std::string truncate(const std::string& text);
};

}  // namespace notification

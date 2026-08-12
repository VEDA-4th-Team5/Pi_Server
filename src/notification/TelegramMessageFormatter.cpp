#include <string_view>
#include "notification/TelegramMessageFormatter.hpp"

namespace notification {

std::string TelegramMessageFormatter::format(
    const TelegramMessage& message
) const {
    const std::string formatted =
        "[Pi Server MQTT]\n"
        "Topic: " + message.topic + "\n" +
        "QoS: " + std::to_string(message.qos) + "\n" +
        "Retain: " + (message.retain ? "true" : "false") + "\n" +
        "Payload:\n" +
        truncate(message.payload);
    return formatted;
}

std::string TelegramMessageFormatter::truncate(const std::string& text) {
    if (text.size() <= kTelegramMaxTextLength) {
        return text;
    }

    constexpr std::string_view suffix = "\n[truncated]";
    const std::size_t max_body =
        static_cast<std::size_t>(kTelegramMaxTextLength);
    if (max_body <= suffix.size()) {
        return std::string(suffix);
    }
    const std::size_t cutoff = max_body - suffix.size();
    return text.substr(0, cutoff) + std::string(suffix);
}

}  // namespace notification



#pragma once

#include <cstdint>
#include <string>

namespace notification {

struct TelegramMessage {
    std::string topic;
    std::string payload;
    int qos{1};
    bool retain{false};
};

constexpr std::uint32_t kTelegramMaxTextLength = 4096;

}  // namespace notification

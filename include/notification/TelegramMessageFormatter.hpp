#pragma once

#include "notification/TelegramMessage.hpp"

#include <string>

namespace notification {

class TelegramMessageFormatter {
public:
    std::string format(const TelegramMessage& message) const;

private:
    static std::string truncate(const std::string& text);
};

}  // namespace notification

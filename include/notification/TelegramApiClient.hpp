#pragma once

#include <curl/curl.h>

#include <string>

namespace notification {

class TelegramApiClient {
public:
    struct Config {
        bool enabled{false};
        std::string bot_token;
        std::string channel;
        long connect_timeout_ms{3000};
        long request_timeout_ms{10000};
    };

    explicit TelegramApiClient(const Config& config);
    bool sendMessage(const std::string& text);

    bool isEnabled() const { return config_.enabled; }
    const std::string& channel() const { return config_.channel; }

private:
    const Config config_;
    std::string base_url_;
};

}  // namespace notification


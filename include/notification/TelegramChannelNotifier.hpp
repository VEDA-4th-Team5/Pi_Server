#pragma once

#include "notification/TelegramApiClient.hpp"
#include "notification/TelegramMessage.hpp"
#include "notification/TelegramMessageFormatter.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace notification {

class TelegramChannelNotifier {
public:
    struct Config {
        bool enabled{false};
        std::size_t queue_capacity{256};
        int retry_count{2};
        int retry_delay_ms{500};
    };

    TelegramChannelNotifier(const Config& config, const TelegramApiClient& api_client);

    ~TelegramChannelNotifier();
    TelegramChannelNotifier(const TelegramChannelNotifier&) = delete;
    TelegramChannelNotifier& operator=(const TelegramChannelNotifier&) = delete;

    bool start();
    void stop();
    bool enqueue(TelegramMessage message);
    bool running() const;

private:
    void run();

    bool sendWithRetry(const TelegramMessage& message, const std::string& text);

    const Config config_;
    const TelegramApiClient& api_client_;
    const TelegramMessageFormatter formatter_;
    std::deque<TelegramMessage> queue_;
    bool stopped_{true};
    bool stop_requested_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cond_;
    std::thread worker_;
};

}  // namespace notification

#include "notification/TelegramChannelNotifier.hpp"

#include "util/Logger.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace notification {

TelegramChannelNotifier::TelegramChannelNotifier(const Config& config,
                                               const TelegramApiClient& api_client)
    : config_(config), api_client_(api_client), formatter_() {
}

TelegramChannelNotifier::~TelegramChannelNotifier() {
    stop();
}

bool TelegramChannelNotifier::start() {
    std::lock_guard lock(queue_mutex_);
    if (!config_.enabled) {
        return false;
    }
    if (!stopped_) {
        return true;
    }
    stop_requested_ = false;
    stopped_ = false;
    worker_ = std::thread(&TelegramChannelNotifier::run, this);
    return true;
}

void TelegramChannelNotifier::stop() {
    {
        std::lock_guard lock(queue_mutex_);
        if (stopped_) return;
        stop_requested_ = true;
    }
    queue_cond_.notify_all();
    if (worker_.joinable()) worker_.join();
    stopped_ = true;
}

bool TelegramChannelNotifier::running() const {
    return !stopped_;
}

bool TelegramChannelNotifier::enqueue(TelegramMessage message) {
    std::lock_guard lock(queue_mutex_);
    if (!config_.enabled) {
        return false;
    }
    // 일반 입출차, retained 상태 및 디버그 MQTT는 Telegram으로 보내지 않는다.
    if (!formatter_.shouldNotify(message)) return true;
    const std::string event_id = formatter_.eventId(message);
    if (!event_id.empty()) {
        if (recent_event_id_set_.contains(event_id)) return true;
        recent_event_ids_.push_back(event_id);
        recent_event_id_set_.insert(event_id);
        const std::size_t recent_capacity = config_.queue_capacity * 4;
        while (recent_event_ids_.size() > recent_capacity) {
            recent_event_id_set_.erase(recent_event_ids_.front());
            recent_event_ids_.pop_front();
        }
    }
    if (queue_.size() >= config_.queue_capacity && !queue_.empty()) {
        util::logWarn("Telegram queue full; dropping oldest message");
        queue_.pop_front();
    }
    queue_.push_back(std::move(message));
    queue_cond_.notify_one();
    return true;
}

bool TelegramChannelNotifier::sendWithRetry(const TelegramMessage&,
                                          const std::string& text) {
    for (int attempt = 0; attempt <= config_.retry_count; ++attempt) {
        if (api_client_.sendMessage(text)) return true;
        if (attempt >= config_.retry_count) return false;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.retry_delay_ms *
                                      static_cast<int>(attempt + 1)));
    }
    return false;
}

void TelegramChannelNotifier::run() {
    while (true) {
        std::unique_lock lock(queue_mutex_);
        queue_cond_.wait(lock, [this] {
            return stop_requested_ || !queue_.empty();
        });
        if (stop_requested_ && queue_.empty()) {
            return;
        }
        TelegramMessage message = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();

        const std::string formatted = formatter_.format(message);
        if (formatted.empty()) continue;
        if (!sendWithRetry(message, formatted)) {
            util::logWarn("Telegram queue send failed for topic=" + message.topic);
        }
    }
}

}  // namespace notification

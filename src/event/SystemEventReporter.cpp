#include "event/SystemEventReporter.hpp"

#include "util/Logger.hpp"
#include "util/StringUtil.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <utility>

namespace event {

std::string toString(const SystemEventSource source) {
    switch (source) {
    case SystemEventSource::HallSensor: return "HALL_SENSOR";
    case SystemEventSource::Uart: return "UART";
    case SystemEventSource::LoRa: return "LORA";
    case SystemEventSource::Mqtt: return "MQTT";
    case SystemEventSource::Rtsp: return "RTSP";
    case SystemEventSource::Database: return "DATABASE";
    }
    return "UNKNOWN";
}

std::string toString(const SystemEventCode code) {
    switch (code) {
    case SystemEventCode::SensorMessageInvalid: return "SENSOR_MESSAGE_INVALID";
    case SystemEventCode::SensorNotMapped: return "SENSOR_NOT_MAPPED";
    case SystemEventCode::SensorSequenceRejected: return "SENSOR_SEQUENCE_REJECTED";
    case SystemEventCode::SensorHandlerFailed: return "SENSOR_HANDLER_FAILED";
    case SystemEventCode::UartOpenFailed: return "UART_OPEN_FAILED";
    case SystemEventCode::UartReadFailed: return "UART_READ_FAILED";
    case SystemEventCode::UartWriteFailed: return "UART_WRITE_FAILED";
    case SystemEventCode::UartBufferOverflow: return "UART_BUFFER_OVERFLOW";
    case SystemEventCode::UartRecovered: return "UART_RECOVERED";
    case SystemEventCode::LoRaFrameRejected: return "LORA_FRAME_REJECTED";
    case SystemEventCode::LoRaRecovered: return "LORA_RECOVERED";
    }
    return "UNKNOWN_SYSTEM_EVENT";
}

std::string toString(const SystemEventSeverity severity) {
    switch (severity) {
    case SystemEventSeverity::Warning: return "WARNING";
    case SystemEventSeverity::Error: return "ERROR";
    case SystemEventSeverity::Critical: return "CRITICAL";
    }
    return "ERROR";
}

std::string serializeSystemEvent(const SystemEvent& event) {
    std::ostringstream output;
    output << "{\"source\":\"" << util::jsonEscape(toString(event.source))
           << "\",\"severity\":\"" << util::jsonEscape(toString(event.severity))
           << "\",\"transport\":\"" << util::jsonEscape(event.transport)
           << "\",\"device\":\"" << util::jsonEscape(event.device)
           << "\",\"message\":\"" << util::jsonEscape(event.message)
           << "\",\"retry_count\":" << event.retry_count
           << ",\"suppressed_count\":" << event.suppressed_count
           << ",\"dropped_count\":" << event.dropped_count
           << ",\"recovered\":" << (event.recovered ? "true" : "false")
           << '}';
    return output.str();
}

SystemEventReporter::SystemEventReporter(Sink sink)
    : SystemEventReporter(std::move(sink), Config{}) {}

SystemEventReporter::SystemEventReporter(Sink sink, Config config)
    : sink_(std::move(sink)), config_(config) {
    config_.queue_capacity = std::max<std::size_t>(1, config_.queue_capacity);
    config_.duplicate_window = std::max(
        std::chrono::milliseconds::zero(), config_.duplicate_window);
    config_.sink_retry_delay = std::max(
        std::chrono::milliseconds{1}, config_.sink_retry_delay);
}

SystemEventReporter::~SystemEventReporter() {
    stop();
}

bool SystemEventReporter::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    if (!sink_) return false;
    stopping_ = false;
    try {
        worker_ = std::thread(&SystemEventReporter::run, this);
        running_ = true;
        return true;
    } catch (const std::exception& error) {
        util::logError("System event reporter start failed: " +
                       std::string(error.what()));
    } catch (...) {
        util::logError("System event reporter start failed: unknown error");
    }
    return false;
}

void SystemEventReporter::stop() noexcept {
    try {
        {
            std::lock_guard lock(mutex_);
            if (!running_ && !worker_.joinable()) return;
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard lock(mutex_);
        running_ = false;
    } catch (...) {
        util::logError("System event reporter stop failed");
    }
}

void SystemEventReporter::report(SystemEvent event) noexcept {
    try {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        if (stopping_) return;

        if (event.recovered) {
            clearRecoveredStateLocked(event);
            enqueueLocked(std::move(event));
            condition_.notify_one();
            return;
        }

        const std::string key = deduplicationKey(event);
        auto [position, inserted] = duplicate_states_.try_emplace(
            key, DuplicateState{now, 0});
        if (!inserted &&
            now - position->second.last_enqueued < config_.duplicate_window) {
            ++position->second.suppressed;
            return;
        }
        if (!inserted) {
            event.suppressed_count += position->second.suppressed;
            position->second.last_enqueued = now;
            position->second.suppressed = 0;
        }
        enqueueLocked(std::move(event));
        condition_.notify_one();
    } catch (...) {
        // 오류 보고 경로가 원래 센서/통신 스레드에 예외를 돌려주지 않는 최종 경계다.
        util::logError("System event reporter dropped an event after internal error");
    }
}

bool SystemEventReporter::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_ && !stopping_;
}

std::size_t SystemEventReporter::queuedCount() const noexcept {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::string SystemEventReporter::deduplicationKey(const SystemEvent& event) {
    return toString(event.source) + '|' + toString(event.code) + '|' +
           event.transport + '|' + event.device + '|' + event.slot_id;
}

void SystemEventReporter::clearRecoveredStateLocked(const SystemEvent& event) {
    const std::string source = toString(event.source) + '|';
    for (auto iterator = duplicate_states_.begin();
         iterator != duplicate_states_.end();) {
        const bool same_source = iterator->first.starts_with(source);
        const bool same_device = event.device.empty() ||
                                 iterator->first.find('|' + event.device + '|') !=
                                     std::string::npos;
        if (same_source && same_device)
            iterator = duplicate_states_.erase(iterator);
        else
            ++iterator;
    }
}

void SystemEventReporter::enqueueLocked(SystemEvent event) {
    if (queue_.size() >= config_.queue_capacity) {
        const auto removable = std::find_if(
            queue_.begin(), queue_.end(), [](const SystemEvent& queued) {
                return queued.severity != SystemEventSeverity::Critical;
            });
        if (removable == queue_.end()) {
            ++dropped_events_;
            return;
        }
        queue_.erase(removable);
        ++dropped_events_;
    }
    event.dropped_count += std::exchange(dropped_events_, 0);
    queue_.push_back(std::move(event));
}

void SystemEventReporter::run() noexcept {
    for (;;) {
        SystemEvent event;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            event = std::move(queue_.front());
            queue_.pop_front();
        }

        if (persist(event)) continue;

        std::unique_lock lock(mutex_);
        if (stopping_) continue;
        if (queue_.size() >= config_.queue_capacity) {
            queue_.pop_back();
            ++dropped_events_;
        }
        queue_.push_front(std::move(event));
        condition_.wait_for(lock, config_.sink_retry_delay,
                            [this] { return stopping_; });
    }

    std::lock_guard lock(mutex_);
    running_ = false;
}

bool SystemEventReporter::persist(const SystemEvent& event) noexcept {
    const std::string prefix = toString(event.code) + " source=" +
                               toString(event.source) + " message=" + event.message;
    switch (event.severity) {
    case SystemEventSeverity::Warning: util::logWarn(prefix); break;
    case SystemEventSeverity::Error:
    case SystemEventSeverity::Critical: util::logError(prefix); break;
    }

    try {
        return sink_(event, serializeSystemEvent(event));
    } catch (const std::exception& error) {
        util::logError("System event sink failed: " + std::string(error.what()));
    } catch (...) {
        util::logError("System event sink failed: unknown error");
    }
    return false;
}

}  // namespace event

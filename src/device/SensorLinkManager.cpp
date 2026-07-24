#include "device/SensorLinkManager.hpp"

#include "util/Logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <exception>
#include <span>
#include <utility>

namespace device {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

}  // namespace

SensorLinkManager::SensorLinkManager(Config config, SensorLineHandler handler,
                                     event::SystemEventReporter* reporter)
    : config_(std::move(config)), handler_(std::move(handler)), reporter_(reporter),
      uart_(config_.uart), lora_(uart_) {}

SensorLinkManager::~SensorLinkManager() { stop(); }

SensorLinkMode SensorLinkManager::parseMode(const std::string& value) {
    const std::string normalized = lower(value);
    if (normalized == "uart" || normalized == "uart-line")
        return SensorLinkMode::UartLine;
    if (normalized == "lora" || normalized == "lora-frame")
        return SensorLinkMode::LoRaFrame;
    return SensorLinkMode::Disabled;
}

std::string SensorLinkManager::modeName(const SensorLinkMode mode) {
    switch (mode) {
    case SensorLinkMode::UartLine: return "uart-line";
    case SensorLinkMode::LoRaFrame: return "lora-frame";
    case SensorLinkMode::Disabled: return "off";
    }
    return "off";
}

bool SensorLinkManager::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (running_.load()) return true;
    if (config_.mode == SensorLinkMode::Disabled || !handler_ ||
        config_.uart.device_path.empty())
        return false;
    running_.store(true);
    worker_ = std::thread(&SensorLinkManager::run, this);
    return true;
}

void SensorLinkManager::stop() {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!running_.exchange(false)) return;
    }
    wait_condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    uart_.disconnect();
}

bool SensorLinkManager::connected() const { return uart_.connected(); }

bool SensorLinkManager::sendAlertCommand(const std::string& command,
                                         const std::uint32_t sequence,
                                         std::string* error) {
    if (command.empty()) {
        if (error != nullptr) *error = "alert command is empty";
        return false;
    }
    if (config_.mode == SensorLinkMode::LoRaFrame) {
        LoRaFrame frame;
        frame.type = LoRaMessageType::AlertCommand;
        frame.sequence = sequence;
        frame.payload.assign(command.begin(), command.end());
        const bool sent = lora_.send(frame, error);
        if (!sent) {
            report(event::SystemEventCode::UartWriteFailed,
                   event::SystemEventSeverity::Error,
                   error == nullptr ? "LoRa alert command write failed" : *error);
        }
        return sent;
    }
    std::string line = command;
    if (line.back() != '\n') line.push_back('\n');
    const bool sent = uart_.writeAll(std::span<const std::uint8_t>(
                              reinterpret_cast<const std::uint8_t*>(line.data()),
                              line.size()),
                          error);
    if (!sent) {
        report(event::SystemEventCode::UartWriteFailed,
               event::SystemEventSeverity::Error,
               error == nullptr ? "UART alert command write failed" : *error);
    }
    return sent;
}

void SensorLinkManager::run() {
    std::array<std::uint8_t, 512> buffer{};
    while (running_.load()) {
        if (!uart_.connected()) {
            std::string error;
            if (!uart_.connect(&error)) {
                util::logWarn("Sensor UART connect failed: " + error);
                link_failed_ = true;
                ++reconnect_attempts_;
                report(event::SystemEventCode::UartOpenFailed,
                       event::SystemEventSeverity::Error, error,
                       reconnect_attempts_);
                if (!waitReconnect()) break;
                continue;
            }
            util::logInfo("Sensor link connected: mode=" + modeName(config_.mode) +
                          " device=" + config_.uart.device_path +
                          " baud=" + std::to_string(config_.uart.baud_rate));
            if (link_failed_) {
                report(event::SystemEventCode::UartRecovered,
                       event::SystemEventSeverity::Warning,
                       "sensor UART link recovered", reconnect_attempts_, true);
                link_failed_ = false;
                reconnect_attempts_ = 0;
            }
        }

        std::string error;
        const int count = uart_.readSome(buffer, &error);
        if (count < 0) {
            util::logWarn("Sensor UART read failed: " + error);
            link_failed_ = true;
            ++reconnect_attempts_;
            report(event::SystemEventCode::UartReadFailed,
                   event::SystemEventSeverity::Error, error,
                   reconnect_attempts_);
            uart_.disconnect();
            if (!waitReconnect()) break;
            continue;
        }
        if (count == 0) continue;
        if (config_.mode == SensorLinkMode::LoRaFrame)
            consumeLoRaFrames(buffer.data(), static_cast<std::size_t>(count));
        else
            consumeUartLines(buffer.data(), static_cast<std::size_t>(count));
    }
    uart_.disconnect();
}

void SensorLinkManager::consumeUartLines(const std::uint8_t* data,
                                         const std::size_t size) {
    line_buffer_.append(reinterpret_cast<const char*>(data), size);
    if (line_buffer_.size() > 4096) {
        util::logWarn("Sensor UART line buffer overflow; buffered data discarded");
        report(event::SystemEventCode::UartBufferOverflow,
               event::SystemEventSeverity::Warning,
               "sensor UART line buffer exceeded 4096 bytes");
        line_buffer_.clear();
        return;
    }
    std::size_t newline;
    while ((newline = line_buffer_.find('\n')) != std::string::npos) {
        std::string line = line_buffer_.substr(0, newline);
        line_buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            handler_(line, "uart");
        } catch (const std::exception& exception) {
            util::logError("Sensor UART handler failed: " +
                           std::string(exception.what()));
            report(event::SystemEventCode::SensorHandlerFailed,
                   event::SystemEventSeverity::Error, exception.what());
        } catch (...) {
            util::logError("Sensor UART handler failed: unknown error");
            report(event::SystemEventCode::SensorHandlerFailed,
                   event::SystemEventSeverity::Error, "unknown handler error");
        }
    }
}

void SensorLinkManager::consumeLoRaFrames(const std::uint8_t* data,
                                          const std::size_t size) {
    const std::uint64_t rejected_before = lora_.rejectedFrames();
    const auto frames = lora_.consume(std::span<const std::uint8_t>(data, size));
    const std::uint64_t rejected_after = lora_.rejectedFrames();
    if (rejected_after > rejected_before) {
        report(event::SystemEventCode::LoRaFrameRejected,
               event::SystemEventSeverity::Warning,
               "LoRa frame failed version, length, or CRC validation; count=" +
                   std::to_string(rejected_after - rejected_before));
    }
    for (const auto& frame : frames) {
        if (frame.type != LoRaMessageType::SensorEvent) continue;
        const std::string line(frame.payload.begin(), frame.payload.end());
        if (line.empty()) continue;
        try {
            handler_(line, "lora");
        } catch (const std::exception& exception) {
            util::logError("LoRa sensor handler failed: " +
                           std::string(exception.what()));
            report(event::SystemEventCode::SensorHandlerFailed,
                   event::SystemEventSeverity::Error, exception.what());
        } catch (...) {
            util::logError("LoRa sensor handler failed: unknown error");
            report(event::SystemEventCode::SensorHandlerFailed,
                   event::SystemEventSeverity::Error, "unknown handler error");
        }
    }
}

bool SensorLinkManager::waitReconnect() {
    std::unique_lock lock(wait_mutex_);
    return !wait_condition_.wait_for(
        lock, std::chrono::milliseconds(std::max(1, config_.reconnect_delay_ms)),
        [this] { return !running_.load(); });
}

void SensorLinkManager::report(const event::SystemEventCode code,
                               const event::SystemEventSeverity severity,
                               const std::string& message,
                               const std::uint32_t retry_count,
                               const bool recovered) noexcept {
    if (reporter_ == nullptr) return;
    reporter_->report({
        .source = code == event::SystemEventCode::LoRaFrameRejected
                      ? event::SystemEventSource::LoRa
                      : event::SystemEventSource::Uart,
        .code = code,
        .severity = severity,
        .slot_id = {},
        .transport = modeName(config_.mode),
        .device = config_.uart.device_path,
        .message = message,
        .retry_count = retry_count,
        .recovered = recovered,
    });
}

}  // namespace device

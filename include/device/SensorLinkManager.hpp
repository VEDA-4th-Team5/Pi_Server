#pragma once

#include "device/LoRaDriver.hpp"
#include "device/UartDriver.hpp"
#include "event/SystemEventReporter.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace device {

enum class SensorLinkMode {
    Disabled,
    UartLine,
    LoRaFrame
};

/** @brief UART/LoRa 입력을 기존 SENSOR 한 줄 처리기로 전달하는 장치 서비스다. */
class SensorLinkManager {
public:
    struct Config {
        SensorLinkMode mode{SensorLinkMode::Disabled};
        UartDriver::Config uart;
        int reconnect_delay_ms{1000};
    };

    using SensorLineHandler =
        std::function<void(const std::string&, const std::string& transport)>;

    SensorLinkManager(Config config, SensorLineHandler handler,
                      event::SystemEventReporter* reporter = nullptr);
    ~SensorLinkManager();

    SensorLinkManager(const SensorLinkManager&) = delete;
    SensorLinkManager& operator=(const SensorLinkManager&) = delete;

    bool start();
    void stop();
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] bool connected() const;

    /** @brief STM32/LoRa 반대 방향으로 경고 명령을 전송한다. */
    bool sendAlertCommand(const std::string& command,
                          std::uint32_t sequence,
                          std::string* error = nullptr);

    static SensorLinkMode parseMode(const std::string& value);
    static std::string modeName(SensorLinkMode mode);

private:
    void run();
    void consumeUartLines(const std::uint8_t* data, std::size_t size);
    void consumeLoRaFrames(const std::uint8_t* data, std::size_t size);
    bool waitReconnect();
    void report(event::SystemEventCode code,
                event::SystemEventSeverity severity,
                const std::string& message,
                std::uint32_t retry_count = 0,
                bool recovered = false) noexcept;

    Config config_;
    SensorLineHandler handler_;
    event::SystemEventReporter* reporter_{};
    UartDriver uart_;
    LoRaDriver lora_;
    std::atomic_bool running_{false};
    std::thread worker_;
    std::string line_buffer_;
    mutable std::mutex lifecycle_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_condition_;
    std::uint32_t reconnect_attempts_{};
    bool link_failed_{};
};

}  // namespace device

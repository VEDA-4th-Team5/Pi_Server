// STM32 UART 통신만 단독으로 확인하기 위한 경량 실행 파일.
// 카메라/MQTT/DB 없이 운영 서버와 같은 device::SensorLinkManager로
// SENSOR:/FIRE: 프레임을 받아 파싱하고 콘솔에 찍어준다.
#include "device/SensorLinkManager.hpp"
#include "sensor/SensorProtocolParser.hpp"
#include "util/Logger.hpp"

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

std::string getEnvOr(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr) ? std::string(value) : std::move(fallback);
}

int getEnvIntOr(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    device::SensorLinkManager::Config link_config;
    link_config.mode = device::SensorLinkMode::UartLine;
    link_config.uart.device_path = getEnvOr(
        "SENSOR_UART_DEVICE", getEnvOr("FIRE_UART_DEVICE", "/dev/ttyAMA0"));
    link_config.uart.baud_rate = getEnvIntOr(
        "SENSOR_UART_BAUD", getEnvIntOr("FIRE_UART_BAUD", 115200));
    link_config.uart.read_timeout_ms =
        getEnvIntOr("SENSOR_UART_READ_TIMEOUT_MS", 250);
    link_config.reconnect_delay_ms = getEnvIntOr(
        "SENSOR_UART_RECONNECT_MS",
        getEnvIntOr("FIRE_UART_REOPEN_DELAY_MS", 2000));

    util::logInfo("uart-fire-listener started");
    util::logInfo("device=" + link_config.uart.device_path +
                   " baud=" + std::to_string(link_config.uart.baud_rate));

    const sensor::SensorProtocolParser parser;
    device::SensorLinkManager sensor_link(
        std::move(link_config),
        [&parser](const std::string& line, const std::string& transport) {
            std::string error;
            const auto received_at = std::chrono::system_clock::now();
            if (sensor::SensorProtocolParser::isFireLine(line)) {
                auto message = parser.parseFire(line, received_at, &error);
                if (!message) {
                    util::logWarn("fire line rejected: " + error + " | " + line);
                    return;
                }
                message->transport = transport;
                message->raw = line;
                std::ostringstream oss;
                oss << "sensor=" << message->sensorId
                    << " state=" << sensor::toString(message->state)
                    << " sequence="
                    << (message->sequence ? std::to_string(*message->sequence)
                                          : "-")
                    << " transport=" << transport << " raw=" << message->raw;
                util::logLine("FIRE", oss.str());
                return;
            }

            auto message = parser.parse(line, received_at, &error);
            if (!message) {
                util::logWarn("parking line rejected: " + error + " | " + line);
                return;
            }
            const char* state_name =
                message->state == parking::ParkingSensorState::Occupied
                    ? "OCCUPIED"
                    : "VACANT";
            std::ostringstream oss;
            oss << "sensor=" << message->sensorId << " state=" << state_name
                << " sequence="
                << (message->sequence ? std::to_string(*message->sequence)
                                      : "-")
                << " transport=" << transport;
            util::logLine("PARKING", oss.str());
        });

    if (!sensor_link.start()) {
        util::logError("failed to start sensor link");
        return 1;
    }

    util::logInfo("listening... press Ctrl+C to stop");
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    sensor_link.stop();
    util::logInfo("uart-fire-listener stopped");
    return 0;
}

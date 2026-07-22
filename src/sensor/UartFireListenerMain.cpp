// STM32 UART 통신만 단독으로 확인하기 위한 경량 실행 파일.
// 카메라/MQTT/DB 없이 SensorLinkManager 로 들어오는 SENSOR:/FIRE: 프레임을
// 파싱해서 콘솔에 그대로 찍어준다.
#include "sensor/SensorLinkManager.hpp"
#include "util/Logger.hpp"

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

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

    sensor::SensorLinkConfig link_config;
    link_config.devicePath = getEnvOr("FIRE_UART_DEVICE", "/dev/ttyAMA0");
    link_config.baudRate = getEnvIntOr("FIRE_UART_BAUD", 115200);
    link_config.reopenDelayMs =
        getEnvIntOr("FIRE_UART_REOPEN_DELAY_MS", 2000);

    util::logInfo("uart-fire-listener started");
    util::logInfo("device=" + link_config.devicePath +
                   " baud=" + std::to_string(link_config.baudRate));

    sensor::SensorLinkManager sensor_link(link_config, g_running);

    sensor_link.setFireHandler(
        [](const sensor::FireSensorMessage& message) {
            std::ostringstream oss;
            oss << "sensor=" << message.sensorId
                << " state=" << sensor::toString(message.state)
                << " sequence="
                << (message.sequence ? std::to_string(*message.sequence)
                                      : "-")
                << " raw=" << message.raw;
            util::logLine("FIRE", oss.str());
        });

    sensor_link.setParkingHandler(
        [](const sensor::SensorProtocolMessage& message) {
            const char* state_name =
                message.state == parking::ParkingSensorState::Occupied
                    ? "OCCUPIED"
                    : "VACANT";
            std::ostringstream oss;
            oss << "sensor=" << message.sensorId << " state=" << state_name
                << " sequence="
                << (message.sequence ? std::to_string(*message.sequence)
                                      : "-");
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

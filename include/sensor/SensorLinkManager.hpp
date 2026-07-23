#pragma once

#include "sensor/FireSensorMessage.hpp"
#include "sensor/SensorProtocolMessage.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace sensor {

struct SensorLinkConfig {
    // /dev/ttyAMA0 (STM32 UART) 또는 STM32 없이 테스트할 때의 FIFO 경로.
    std::string devicePath;
    int baudRate{115200};
    int reopenDelayMs{2000};
};

// STM32 에서 오는 모든 센서 신호의 Pi 측 단일 진입점이다.
// LoRa 가 추가되어도 수신 경로만 여기서 늘리고 상위 계층은 바뀌지 않는다.
// 상위 계층 헤더를 include 하지 않고 콜백으로만 알린다.
class SensorLinkManager {
public:
    using ParkingHandler = std::function<void(const SensorProtocolMessage&)>;
    using FireHandler = std::function<void(const FireSensorMessage&)>;

    SensorLinkManager(SensorLinkConfig config, std::atomic<bool>& running);
    ~SensorLinkManager();

    SensorLinkManager(const SensorLinkManager&) = delete;
    SensorLinkManager& operator=(const SensorLinkManager&) = delete;

    void setParkingHandler(ParkingHandler handler);
    void setFireHandler(FireHandler handler);

    bool start();
    void stop();

private:
    void run();
    int openDevice() const;
    void dispatchLine(const std::string& line) const;

    SensorLinkConfig config_;
    std::atomic<bool>& running_;
    std::atomic<bool> started_{false};
    SensorProtocolParser parser_;
    ParkingHandler parking_handler_;
    FireHandler fire_handler_;
    std::thread worker_;
};

}  // namespace sensor

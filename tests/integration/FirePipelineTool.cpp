// STM32 -> Pi(파싱/정책) -> 실제 MQTT 브로커 전체 경로를 카메라/DB/OCR 없이
// 단독으로 검증하는 수동 진단 도구다.
//
// main.cpp 는 절대 건드리지 않는다 — 여기서 검증된 배선(SensorLinkManager ->
// FireAlarmManager -> publish 콜백)을 그대로 main.cpp 의 동일한 코드에 옮기면
// 통합이 끝나는 구조를 위한 것이다. 이 도구 자체는 카메라 RTSP 대기 없이
// AppConfig::loadFromEnv() 의 화재/MQTT 관련 값만 그대로 재사용한다.
//
// 사용법 (예: .env.fire.local 을 source 한 뒤):
//   fire-pipeline-tool
//
// 확인:
//   mosquitto_sub -h <MQTT_HOST> -t 'parking/fire/#' -v

#include "app/AppConfig.hpp"
#include "event/FireAlarmEvent.hpp"
#include "event/FireAlarmManager.hpp"
#include "sensor/FireSensorMessage.hpp"
#include "sensor/SensorLinkManager.hpp"
#include "sensor/SensorProtocolMessage.hpp"
#include "util/Logger.hpp"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};
void signalHandler(int) {
    g_running.store(false);
}

// MqttEventBridge 는 카메라 채널/DB/스냅샷/OCR 까지 생성자에서 요구해서
// 이 단독 도구엔 너무 무겁다. FireAlarmManager::Publisher 는 함수 하나만
// 받으면 되므로, libmosquitto 를 그 최소 형태로만 감싼다.
class MinimalMqttPublisher {
public:
    explicit MinimalMqttPublisher(std::string client_id)
        : client_id_(std::move(client_id)) {}

    ~MinimalMqttPublisher() { stop(); }

    MinimalMqttPublisher(const MinimalMqttPublisher&) = delete;
    MinimalMqttPublisher& operator=(const MinimalMqttPublisher&) = delete;

    bool start(const std::string& host, int port) {
        mosquitto_lib_init();
        mosq_ = mosquitto_new(client_id_.c_str(), true, nullptr);
        if (!mosq_) {
            util::logError("mosquitto_new failed");
            return false;
        }

        const int rc = mosquitto_connect(mosq_, host.c_str(), port, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            util::logError(
                std::string("MQTT connect failed: ") + mosquitto_strerror(rc));
            return false;
        }

        if (mosquitto_loop_start(mosq_) != MOSQ_ERR_SUCCESS) {
            util::logError("MQTT loop start failed");
            return false;
        }
        loop_started_ = true;

        util::logInfo("MQTT connected: " + host + ":" + std::to_string(port));
        return true;
    }

    bool publish(const std::string& topic, const std::string& payload) {
        if (!mosq_) return false;
        const int rc = mosquitto_publish(
            mosq_, nullptr, topic.c_str(),
            static_cast<int>(payload.size()), payload.c_str(), 0, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            util::logError(
                std::string("MQTT publish failed: ") + mosquitto_strerror(rc));
            return false;
        }
        return true;
    }

    void stop() {
        if (mosq_) {
            // loop_start() 를 부른 적 없는데 loop_stop() 을 부르면 시작한 적
            // 없는 스레드를 join 하려다 libmosquitto 내부에서 멈춘다
            // (connect 실패 직후 곧장 소멸될 때 실제로 걸렸던 버그).
            if (loop_started_) {
                mosquitto_loop_stop(mosq_, true);
                loop_started_ = false;
            }
            mosquitto_destroy(mosq_);
            mosq_ = nullptr;
            mosquitto_lib_cleanup();
        }
    }

private:
    std::string client_id_;
    bool loop_started_ = false;
    mosquitto* mosq_ = nullptr;
};

}  // namespace

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const app::AppConfig config = app::AppConfig::loadFromEnv();

    if (!config.fire_alarm_enabled) {
        util::logError(
            "FIRE_ALARM_ENABLED=false — set it before running this tool "
            "(예: set -a; source .env.fire.local; set +a)");
        return 1;
    }

    MinimalMqttPublisher publisher("fire-pipeline-tool-" + config.camera_id);
    if (!publisher.start(config.mqtt_host, config.mqtt_port)) {
        return 1;
    }

    auto bindings = event::parseFireSensorBindings(config.fire_sensor_slot_map);
    if (bindings.empty()) {
        util::logWarn(
            "FIRE_SENSOR_SLOT_MAP 이 비어 있음 — slot_id 없이 발행됩니다");
    }

    event::FireAlarmManager fire_alarm_manager(
        config.camera_id,
        config.default_channel_id,
        config.fire_topic_prefix,
        std::move(bindings),
        [&publisher](const std::string& topic, const std::string& payload) {
            return publisher.publish(topic, payload);
        });

    sensor::SensorLinkConfig link_config;
    link_config.devicePath = config.fire_uart_device;
    link_config.baudRate = config.fire_uart_baud;
    link_config.reopenDelayMs = config.fire_uart_reopen_delay_ms;

    sensor::SensorLinkManager link(link_config, g_running);

    link.setFireHandler(
        [&fire_alarm_manager](const sensor::FireSensorMessage& message) {
            event::FireSignal signal;
            signal.sensorId = message.sensorId;
            signal.detected = message.state == sensor::FireSensorState::Detected;
            signal.occurredAt = message.occurredAt;
            signal.sourceSequence = message.sequence;
            signal.sourceTransport = message.transport;
            signal.rawPayload = message.raw;
            fire_alarm_manager.onFireSignal(signal);
        });

    // 이 도구는 화재 경로만 검증한다. 주차 센서(SENSOR:) 프레임은 아직
    // 카메라 IVA 경로가 담당하므로 로그만 남기고 발행하지 않는다(main.cpp 와 동일).
    link.setParkingHandler([](const sensor::SensorProtocolMessage& message) {
        util::logLine("SENSOR_UART",
                      "parking sensor not wired in this tool: sensor=" +
                          message.sensorId);
    });

    if (!link.start()) {
        util::logError("failed to start sensor link");
        return 1;
    }

    util::logInfo(
        "fire-pipeline-tool: device=" + config.fire_uart_device +
        " mqtt=" + config.mqtt_host + ":" + std::to_string(config.mqtt_port) +
        " topic_prefix=" + config.fire_topic_prefix);
    util::logInfo("press Ctrl+C to stop");

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    link.stop();
    publisher.stop();
    util::logInfo("fire-pipeline-tool stopped");
    return 0;
}

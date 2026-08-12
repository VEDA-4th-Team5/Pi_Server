// MqttEventBridge는 Qt가 실시간으로 의존하는 푸시 경로인데도 ctest 등록이
// 0개였다. 생성자가 카메라/DB/스냅샷/OCR까지 실제 레퍼런스로 요구해서 무겁고
// (tests/integration/FirePipelineTool.cpp의 주석 참고 — 원 개발자도 그 도구엔
// "너무 무겁다"고 판단해 최소 publisher로 우회했다), onMessage()도 private라
// 실제 mosquitto 브로커를 통한 왕복 없이는 트리거할 수 없다.
//
// 이 테스트는 로컬에서 이미 도는 mosquitto 브로커(127.0.0.1:1883)에 실제로
// 연결해서, Fire ACK 명령 경로(토픽 파싱 -> JSON 검증 -> channel_id 추출 ->
// 콜백)를 처음부터 끝까지 검증한다. IVA 카메라 이벤트 경로(OCR/스냅샷까지
// 이어지는 훨씬 큰 그래프)는 이번 범위에 넣지 않았다 — 후속 작업 필요.

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef PARKING_TIMER_TEST_SQL_DIR
#error PARKING_TIMER_TEST_SQL_DIR must be defined
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// FirePipelineTool.cpp의 MinimalMqttPublisher와 같은 목적 — 테스트 쪽에서
// 별도 클라이언트로 브로커에 실제 publish해서 MqttEventBridge::onMessage를
// 진짜 네트워크 왕복으로 트리거한다.
class TestPublisher {
public:
    bool start(const std::string& host, int port) {
        mosquitto_lib_init();
        mosq_ = mosquitto_new("mqtt-event-bridge-test-publisher", true, nullptr);
        if (!mosq_) return false;
        if (mosquitto_connect(mosq_, host.c_str(), port, 60) != MOSQ_ERR_SUCCESS)
            return false;
        return mosquitto_loop_start(mosq_) == MOSQ_ERR_SUCCESS;
    }

    bool publish(const std::string& topic, const std::string& payload) {
        if (!mosq_) return false;
        return mosquitto_publish(mosq_, nullptr, topic.c_str(),
                                 static_cast<int>(payload.size()),
                                 payload.c_str(), 0, false) == MOSQ_ERR_SUCCESS;
    }

    ~TestPublisher() {
        if (mosq_) {
            mosquitto_loop_stop(mosq_, true);
            mosquitto_destroy(mosq_);
        }
        mosquitto_lib_cleanup();
    }

private:
    mosquitto* mosq_{nullptr};
};

bool waitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("mqtt_event_bridge_test_" + std::to_string(unique));

    try {
        database::EventDatabase database(root / "parking.sqlite3");
        const fs::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
        database.migrateRuntimeSchema();

        std::atomic<bool> running{true};
        snapshot::SnapshotStorage storage(
            (root / "snapshots").string(), 50, running);

        std::vector<std::shared_ptr<camera::CameraChannel>> channels;
        parking::ParkingTriggerCoordinator trigger_coordinator(2000, 5000);

        // 실제 네트워크를 절대 타지 않는 가짜 transport. Fire ACK 경로는
        // OCR을 쓰지 않지만 MqttEventBridge 생성자가 OcrWorker&를 요구한다.
        ocr::GeminiOcrClient::HttpTransport fake_transport =
            [](const ocr::GeminiHttpRequest&) {
                ocr::GeminiHttpResponse response;
                response.completed = false;
                response.error = "unused by MqttEventBridgeTest";
                return response;
            };
        ocr::GeminiOcrClient gemini_client(
            "unused-key", "unused-model", 1, 1, "", fake_transport);
        ocr::OcrWorker ocr_worker(std::move(gemini_client), database, false);

        app::AppConfig config{};
        config.camera_id = "test-cam";
        config.mqtt_host = "127.0.0.1";
        config.mqtt_port = 1883;
        config.mqtt_event_sub_topic = "camera/mqtt-event-bridge-test/event";
        config.default_channel_id = "ch01";
        config.hall_mqtt_input_enabled = false;
        config.fire_alarm_enabled = true;
        config.fire_command_topic_prefix = "mqtt-event-bridge-test/fire/cmd";

        std::vector<std::pair<std::string, std::string>> fire_acks;
        auto fire_ack_handler = [&fire_acks](const std::string& channel_id,
                                             const std::string& alarm_id) {
            fire_acks.emplace_back(channel_id, alarm_id);
            return true;
        };

        std::vector<parking::ParkingSlotConfig> slot_configs;

        mqtt::MqttEventBridge bridge(
            config, channels, database, storage, trigger_coordinator,
            ocr_worker, slot_configs,
            /*sensor_message_handler=*/{}, fire_ack_handler,
            /*iva_occupancy_handler=*/{}, /*telegram_notifier=*/nullptr);

        require(bridge.start(),
                "MqttEventBridge must connect to the local mosquitto broker "
                "(is mosquitto running on 127.0.0.1:1883?)");

        TestPublisher publisher;
        require(publisher.start("127.0.0.1", 1883),
                "test publisher must connect to the same local broker");

        // -- 정상 ACK: 토픽에서 뽑은 channel_id와 payload의 alarm_id가
        // 그대로 콜백까지 도달해야 한다.
        require(publisher.publish(
                    "mqtt-event-bridge-test/fire/cmd/ch01",
                    R"({"command":"ALARM_ACK","alarm_id":"alarm-1"})"),
                "publish must succeed");
        require(waitFor([&] { return !fire_acks.empty(); },
                        std::chrono::milliseconds(3000)),
                "fire_ack_handler must be invoked after a real MQTT round "
                "trip within 3s — a timeout here means MqttEventBridge is "
                "not actually receiving what it subscribed to");
        require(fire_acks.back().first == "ch01" &&
                    fire_acks.back().second == "alarm-1",
                "channel_id must come from the topic and alarm_id from the "
                "payload");

        // -- 지원하지 않는 command는 무시되어야 한다 (콜백 호출 없음).
        const auto count_before_bad = fire_acks.size();
        require(publisher.publish("mqtt-event-bridge-test/fire/cmd/ch01",
                                  R"({"command":"NOT_ACK"})"),
                "publish must succeed");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        require(fire_acks.size() == count_before_bad,
                "an unsupported command must not reach fire_ack_handler");

        // -- topic과 payload의 channel_id가 어긋나면 거부해야 한다.
        require(publisher.publish(
                    "mqtt-event-bridge-test/fire/cmd/ch01",
                    R"({"command":"ALARM_ACK","alarm_id":"x","channel_id":"ch02"})"),
                "publish must succeed");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        require(fire_acks.size() == count_before_bad,
                "a payload channel_id that disagrees with the topic's "
                "channel must be rejected, not silently applied to the "
                "wrong channel's alarm");

        // -- 잘못된 JSON은 예외 없이 무시되어야 한다.
        require(publisher.publish("mqtt-event-bridge-test/fire/cmd/ch01",
                                  "not-json"),
                "publish must succeed");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        require(fire_acks.size() == count_before_bad,
                "malformed JSON must not crash the bridge or reach the "
                "handler");

        bridge.stop();
        running.store(false);
        database.close();
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MqttEventBridgeTest failed: " << error.what()
                  << std::endl;
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}

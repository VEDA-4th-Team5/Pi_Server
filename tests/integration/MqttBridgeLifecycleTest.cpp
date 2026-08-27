#include "app/AppConfig.hpp"
#include "app/RuntimeShutdown.hpp"
#include "database/EventDatabase.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "sensor/HallParkingService.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef PARKING_TIMER_TEST_SQL_DIR
#error PARKING_TIMER_TEST_SQL_DIR must be defined
#endif

namespace {
using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ManualEvent {
public:
    void signal() {
        {
            std::lock_guard lock(mutex_);
            signaled_ = true;
        }
        condition_.notify_all();
    }

    void wait(const std::string& failure) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 2s, [this] { return signaled_; }))
            throw std::runtime_error(failure);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{false};
};

template <typename Predicate>
bool waitUntil(Predicate&& predicate,
               const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

std::int64_t wallEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class ActorCheckpointGate {
public:
    void arm(const parking::SlotActorCheckpoint checkpoint,
             const std::int64_t notBeforeEpochMs = 0) {
        std::lock_guard lock(mutex_);
        checkpoint_ = checkpoint;
        not_before_epoch_ms_ = notBeforeEpochMs;
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    void operator()(const parking::SlotActorCheckpoint checkpoint) {
        std::unique_lock lock(mutex_);
        if (!armed_ || checkpoint != checkpoint_ ||
            wallEpochMs() < not_before_epoch_ms_) {
            return;
        }
        entered_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, 3s, [this] { return released_; });
        armed_ = false;
    }

    void waitEntered(const std::string& failure) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 3s, [this] { return entered_; }))
            throw std::runtime_error(failure);
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    parking::SlotActorCheckpoint checkpoint_{
        parking::SlotActorCheckpoint::BeforeDeadlineLinearization};
    std::int64_t not_before_epoch_ms_{};
    bool armed_{};
    bool entered_{};
    bool released_{};
};

class TemporaryRoot {
public:
    TemporaryRoot()
        : path(std::filesystem::temp_directory_path() /
               ("mqtt-actor-causal-" + std::to_string(wallEpochMs()))) {
        std::filesystem::create_directories(path);
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

class FakeMqttTransport final : public mqtt::IMqttTransport {
public:
    bool emitFireAckDuringStart{false};
    int startCount{};
    int stopCount{};
    int regularPublishCount{};
    int epochPublishCount{};
    mqtt::MqttConnectionEpoch activeEpoch{1};
    mqtt::MqttConnectionEpoch lastExpectedEpoch{};

    bool start(const mqtt::MqttConnectionOptions&,
               const std::vector<mqtt::MqttSubscription>&,
               MessageCallback messageCallback,
               ObserverCallbacks observerCallbacks) override {
        ++startCount;
        messageCallback_ = std::move(messageCallback);
        observers_ = std::move(observerCallbacks);
        running_ = true;
        if (emitFireAckDuringStart) {
            emitRaw("parking/v1/fire/ack/CH1",
                    R"({"command":"ALARM_ACK","channel_id":"CH1","alarm_id":"A1"})");
        }
        return true;
    }

    mqtt::MqttPublishResult publish(const std::string&,
                                    const std::string&,
                                    int,
                                    bool) override {
        ++regularPublishCount;
        return {running_, running_ ? 1 : -1};
    }

    mqtt::MqttPublishResult publishInEpoch(
        const std::string&,
        const std::string&,
        int,
        bool,
        const mqtt::MqttConnectionEpoch expectedEpoch) override {
        ++epochPublishCount;
        lastExpectedEpoch = expectedEpoch;
        const bool accepted = running_ && expectedEpoch == activeEpoch;
        return {accepted, accepted ? 1 : -1};
    }

    bool stopAndJoin() noexcept override {
        if (!running_) return true;
        running_ = false;
        ++stopCount;
        return true;
    }

    // A deliberately hostile late callback. The transport has stopped, but a
    // faulty adapter still invokes the callback retained from start(). The
    // production endpoint gate must reject it without touching the target.
    void emitRaw(const std::string& topic, const std::string& payload) {
        if (messageCallback_) messageCallback_(topic, payload);
    }

private:
    bool running_{false};
    MessageCallback messageCallback_;
    ObserverCallbacks observers_;
};

struct FireAckTarget {
    ~FireAckTarget() {
        if (destroyed) destroyed->store(true, std::memory_order_release);
    }

    bool acknowledge(const std::string& channelId,
                     const std::string& alarmId) {
        ++calls;
        lastChannel = channelId;
        lastAlarm = alarmId;
        if (entered) entered->signal();
        if (release) release->wait("held Fire ACK was not released");
        return true;
    }

    std::atomic<int> calls{0};
    std::string lastChannel;
    std::string lastAlarm;
    ManualEvent* entered{};
    ManualEvent* release{};
    std::atomic<bool>* destroyed{};
};

void waitForState(mqtt::MqttEventBridge& bridge,
                  const mqtt::MqttEndpointState expected,
                  const std::string& failure) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (bridge.state() != expected) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(failure);
        std::this_thread::yield();
    }
}

app::AppConfig fireConfig() {
    app::AppConfig config{};
    config.camera_id = "cam01";
    config.default_channel_id = "CH1";
    config.mqtt_host = "127.0.0.1";
    config.mqtt_port = 1883;
    config.mqtt_event_sub_topic = "camera/events/#";
    config.fire_alarm_enabled = true;
    config.fire_command_topic_prefix = "parking/v1/fire/ack";
    config.hall_mqtt_input_enabled = false;
    config.parking_occupancy_source = "HALL";
    return config;
}

mqtt::RegularEgressPermit permitForEpoch(
    const mqtt::MqttConnectionEpoch epoch) {
    return mqtt::RegularEgressPermit(
        epoch, 1, [] { return true; }, [] {});
}

void testProductionBridgeSeesPreconstructedFireTarget() {
    auto config = fireConfig();
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-lifecycle-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);

    auto target = std::make_shared<FireAckTarget>();
    std::weak_ptr<FireAckTarget> weakTarget = target;
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->emitFireAckDuringStart = true;
    std::atomic<int> clear_calls{0};
    std::string clear_channel;

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        {}, {},
        [weakTarget](const std::string& channelId,
                     const std::string& alarmId) {
            const auto leased = weakTarget.lock();
            return leased && leased->acknowledge(channelId, alarmId);
        },
        {}, std::move(transport));

    require(bridge.bindTransportFactHandler(
                [](const mqtt::MqttTransportFact&) { return true; }),
            "Fire delivery fact target bind failed");
    require(bridge.bindRegularEgressAdmission([] {
                return permitForEpoch(1);
            }),
            "Fire-priority egress gate bind failed");
    require(bridge.bindFireClearHandler(
                [&](const std::string& channel_id) {
                    clear_channel = channel_id;
                    ++clear_calls;
                    return true;
                }),
            "Fire clear target bind failed");
    require(bridge.start(), "production bridge failed to start with fake transport");
    require(target->calls.load() == 1 && target->lastChannel == "CH1" &&
                target->lastAlarm == "A1",
            "Fire ACK delivered inside transport start missed its live target");
    fake->emitRaw("parking/v1/fire/ack/CH1",
                  R"({"command":"ALARM_CLEAR","channel_id":"CH1"})");
    require(clear_calls.load() == 1 && clear_channel == "CH1",
            "live ALARM_CLEAR did not reach its target");

    require(bridge.stop(), "production bridge stop failed");
    fake->emitRaw("parking/v1/fire/ack/CH1",
                  R"({"command":"ALARM_ACK","alarm_id":"A2"})");
    require(target->calls.load() == 1,
            "late transport callback mutated Fire target after bridge stop");
    require(bridge.stop(), "idempotent production bridge stop failed");
    require(fake->stopCount == 1,
            "production bridge transport stop was not idempotent");
}

void testProductionBridgeRejectsMissingFireTarget() {
    auto config = fireConfig();
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-lifecycle-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        {}, {}, {}, {}, std::move(transport));
    require(!bridge.start(),
            "Fire-enabled bridge started without a bound Fire target");
    require(fake->startCount == 0,
            "transport opened before required Fire target validation");
}

void testHeldProductionFireAckOutlivesShutdownLease() {
    auto config = fireConfig();
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-lifecycle-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    ManualEvent entered;
    ManualEvent release;
    std::atomic<bool> destroyed{false};
    auto target = std::make_shared<FireAckTarget>();
    target->entered = &entered;
    target->release = &release;
    target->destroyed = &destroyed;
    std::weak_ptr<FireAckTarget> weakTarget = target;
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        {}, {},
        [weakTarget](const std::string& channelId,
                     const std::string& alarmId) {
            const auto leased = weakTarget.lock();
            return leased && leased->acknowledge(channelId, alarmId);
        },
        {}, std::move(transport));
    require(bridge.bindTransportFactHandler(
                [](const mqtt::MqttTransportFact&) { return true; }),
            "Fire delivery fact target bind failed");
    require(bridge.bindRegularEgressAdmission([] {
                return permitForEpoch(1);
            }),
            "Fire-priority egress gate bind failed");
    require(bridge.start(), "production bridge failed to start");

    std::thread callback([&] {
        fake->emitRaw("parking/v1/fire/ack/CH1",
                      R"({"command":"ALARM_ACK","alarm_id":"HELD"})");
    });
    entered.wait("production Fire ACK callback did not enter");

    app::RuntimeShutdownHooks hooks;
    hooks.quiesceMqttApplication = [&] {
        if (!bridge.quiesceIngress())
            throw std::runtime_error("MQTT application drain failed");
    };
    hooks.stopMqtt = [&] {
        if (!bridge.stop())
            throw std::runtime_error("MQTT transport join failed");
    };
    hooks.destroyCallbackTargets = [&] { target.reset(); };
    app::RuntimeShutdown shutdown(std::move(hooks));
    std::atomic<bool> shutdownReturned{false};
    std::atomic<bool> shutdownSucceeded{false};
    std::thread stopper([&] {
        shutdownSucceeded.store(shutdown.shutdown());
        shutdownReturned.store(true, std::memory_order_release);
    });

    waitForState(bridge, mqtt::MqttEndpointState::Quiescing,
                 "production bridge did not enter Quiescing");
    require(!shutdownReturned.load(std::memory_order_acquire) &&
                !destroyed.load(std::memory_order_acquire),
            "Fire target was destroyed while its ACK lease was active");
    fake->emitRaw("parking/v1/fire/ack/CH1",
                  R"({"command":"ALARM_ACK","alarm_id":"LATE"})");
    require(target->calls.load() == 1,
            "late Fire ACK entered after application quiescence");

    release.signal();
    callback.join();
    stopper.join();
    require(shutdownSucceeded.load() && destroyed.load() &&
                bridge.state() == mqtt::MqttEndpointState::Stopped,
            "Fire target was not destroyed after callback and transport join");
}

void testRegularPermitCannotCrossReconnectEpoch() {
    auto config = fireConfig();
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-epoch-permit-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    auto target = std::make_shared<FireAckTarget>();
    std::weak_ptr<FireAckTarget> weakTarget = target;
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->activeEpoch = 2;

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        {}, {},
        [weakTarget](const std::string& channelId,
                     const std::string& alarmId) {
            const auto leased = weakTarget.lock();
            return leased && leased->acknowledge(channelId, alarmId);
        },
        {}, std::move(transport));
    require(bridge.bindTransportFactHandler(
                [](const mqtt::MqttTransportFact&) { return true; }),
            "epoch permit fact target bind failed");
    require(bridge.bindRegularEgressAdmission([] {
                return permitForEpoch(1);
            }),
            "epoch permit admission bind failed");
    require(bridge.start(), "epoch permit bridge failed to start");

    require(!bridge.publishQtEvent("parking/v1/events", "{}", 1, false),
            "epoch-1 permit published through the epoch-2 transport");
    require(fake->epochPublishCount == 1 &&
                fake->lastExpectedEpoch == 1 &&
                fake->regularPublishCount == 0,
            "bridge bypassed exact-epoch regular publishing");
    require(bridge.stop(), "epoch permit bridge stop failed");
}

void testCameraIvaUsesSourceTimeAndAreaIdentity() {
    auto config = fireConfig();
    config.fire_alarm_enabled = false;
    config.parking_occupancy_source = "HYBRID_OR";
    config.camera_iva_event_source = "MQTT";
    config.hall_mqtt_input_enabled = true;
    config.hall_mqtt_topic = "parking/sensor/hall";
    config.iva_areas.push_back(
        {"EV01", "name1", "ch01", 0.0, 0.0, 1.0, 1.0});
    std::vector<parking::ParkingSlotConfig> slots{
        {"EV01", true, "EV", "hall-ev01",
         {{"cam01", "vs-0", "name1", true, 0}}}};
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-camera-time-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    std::vector<event::IvaOccupancySignal> received;
    std::vector<std::string> hall_received;
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        std::move(slots),
        [&](const std::string& line) { hall_received.push_back(line); }, {},
        [&](const event::IvaOccupancySignal& signal) {
            received.push_back(signal);
            return true;
        },
        std::move(transport));
    require(bridge.start(), "CAMERA_IVA bridge failed to start");
    const std::string topic =
        "cam01/onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0/intrusion";
    fake->emitRaw(
        topic,
        R"({"UtcTime":"2026-08-10T01:28:40.128Z","Source":{"VideoSourceToken":"vs-0","RuleName":"name1"},"Data":{"State":"true","ObjectId":41808,"Action":"Intrusion"}})");
    fake->emitRaw(
        topic,
        R"({"UtcTime":"2026-08-10T01:28:41.128Z","Source":{"VideoSourceToken":"vs-0","RuleName":"name1"},"Data":{"State":"false","ObjectId":41808,"Action":"Exit"}})");
    require(received.size() == 2 &&
                received[0].action == event::IvaOccupancyAction::Intrusion &&
                received[1].action == event::IvaOccupancyAction::Exit,
            "bridge did not preserve camera causal event order");
    require(received[0].occurredAtFromSource &&
                received[1].occurredAtFromSource &&
                received[1].occurredAt - received[0].occurredAt == 1s,
            "bridge replaced camera UtcTime with receive time");
    require(received[0].objectId == "41808" &&
                received[0].occupancyAuthority &&
                !received[0].sourceIdentity.empty() &&
                received[0].sourceIdentity != received[1].sourceIdentity,
            "bridge lost diagnostic ObjectId or area event identity");

    fake->emitRaw(
        topic,
        R"({"UtcTime":"2026-08-10T01:28:42.128Z","Source":{"VideoSourceToken":"vs-0","RuleName":"name1"},"Data":{"State":"false","Action":"Exit"}})");
    fake->emitRaw(
        topic,
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"name1","slot_id":"EV01","event_type":"IVA_AREA","action":"INTRUSION","active":true,"object_id":"41808"})");
    require(received.size() == 3 &&
                received.back().action == event::IvaOccupancyAction::Exit &&
                received.back().objectId.empty() &&
                received.back().occurredAtFromSource,
            "objectless source event did not reach area occupancy");
    fake->emitRaw(config.hall_mqtt_topic, "SENSOR:HALL01:OCCUPIED:1");
    require(hall_received.size() == 1 &&
                hall_received.front() == "SENSOR:HALL01:OCCUPIED:1",
            "HYBRID_OR bridge did not dispatch Hall MQTT input");
    require(bridge.stop(), "CAMERA_IVA bridge stop failed");
}

void testOnvifModeSuppressesOnlyMqttIvaTransitions() {
    auto config = fireConfig();
    config.fire_alarm_enabled = false;
    config.parking_occupancy_source = "HYBRID_OR";
    config.camera_iva_event_source = "ONVIF";
    config.hall_mqtt_input_enabled = true;
    config.hall_mqtt_topic = "parking/sensor/hall";
    config.iva_areas.push_back(
        {"EV01", "name1", "ch01", 0.0, 0.0, 1.0, 1.0});
    std::vector<parking::ParkingSlotConfig> slots{
        {"EV01", true, "EV", "hall-ev01",
         {{"cam01", "vs-0", "name1", true, 0}}}};
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-onvif-mode-test", 1, running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    std::vector<event::IvaOccupancySignal> iva_received;
    std::vector<std::string> hall_received;
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker, std::move(slots),
        [&](const std::string& line) { hall_received.push_back(line); }, {},
        [&](const event::IvaOccupancySignal& signal) {
            iva_received.push_back(signal);
            return true;
        },
        std::move(transport));
    require(bridge.start(), "ONVIF mode MQTT bridge failed to start");

    fake->emitRaw(
        "E4:30:22:F2:D1:A0/onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0/name1",
        R"({"UtcTime":"2026-08-24T03:10:00.000Z","Source":{"VideoSourceToken":"vs-0","RuleName":"name1"},"Data":{"State":"true","ObjectId":"100","Action":"Intrusion"}})");
    fake->emitRaw(
        "cam01/onvif-ej/iva/vs-0/EV01/intrusion",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"name1","slot_id":"EV01","event_type":"IVA_AREA","action":"INTRUSION","active":true})");
    require(iva_received.empty(),
            "ONVIF mode accepted a duplicate MQTT IVA transition");

    fake->emitRaw(config.hall_mqtt_topic, "SENSOR:HALL01:OCCUPIED:1");
    require(hall_received.size() == 1,
            "ONVIF mode disabled non-IVA Hall MQTT input");
    require(bridge.stop(), "ONVIF mode MQTT bridge stop failed");
}

void testCh3PublicationSharesNativeWiseAiAreaIdentity() {
    auto config = fireConfig();
    config.fire_alarm_enabled = false;
    config.parking_occupancy_source = "HYBRID_OR";
    config.iva_areas.push_back(
        {"P01", "name5", "ch03", 0.0, 0.0, 1.0, 1.0, 2, true});
    std::vector<parking::ParkingSlotConfig> slots{
        {"P01", true, "normal", "HALL05",
         {{"cam01", "vs-2", "name5", true, 0}}}};
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;
    database::EventDatabase database;
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage("unused-ch3-publication-test", 1,
                                      running);
    ocr::OcrWorker ocrWorker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    std::vector<event::IvaOccupancySignal> received;
    std::vector<event::IvaCoordinationCode> coordination;
    event::IvaOccupancyCoordinator coordinator(slots, 20s);
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();

    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocrWorker,
        slots, {}, {},
        [&](const event::IvaOccupancySignal& signal) {
            received.push_back(signal);
            coordination.push_back(coordinator.handle(signal).code);
            return true;
        },
        std::move(transport));
    require(bridge.start(), "CH3 publication bridge failed to start");

    fake->emitRaw(
        "cam01/onvif-ej/iva/vs-2/P01/intrusion",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-2","rule_name":"name5","slot_id":"P01","event_type":"IVA_AREA","action":"INTRUSION","active":true})");
    require(received.size() == 1 &&
                received[0].slotId == "P01" &&
                received[0].channelId == "ch03" &&
                received[0].videoSourceToken == "vs-2" &&
                received[0].ruleName == "name5" &&
                received[0].action ==
                    event::IvaOccupancyAction::Intrusion &&
                !received[0].authoritativeExit &&
                !received[0].occurredAtFromSource &&
                coordination[0] == event::IvaCoordinationCode::Occupied,
            "vs-2 custom intrusion was not normalized to P01 native area");

    // 고정 Publication EXIT는 카메라 실제 Action의 증거가 아니므로 무시한다.
    fake->emitRaw(
        "cam01/onvif-ej/iva/vs-2/P01/exit",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-2","rule_name":"name5","slot_id":"P01","event_type":"IVA_AREA","action":"EXIT","active":false})");
    require(received.size() == 1,
            "custom EXIT must not alter CH3 occupancy");

    // 원본 WiseAI Intrusion은 같은 vs-2/name5 영역이므로 중복 전이가 아니다.
    const std::string nativeTopic =
        "E4:30:22:F2:D1:A0/onvif-ej/OpenApp/WiseAI/IvaArea/&vs-2/name5";
    fake->emitRaw(
        nativeTopic,
        R"({"UtcTime":"2026-08-24T03:10:00.000Z","Source":{"VideoSourceToken":"vs-2","RuleName":"name5"},"Data":{"State":"true","ObjectId":"9001","Action":"Intrusion"}})");
    require(received.size() == 2 &&
                coordination[1] == event::IvaCoordinationCode::Duplicate,
            "custom and native CH3 intrusion created separate area states");

    // 출차 권한은 source time이 있는 원본 WiseAI Exit에만 있다.
    fake->emitRaw(
        nativeTopic,
        R"({"UtcTime":"2026-08-24T03:10:10.000Z","Source":{"VideoSourceToken":"vs-2","RuleName":"name5"},"Data":{"State":"true","ObjectId":"9001","Action":"Exit"}})");
    require(received.size() == 3 &&
                received.back().slotId == "P01" &&
                received.back().channelId == "ch03" &&
                received.back().videoSourceToken == "vs-2" &&
                received.back().action == event::IvaOccupancyAction::Exit &&
                received.back().authoritativeExit &&
                received.back().occurredAtFromSource &&
                coordination.back() ==
                    event::IvaCoordinationCode::ExitPending,
            "native vs-2/name5 Exit did not clear the CH3 P01 area");

    require(bridge.stop(), "CH3 publication bridge stop failed");
}

void testProductionBridgeAndActorOrderBothSidesOfDeadline() {
    TemporaryRoot temporary;
    auto config = fireConfig();
    config.fire_alarm_enabled = false;
    config.parking_occupancy_source = "CAMERA_IVA";
    config.camera_iva_exit_confirm_ms = 300;
    config.parking_occupancy_confirm_ms = 0;
    config.parking_hall_work_queue_capacity = 16;
    config.iva_areas.push_back(
        {"EV01", "name1", "ch01", 0.0, 0.0, 1.0, 1.0});
    std::vector<parking::ParkingSlotConfig> slots{
        {"EV01", true, "EV", "hall-ev01",
         {{"cam01", "vs-0", "name1", true, 0}}}};

    database::EventDatabase database(temporary.path / "parking.sqlite3");
    const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sql_dir / "schema.sql", sql_dir / "seed_test.sql");

    auto channel = std::make_shared<camera::CameraChannel>();
    channel->camera_id = "cam01";
    channel->channel_id = "ch01";
    channel->latest_full_frame = cv::Mat(
        120, 160, CV_8UC3, cv::Scalar(20, 80, 140));
    std::vector<std::shared_ptr<camera::CameraChannel>> channels{channel};
    std::atomic<bool> running{true};
    snapshot::SnapshotStorage storage(
        (temporary.path / "snapshots").string(), 32, running);

    parking_timer::EventManager events;
    events.setPublisher(
        [](std::string_view, std::int64_t, std::string_view,
           std::string_view, std::string_view, std::string_view) {
            return true;
        });
    parking_timer::ParkingSlotManager timer(database, events, 10s);
    require(timer.start(), "causal-order timer did not start");
    parking::EvidenceCaptureWorker::Config evidence_config;
    evidence_config.overstayDelay = 10s;
    parking::EvidenceCaptureWorker evidence(
        storage, database, evidence_config);
    require(evidence.start(), "causal-order evidence worker did not start");

    ActorCheckpointGate gate;
    sensor::HallParkingService service(
        slots, config, channels, database, {}, timer, events, evidence,
        nullptr, {}, [&gate](const parking::SlotActorCheckpoint checkpoint) {
            gate(checkpoint);
        });
    ocr::OcrWorker ocr_worker(
        ocr::GeminiOcrClient("", "test-model", 1, 1), database, false);
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    mqtt::MqttEventBridge bridge(
        config, channels, database, storage, ocr_worker,
        slots, {}, {},
        [&service](const event::IvaOccupancySignal& signal) {
            return service.handleCameraIvaSignal(signal);
        },
        std::move(transport));
    require(bridge.start(), "production causal-order bridge did not start");

    const std::string topic =
        "cam01/onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0/intrusion";
    const auto emit = [&](const std::string& utc,
                          const std::string& state,
                          const std::string& action) {
        fake->emitRaw(
            topic,
            "{\"UtcTime\":\"" + utc +
                "\",\"Source\":{\"VideoSourceToken\":\"vs-0\","
                "\"RuleName\":\"name1\"},\"Data\":{\"State\":\"" +
                state + "\",\"ObjectId\":41808,\"Action\":\"" + action +
                "\"}}");
    };

    emit("2026-08-11T04:00:00.000Z", "true", "Intrusion");
    require(waitUntil([&] { return database.findActiveBySlot("EV01").has_value(); }),
            "production bridge intrusion did not create a session");
    const auto first = database.findActiveBySlot("EV01");
    require(first.has_value(), "production bridge first session is missing");

    // Producer wins: queue INTRUSION while the actor is immediately before
    // deadline linearization. The still-SCHEDULED deadline must be superseded.
    emit("2026-08-11T04:00:01.000Z", "false", "Exit");
    std::optional<std::int64_t> first_due;
    require(waitUntil([&] {
                first_due = database.nextScheduledSlotDeadlineEpochMs();
                return first_due.has_value();
            }),
            "first production EXIT did not persist its deadline");
    gate.arm(parking::SlotActorCheckpoint::BeforeDeadlineLinearization,
             *first_due);
    gate.waitEntered("actor did not reach pre-deadline linearization");
    emit("2026-08-11T04:00:02.000Z", "true", "Intrusion");
    gate.release();
    require(waitUntil([&] {
                const auto active = database.findActiveBySlot("EV01");
                return active && active->id == first->id &&
                    !database.nextScheduledSlotDeadlineEpochMs().has_value();
            }),
            "queued intrusion did not supersede the old scheduled EXIT");

    // Deadline wins: hold after its durable ADMITTED command exists, enqueue a
    // later INTRUSION, then release. The old session must close first and the
    // later observation must create a new generation.
    emit("2026-08-11T04:00:03.000Z", "false", "Exit");
    require(waitUntil([&] {
                return database.nextScheduledSlotDeadlineEpochMs().has_value();
            }),
            "second production EXIT did not persist its deadline");
    gate.arm(parking::SlotActorCheckpoint::AfterDeadlineAdmission);
    gate.waitEntered("actor did not expose admitted deadline checkpoint");
    emit("2026-08-11T04:00:04.000Z", "true", "Intrusion");
    gate.release();
    require(waitUntil([&] {
                const auto active = database.findActiveBySlot("EV01");
                return active && active->id != first->id;
            }),
            "admitted old EXIT did not precede the later intrusion");
    const auto ended = database.findLogById(first->id);
    require(ended && ended->departed_at.has_value(),
            "deadline-first ordering did not end the exact old session");

    require(bridge.stop(), "production causal-order bridge did not stop");
    require(service.stop(3s), "production causal-order actor did not drain");
    evidence.stop();
}

}  // namespace

int main() {
    try {
        testProductionBridgeSeesPreconstructedFireTarget();
        testProductionBridgeRejectsMissingFireTarget();
        testHeldProductionFireAckOutlivesShutdownLease();
        testRegularPermitCannotCrossReconnectEpoch();
        testCameraIvaUsesSourceTimeAndAreaIdentity();
        testOnvifModeSuppressesOnlyMqttIvaTransitions();
        testCh3PublicationSharesNativeWiseAiAreaIdentity();
        testProductionBridgeAndActorOrderBothSidesOfDeadline();
        std::cout << "MQTT bridge lifecycle integration tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

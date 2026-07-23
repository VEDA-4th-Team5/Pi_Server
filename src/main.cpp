#include "app/AppConfig.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "event/FireAlarmManager.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "parking/ActiveParkingSessionIndex.hpp"
#include "parking/ParkingSessionWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorLinkManager.hpp"
#include "snapshot/SnapshotStorage.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace {
// signal handler와 모든 작업 스레드가 함께 보는 전역 종료 플래그이다.
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

std::vector<std::shared_ptr<camera::CameraChannel>> createCameraChannels(
    const app::AppConfig& config
) {
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;

    // shared_ptr를 쓰므로 RTSP, MQTT, BestShot 모듈이 같은 채널 상태를 공유한다.
    for (const auto& rtsp_config : config.rtsp_channels) {
        auto channel = std::make_shared<camera::CameraChannel>();

        channel->camera_id = config.camera_id;
        channel->channel_id = rtsp_config.channel_id;
        channel->rtsp_url = rtsp_config.rtsp_url;

        channels.push_back(channel);
    }

    return channels;
}

}
 
int main() {
    // OpenCV가 내부적으로 사용하는 FFmpeg에 TCP 전송과 타임아웃을 지정한다.
    // UDP보다 지연은 조금 늘 수 있지만 CCTV 스트림의 패킷 손실에 더 안정적이다.
    setenv(
        "OPENCV_FFMPEG_CAPTURE_OPTIONS",
        "rtsp_transport;tcp|stimeout;5000000|max_delay;500000",
        0
    );

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    app::AppConfig config = app::AppConfig::loadFromEnv();

    util::logInfo("pi-server started");
    util::logInfo("camera_id=" + config.camera_id);

    std::vector<std::shared_ptr<camera::CameraChannel>> channels =
        createCameraChannels(config);

    {
        std::ostringstream oss;
        oss << "channel_count=" << channels.size();
        util::logInfo(oss.str());
    }

    util::logInfo("headless mode: cv::imshow disabled");
    util::logInfo("snapshot mode: ALL CHANNELS FULL-SIZE original frame");
    util::logInfo("preview mode: " + std::to_string(config.preview_width) + "x" + std::to_string(config.preview_height) + " internal frame");

    if (channels.empty()) {
        util::logError("No RTSP URL configured");
        util::logError("HINT: export CAMERA_RTSP='rtsp://USER:PASSWORD@CAMERA_IP:554/profile2/media.smp'");
        util::logError("HINT: or set CAMERA_RTSP_CH1~CAMERA_RTSP_CH4");
        return 1;
    }

    // 초기화 순서: DB -> RTSP -> 최초 프레임 -> BestShot -> MQTT.
    // 최초 프레임 전에 MQTT를 받으면 저장할 영상이 없기 때문에 이 순서가 중요하다.
    database::EventDatabase database;

    if (!database.open(config.db_path)) {
        return 1;
    }

    std::unique_ptr<http::ParkingHttpServer> http_server;
    if (config.http_api_enabled) {
        http::ServerConfig http_config;
        http_config.listen_address = config.http_listen_address;
        http_config.port = config.http_port;
        http_config.tls_certificate_path = config.http_tls_certificate_path;
        http_config.tls_private_key_path = config.http_tls_private_key_path;
        http_config.data_root = config.http_data_root;
        http_config.max_image_bytes = static_cast<std::size_t>(
            std::max(1, config.http_max_image_mb)) * 1024U * 1024U;
        http_server = std::make_unique<http::ParkingHttpServer>(database,
                                                                http_config);
        if (!http_server->start()) {
            database.close();
            return 1;
        }
    }

    camera::RtspStreamReceiver rtsp_receiver(
        channels,
        config.preview_width,
        config.preview_height,
        config.rtsp_retry_delay_ms,
        config.empty_frame_delay_ms,
        config.max_consecutive_read_failures,
        g_running
    );

    snapshot::SnapshotStorage snapshot_storage(
        config.snapshot_dir,
        config.snapshot_frame_wait_ms,
        g_running
    );

    parking::ParkingTriggerCoordinator trigger_coordinator(
        config.bestshot_correlation_window_ms,
        config.iva_duplicate_suppression_ms
    );

    ocr::GeminiOcrClient gemini_client(
        config.gemini_api_key,
        config.gemini_model,
        config.gemini_connect_timeout_sec,
        config.gemini_request_timeout_sec,
        config.gemini_fallback_model
    );
    ocr::OcrWorker ocr_worker(
        std::move(gemini_client), database,
        config.plate_preprocess_mode != "off");

    bestshot::BestShotReceiver bestshot_receiver(
        channels, database, trigger_coordinator, ocr_worker, g_running);

    rtsp_receiver.start();

    if (!rtsp_receiver.waitForInitialFrames(config.initial_frame_timeout_sec)) {
        g_running.store(false);
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        database.close();
        return 1;
    }

    ocr_worker.start();
    bestshot_receiver.start();

    mqtt::MqttEventBridge mqtt_bridge(
        config,
        channels,
        database,
        snapshot_storage,
        trigger_coordinator,
        ocr_worker
    );

    if (!mqtt_bridge.start()) {
        g_running.store(false);
        bestshot_receiver.stop();
        ocr_worker.stop();
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        database.close();
        return 1;
    }

    // 홀센서 주차 점유 경로: STM32 --UART--> SensorLinkManager --콜백-->
    //   ParkingSensorEventAdapter(sensor_id->slot) --> ParkingSessionWorker
    //   (VACANT/OCCUPIED 상태머신) --> sink(활성세션 index, 로깅, 이후 촬영 스케줄러).
    // 화재 경로와 같은 STM32 UART 링크(sensor_link)를 공유한다. Service 가 Core 를
    // include 하지 않도록 모든 배선은 여기(main)에서만 한다.
    std::vector<parking::ParkingSlotConfig> parking_slot_configs;
    std::unique_ptr<parking::SensorSlotIndex> sensor_slot_index;
    std::unique_ptr<sensor::ParkingSensorEventAdapter> parking_adapter;
    std::unique_ptr<parking::ActiveParkingSessionIndex> active_session_index;
    std::unique_ptr<parking::ParkingSessionWorker> parking_worker;

    if (config.parking_hall_enabled) {
        try {
            parking_slot_configs =
                parking::ParkingSlotConfigLoader::loadFromFile(
                    config.parking_slots_config_path);
        } catch (const std::exception& error) {
            util::logWarn(
                std::string("parking hall disabled: cannot load ") +
                config.parking_slots_config_path + ": " + error.what());
        }

        if (parking_slot_configs.empty()) {
            util::logWarn(
                "parking hall enabled but no slots loaded; hall path off");
        } else {
            sensor_slot_index = std::make_unique<parking::SensorSlotIndex>(
                parking_slot_configs);
            parking_adapter =
                std::make_unique<sensor::ParkingSensorEventAdapter>(
                    *sensor_slot_index);
            active_session_index =
                std::make_unique<parking::ActiveParkingSessionIndex>();
            parking_worker =
                std::make_unique<parking::ParkingSessionWorker>(
                    parking_slot_configs);

            // sink 1: 활성 세션 read model 갱신 (IVA/BestShot 워커가 조회).
            parking::ActiveParkingSessionIndex* active_index_ptr =
                active_session_index.get();
            parking_worker->addSink(
                [active_index_ptr](
                    const parking::ParkingTransitionResult& transition) {
                    active_index_ptr->apply(transition);
                });

            // sink 2: 실제 상태 변화만 로깅 (1~2초 폴링 중복은 남기지 않는다).
            parking_worker->addSink(
                [](const parking::ParkingTransitionResult& transition) {
                    if (transition.changed()) {
                        util::logLine(
                            "PARKING_SESSION",
                            std::string(parking::toString(transition.code)) +
                                " slot=" + transition.slotId +
                                " session=" + transition.sessionId);
                    }
                });

            util::logInfo(
                "parking hall path enabled: slots=" +
                std::to_string(parking_worker->slotCount()) +
                " config=" + config.parking_slots_config_path);
        }
    }

    // 화재 후보 경로: STM32 --UART--> SensorLinkManager --콜백--> FireAlarmManager
    //                 --콜백--> MqttEventBridge::publish --> Qt.
    // 화재와 홀센서는 같은 STM32 UART 링크를 공유하므로 둘 중 하나라도 켜지면
    // sensor_link 를 만든다. Service 계층이 Core 를 include 하지 않도록 배선은
    // 여기(main)에서만 한다.
    std::unique_ptr<event::FireAlarmManager> fire_alarm_manager;
    std::unique_ptr<sensor::SensorLinkManager> sensor_link;

    if (config.fire_alarm_enabled || parking_worker) {
        if (config.fire_alarm_enabled) {
            auto bindings =
                event::parseFireSensorBindings(config.fire_sensor_slot_map);
            if (bindings.empty()) {
                util::logWarn(
                    "FIRE_ALARM_ENABLED but FIRE_SENSOR_SLOT_MAP is empty; "
                    "alarms will be published without a slot_id");
            }

            fire_alarm_manager = std::make_unique<event::FireAlarmManager>(
                config.camera_id,
                config.default_channel_id,
                config.fire_topic_prefix,
                std::move(bindings),
                [&mqtt_bridge](const std::string& topic,
                               const std::string& payload) {
                    return mqtt_bridge.publish(topic, payload);
                });
        }

        sensor::SensorLinkConfig link_config;
        link_config.devicePath = config.fire_uart_device;
        link_config.baudRate = config.fire_uart_baud;
        link_config.reopenDelayMs = config.fire_uart_reopen_delay_ms;

        sensor_link = std::make_unique<sensor::SensorLinkManager>(
            link_config, g_running);

        if (fire_alarm_manager) {
            sensor_link->setFireHandler(
                [&fire_alarm_manager](
                    const sensor::FireSensorMessage& message) {
                    event::FireSignal signal;
                    signal.sensorId = message.sensorId;
                    signal.detected =
                        message.state == sensor::FireSensorState::Detected;
                    signal.occurredAt = message.occurredAt;
                    signal.sourceSequence = message.sequence;
                    signal.sourceTransport = message.transport;
                    signal.rawPayload = message.raw;
                    fire_alarm_manager->onFireSignal(signal);
                });
        }

        if (parking_worker) {
            // 주차 점유 신호를 상태머신 워커로 흘려보낸다.
            sensor::ParkingSensorEventAdapter* adapter_ptr =
                parking_adapter.get();
            parking::ParkingSessionWorker* worker_ptr = parking_worker.get();
            sensor_link->setParkingHandler(
                [adapter_ptr, worker_ptr](
                    const sensor::SensorProtocolMessage& message) {
                    std::string error;
                    const auto event = adapter_ptr->adapt(message, &error);
                    if (!event) {
                        util::logLine(
                            "SENSOR_UART",
                            "parking sensor ignored: " + error +
                                " sensor=" + message.sensorId);
                        return;
                    }
                    worker_ptr->onSensorEvent(*event);
                });
        } else {
            sensor_link->setParkingHandler(
                [](const sensor::SensorProtocolMessage& message) {
                    util::logLine(
                        "SENSOR_UART",
                        "parking sensor not wired (PARKING_HALL_ENABLED off): "
                        "sensor=" +
                            message.sensorId);
                });
        }

        if (!sensor_link->start()) {
            util::logWarn("sensor link disabled");
            sensor_link.reset();
        }
    }

    util::logInfo("waiting for camera MQTT events...");
    util::logInfo("press Ctrl+C to stop");

    // 실제 작업은 각 모듈의 작업 스레드가 수행하고 main은 종료 신호를 기다린다.
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 생성의 역순으로 정리하여 사용 중인 자원이 먼저 사라지는 것을 막는다.
    // sensor_link 가 mqtt_bridge 를 콜백으로 잡고 있으므로 먼저 멈춘다.
    if (sensor_link) sensor_link->stop();
    mqtt_bridge.stop();
    bestshot_receiver.stop();
    ocr_worker.stop();
    rtsp_receiver.stop();
    if (http_server) http_server->stop();
    database.close();

    util::logInfo("pi-server stopped");

    return 0;
}

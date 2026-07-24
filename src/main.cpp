#include "app/AppConfig.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "device/SensorLinkManager.hpp"
#include "event/SystemEventReporter.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "snapshot/SnapshotStorage.hpp"
#include "sensor/HallParkingService.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
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

const app::IvaAreaConfig* findArea(const app::AppConfig& config,
                                   const std::string& slot_id) {
    for (const auto& area : config.iva_areas)
        if (area.slot_id == slot_id) return &area;
    return nullptr;
}

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channel_id) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channel_id) return channel;
    return nullptr;
}

std::string captureSlotSnapshot(
    const app::AppConfig& config,
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    snapshot::SnapshotStorage& storage,
    const std::string& slot_id) {
    const auto* area = findArea(config, slot_id);
    if (!area) return {};
    auto channel = findChannel(channels, area->channel_id);
    if (!channel) return {};
    return storage.saveIvaAreaSnapshot(
        channel, slot_id,
        {area->roi_x, area->roi_y, area->roi_width, area->roi_height});
}

std::string buildQtParkingEvent(
    const app::AppConfig& config,
    database::EventDatabase& database,
    const std::string_view event_type,
    const std::int64_t session_id,
    const std::string_view slot_id,
    const std::string_view plate_number,
    const std::string_view timestamp,
    const std::string_view detail) {
    const auto* area = findArea(config, std::string(slot_id));
    std::string external_type(event_type);
    if (event_type == "VIOLATION_TRIGGERED") external_type = "OVERTIME_VIOLATION";
    else if (event_type == "DEPARTURE") external_type = "SLOT_VACATED";
    else if (event_type == "TIMER_STARTED") external_type = "PLATE_RECOGNIZED";
    const bool alarm = external_type == "OVERTIME_VIOLATION";
    const bool vacant = external_type == "SLOT_VACATED" ||
                        external_type == "EARLY_DEPARTURE_IMAGES_DELETED";
    std::string vehicle_type = "UNKNOWN";
    if (!plate_number.empty()) {
        vehicle_type = parking_timer::toString(
            database.classifyVehicle(plate_number));
    }
    std::ostringstream output;
    output << "{\"event_id\":\"session-" << session_id << '-'
           << util::jsonEscape(std::string(timestamp)) << "\","
           << "\"event_type\":\"" << util::jsonEscape(external_type) << "\","
           << "\"session_id\":" << session_id << ','
           << "\"slot_id\":\"" << util::jsonEscape(std::string(slot_id)) << "\","
           << "\"channel_id\":\""
           << util::jsonEscape(area ? area->channel_id : "") << "\","
           << "\"plate_number\":\""
           << util::jsonEscape(std::string(plate_number)) << "\","
           << "\"vehicle_type\":\"" << util::jsonEscape(vehicle_type) << "\","
           << "\"ocr_status\":\""
           << (plate_number.empty() ? "PENDING" : "RECOGNIZED") << "\","
           << "\"parking_state\":\"" << (vacant ? "VACANT" : "OCCUPIED") << "\","
           << "\"occupied_seconds\":"
           << (alarm ? config.parking_timeout_seconds : 0) << ','
           << "\"alarm_state\":\"" << (alarm ? "OPEN" : "NONE") << "\","
           << "\"roi\":{"
           << "\"x\":" << (area ? area->roi_x : 0.0) << ','
           << "\"y\":" << (area ? area->roi_y : 0.0) << ','
           << "\"width\":" << (area ? area->roi_width : 0.0) << ','
           << "\"height\":" << (area ? area->roi_height : 0.0) << "},"
           << "\"session_images_url\":\"/api/v1/parking-sessions/"
           << session_id << "/images\","
           << "\"evidence_path\":\"" << util::jsonEscape(std::string(detail)) << "\","
           << "\"timestamp\":\"" << util::jsonEscape(std::string(timestamp)) << "\"}";
    return output.str();
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

    // 센서/통신 스레드는 DB I/O를 직접 기다리지 않고 bounded reporter queue에 기록한다.
    event::SystemEventReporter system_event_reporter(
        [&database](const event::SystemEvent& system_event,
                    const std::string& message) {
            return database.insertSystemEvent(
                event::toString(system_event.code), system_event.slot_id, message);
        });
    event::SystemEventReporter* system_event_sink =
        system_event_reporter.start() ? &system_event_reporter : nullptr;
    if (system_event_sink == nullptr)
        util::logError("System event reporter disabled after start failure");

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
            system_event_reporter.stop();
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

    std::unique_ptr<parking_timer::EventManager> timer_events;
    std::unique_ptr<parking_timer::ParkingSlotManager> parking_timer;
    if (config.parking_timer_enabled) {
        timer_events = std::make_unique<parking_timer::EventManager>();
        parking_timer = std::make_unique<parking_timer::ParkingSlotManager>(
            database, *timer_events,
            std::chrono::seconds(config.parking_timeout_seconds),
            [&config, &channels, &snapshot_storage](
                std::int64_t, const std::string& slot_id, const std::string&) {
                return captureSlotSnapshot(
                    config, channels, snapshot_storage, slot_id);
            });
        util::logInfo("parking timer enabled: timeout=" +
                      std::to_string(config.parking_timeout_seconds) + "s");
    }

    ocr::GeminiOcrClient gemini_client(
        config.gemini_api_key,
        config.gemini_model,
        config.gemini_connect_timeout_sec,
        config.gemini_request_timeout_sec,
        config.gemini_fallback_model
    );
    ocr::OcrWorker ocr_worker(
        std::move(gemini_client), database,
        config.plate_preprocess_mode != "off",
        [&parking_timer](const ocr::OcrWorker::RecognitionResult& result) {
            if (parking_timer) {
                parking_timer->handleRecognizedSession(
                    result.session_id, result.slot_id, result.plate_number);
            }
        });

    bestshot::BestShotReceiver bestshot_receiver(
        channels, database, trigger_coordinator, ocr_worker, g_running);

    const auto sensor_link_mode =
        device::SensorLinkManager::parseMode(config.sensor_link_mode);
    std::unique_ptr<sensor::HallParkingService> hall_service;
    if (parking_timer && (config.hall_mqtt_input_enabled ||
                          sensor_link_mode != device::SensorLinkMode::Disabled)) {
        auto slot_configs = parking::ParkingSlotConfigLoader::loadFromFile(
            config.parking_slot_config_path);
        hall_service = std::make_unique<sensor::HallParkingService>(
            std::move(slot_configs), config, channels, snapshot_storage,
            database,
            [&ocr_worker](int session_id, const std::string& slot_id,
                          const std::string& path) {
                ocr_worker.enqueue(session_id, slot_id, path);
            },
            [&ocr_worker](int session_id) {
                ocr_worker.cancelSession(session_id);
            },
            *parking_timer, *timer_events, system_event_sink);
    }

    rtsp_receiver.start();

    if (!rtsp_receiver.waitForInitialFrames(config.initial_frame_timeout_sec)) {
        g_running.store(false);
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        hall_service.reset();
        parking_timer.reset();
        timer_events.reset();
        system_event_reporter.stop();
        database.close();
        return 1;
    }

    ocr_worker.start();
    bestshot_receiver.start();

    std::unique_ptr<device::SensorLinkManager> sensor_link;
    if (hall_service && sensor_link_mode != device::SensorLinkMode::Disabled) {
        device::SensorLinkManager::Config sensor_config;
        sensor_config.mode = sensor_link_mode;
        sensor_config.uart.device_path = config.sensor_uart_device;
        sensor_config.uart.baud_rate = config.sensor_uart_baud_rate;
        sensor_config.uart.read_timeout_ms = config.sensor_uart_read_timeout_ms;
        sensor_config.reconnect_delay_ms = config.sensor_uart_reconnect_ms;
        sensor_link = std::make_unique<device::SensorLinkManager>(
            std::move(sensor_config),
            [&hall_service](const std::string& line,
                            const std::string& transport) {
                if (hall_service) hall_service->handleLine(line, transport);
            }, system_event_sink);
        if (!sensor_link->start()) {
            util::logError("Sensor UART/LoRa link could not be started");
            sensor_link.reset();
        }
    }

    mqtt::MqttEventBridge mqtt_bridge(
        config,
        channels,
        database,
        snapshot_storage,
        trigger_coordinator,
        ocr_worker,
        [&hall_service](const std::string& line) {
            if (hall_service) hall_service->handleLine(line);
        }
    );

    if (!mqtt_bridge.start()) {
        g_running.store(false);
        if (sensor_link) sensor_link->stop();
        bestshot_receiver.stop();
        ocr_worker.stop();
        hall_service.reset();
        parking_timer.reset();
        timer_events.reset();
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        system_event_reporter.stop();
        database.close();
        return 1;
    }

    if (timer_events) {
        timer_events->setPublisher(
            [&mqtt_bridge, &config, &database](
                const std::string_view event_type, const std::int64_t session_id,
                const std::string_view slot_id, const std::string_view plate,
                const std::string_view timestamp, const std::string_view detail) {
                if (slot_id.empty() || session_id < 0) return;
                const std::string payload = buildQtParkingEvent(
                    config, database, event_type, session_id, slot_id, plate,
                    timestamp, detail);
                const std::string event_topic =
                    "parking/v1/events/" + std::string(slot_id);
                const std::string state_topic =
                    "parking/v1/state/" + std::string(slot_id);
                mqtt_bridge.publishQtEvent(event_topic, payload, 1, false);
                if (event_type != "EARLY_DEPARTURE_IMAGES_DELETED")
                    mqtt_bridge.publishQtEvent(state_topic, payload, 1, true);
            });
    }
    if (parking_timer) {
        const auto restored = parking_timer->restoreActiveSessions();
        util::logInfo("parking timer restored active sessions=" +
                      std::to_string(restored));
    }

    util::logInfo("waiting for camera MQTT events...");
    util::logInfo("press Ctrl+C to stop");

    // 실제 작업은 각 모듈의 작업 스레드가 수행하고 main은 종료 신호를 기다린다.
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 생성의 역순으로 정리하여 사용 중인 자원이 먼저 사라지는 것을 막는다.
    mqtt_bridge.stop();
    if (sensor_link) sensor_link->stop();
    bestshot_receiver.stop();
    ocr_worker.stop();
    hall_service.reset();
    parking_timer.reset();
    timer_events.reset();
    rtsp_receiver.stop();
    if (http_server) http_server->stop();
    system_event_reporter.stop();
    database.close();

    util::logInfo("pi-server stopped");

    return 0;
}

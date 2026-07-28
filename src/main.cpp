#include "app/AppConfig.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "device/SensorLinkManager.hpp"
#include "event/SystemEventReporter.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "parking/CaptureRequest.hpp"
#include "parking/CaptureScheduler.hpp"
#include "parking/CaptureSchedulerRuntime.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "parking/HallCaptureExecutor.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorLinkManager.hpp"
#include "snapshot/SnapshotStorage.hpp"
#include "sensor/HallParkingService.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"
#include "util/TimeUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
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

// EVDA-138에서 카메라 응답 규약이 확정되기 전 사용하는 MQTT 요청 초안이다.
// publish 성공은 Broker 접수만 의미하며 실제 카메라 촬영 성공을 뜻하지 않는다.
std::string buildCaptureRequestPayload(
    const parking::CaptureRequest& request) {
    std::ostringstream output;
    output << "{\"schema\":\"capture_request_draft_v0\","
           << "\"session_id\":\"" << util::jsonEscape(request.sessionId)
           << "\",\"slot_id\":\"" << util::jsonEscape(request.slotId)
           << "\",\"sensor_id\":\"" << util::jsonEscape(request.sensorId)
           << "\",\"camera_id\":\""
           << util::jsonEscape(request.target.cameraId)
           << "\",\"channel_id\":\""
           << util::jsonEscape(request.target.channelId)
           << "\",\"area_name\":\""
           << util::jsonEscape(request.target.areaName)
           << "\",\"reason\":\"" << parking::toReasonString(request.reason)
           << "\",\"attempt\":" << request.attempt
           << ",\"roi\":{\"x\":" << request.target.roiX
           << ",\"y\":" << request.target.roiY
           << ",\"w\":" << request.target.roiWidth
           << ",\"h\":" << request.target.roiHeight << "},"
           << "\"response_timeout_ms\":" << request.responseTimeout.count()
           << ",\"occupied_at\":\""
           << util::jsonEscape(util::isoString(request.sessionStartedAt))
           << "\",\"requested_at\":\""
           << util::jsonEscape(util::nowIsoString()) << "\"}";
    return output.str();
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
    try {
        database.migrateRuntimeSchema();
    } catch (const std::exception& error) {
        util::logError("Runtime DB migration failed: " +
                       std::string(error.what()));
        database.close();
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

    mqtt::MqttEventBridge* capture_mqtt_bridge = nullptr;
    std::unique_ptr<parking::CaptureScheduler> capture_scheduler;
    std::unique_ptr<parking::CaptureSchedulerRuntime> capture_runtime;
    if (config.capture_sched_enabled) {
        parking::CaptureSchedulerConfig scheduler_config;
        scheduler_config.responseTimeout = std::chrono::milliseconds(
            config.capture_response_timeout_ms);
        scheduler_config.retryInterval = std::chrono::milliseconds(
            config.capture_retry_interval_ms);
        scheduler_config.maxRetries = config.capture_max_retries;

        std::vector<int> offset_seconds;
        std::istringstream offset_stream(config.capture_offsets_sec);
        std::string offset_token;
        while (std::getline(offset_stream, offset_token, ',')) {
            try {
                offset_seconds.push_back(std::max(0, std::stoi(offset_token)));
            } catch (const std::exception&) {
                util::logWarn("invalid CAPTURE_OFFSETS_SEC token ignored: " +
                              offset_token);
            }
        }
        if (!offset_seconds.empty()) {
            scheduler_config.offsets.clear();
            scheduler_config.offsets.push_back(
                {parking::CaptureReason::HallOccupied30s,
                 std::chrono::seconds(offset_seconds[0])});
            if (offset_seconds.size() > 1) {
                scheduler_config.offsets.push_back(
                    {parking::CaptureReason::HallOccupied60s,
                     std::chrono::seconds(offset_seconds[1])});
            }
        }

        parking::CaptureTargetResolver resolver =
            [&config](const std::string& slot_id)
            -> std::optional<parking::CaptureTarget> {
            const auto* area = findArea(config, slot_id);
            if (area == nullptr) return std::nullopt;
            return parking::CaptureTarget{
                config.camera_id, area->channel_id, area->area_name,
                area->roi_x, area->roi_y, area->roi_width, area->roi_height};
        };
        capture_scheduler = std::make_unique<parking::CaptureScheduler>(
            std::move(scheduler_config), std::move(resolver));
    }

    std::unique_ptr<parking_timer::EventManager> timer_events;
    std::unique_ptr<parking_timer::ParkingSlotManager> parking_timer;
    if (config.parking_timer_enabled) {
        timer_events = std::make_unique<parking_timer::EventManager>();
        parking_timer = std::make_unique<parking_timer::ParkingSlotManager>(
            database, *timer_events,
            std::chrono::seconds(config.parking_timeout_seconds),
            [&database](
                std::int64_t session_id, const std::string& slot_id,
                const std::string&) {
                // 새 증거 worker가 T0 기준 이미지를 이미 저장했다면 같은 파일을
                // 재사용한다. 이전 DB/촬영 실패 세션만 기존 Snapshot으로 보완한다.
                if (const auto existing = database.findEvidenceImagePath(
                        session_id, "OVERSTAY_EVIDENCE")) {
                    return *existing;
                }
                util::logWarn("Timer reached before overstay evidence was ready: "
                              "session=" + std::to_string(session_id) +
                              " slot=" + slot_id);
                return std::string{};
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

    parking::HallCapturePorts hall_capture_ports;
    hall_capture_ports.writeImageLog =
        [&database](const parking::CapturedImage& image) {
            try {
                const auto result = database.insertHallCaptureImage(
                    image.sessionId, image.originalPath, image.enhancedPath,
                    parking::toEnhancementType(image.stage),
                    parking_timer::utcNow());
                switch (result) {
                    case database::EvidenceInsertResult::Inserted:
                        return parking::ImageStoreResult::Inserted;
                    case database::EvidenceInsertResult::Duplicate:
                        return parking::ImageStoreResult::Duplicate;
                    case database::EvidenceInsertResult::InactiveSession:
                        return parking::ImageStoreResult::InactiveSession;
                }
            } catch (const std::exception& error) {
                util::logError("hall capture DB insert failed: " +
                               std::string(error.what()));
            }
            return parking::ImageStoreResult::Failed;
        };
    hall_capture_ports.submitOcr =
        [&ocr_worker](const parking::CapturedImage& image) {
            ocr_worker.enqueueHallCapture({
                image.sessionId,
                image.stage == parking::CaptureStage::Second60s ? 1 : 0,
                image.slotId,
                image.originalPath,
                image.enhancedPath});
        };
    hall_capture_ports.writeOcrFailure =
        [&database](const std::int64_t session_id,
                    const std::string& slot_id, const int attempts) {
            if (!database.markPlateOcrUnresolved(
                    session_id, slot_id, attempts)) {
                util::logError("hall OCR failure log write failed: session=" +
                               std::to_string(session_id));
            }
        };
    hall_capture_ports.registerEvTimer =
        [&parking_timer](const std::int64_t session_id,
                         const std::string& slot_id,
                         const std::string& plate_number) {
            if (parking_timer) {
                parking_timer->handleRecognizedSession(
                    session_id, slot_id, plate_number);
            }
        };
    auto hall_ocr_coordinator =
        std::make_unique<parking::HallCaptureCoordinator>(
            std::move(hall_capture_ports), config.capture_ocr_max_attempts);
    ocr_worker.setHallCaptureCallback(
        [&hall_ocr_coordinator](const ocr::HallCaptureResult& result) {
            if (!hall_ocr_coordinator) return;
            hall_ocr_coordinator->onOcrOutcome({
                result.session_id,
                result.stage == 1 ? parking::CaptureStage::Second60s
                                  : parking::CaptureStage::First30s,
                result.recognized,
                result.plate_number,
                result.confidence,
                result.classification});
        });

    std::unique_ptr<parking::HallCaptureExecutor> hall_capture_executor;
    if (capture_scheduler) {
        const std::string topic_prefix = config.capture_topic_prefix;
        if (config.hall_capture_ocr_enabled) {
            hall_capture_executor =
                std::make_unique<parking::HallCaptureExecutor>(
                    channels, snapshot_storage, *hall_ocr_coordinator,
                    [&capture_mqtt_bridge, topic_prefix](
                        const parking::CaptureRequest& request) {
                        return capture_mqtt_bridge != nullptr &&
                            capture_mqtt_bridge->publishApplicationEvent(
                                topic_prefix + "/" + request.slotId,
                                buildCaptureRequestPayload(request), 1, false);
                    });
            capture_runtime =
                std::make_unique<parking::CaptureSchedulerRuntime>(
                    *capture_scheduler,
                    [&hall_capture_executor](
                        const parking::CaptureRequest& request) {
                        return hall_capture_executor &&
                               hall_capture_executor->execute(request);
                    });
        } else {
            capture_runtime =
                std::make_unique<parking::CaptureSchedulerRuntime>(
                    *capture_scheduler,
                    [&capture_mqtt_bridge, topic_prefix](
                        const parking::CaptureRequest& request) {
                        return capture_mqtt_bridge != nullptr &&
                            capture_mqtt_bridge->publishApplicationEvent(
                                topic_prefix + "/" + request.slotId,
                                buildCaptureRequestPayload(request), 1, false);
                    });
        }
    }

    parking::EvidenceCaptureWorker::Config evidence_config;
    evidence_config.overstayDelay = std::chrono::seconds(
        config.parking_overstay_evidence_delay_seconds);
    auto evidence_worker = std::make_unique<parking::EvidenceCaptureWorker>(
        snapshot_storage, database, evidence_config,
        [&ocr_worker, &timer_events, &config](
            const parking::EvidenceCaptureResult& result) {
            if (!result.stored) return;
            if (result.reason == parking::EvidenceReason::OccupancyStart) {
                // 30/60초 홀 OCR이 활성화되면 차량이 자리를 잡은 뒤의 ROI를
                // 사용한다. 스케줄러가 꺼진 환경에서는 기존 시작 증거 OCR로
                // fallback하여 번호판 인식 기능이 사라지지 않게 한다.
                if (!(config.hall_capture_ocr_enabled &&
                      config.capture_sched_enabled)) {
                    ocr_worker.enqueue(static_cast<int>(result.sessionId),
                                       result.slotId, result.imagePath);
                }
                if (timer_events) {
                    timer_events->publish(
                        "OCCUPANCY_START_EVIDENCE_STORED", result.slotId, "",
                        parking_timer::utcNow(), result.imagePath,
                        result.sessionId);
                }
            } else if (timer_events) {
                timer_events->publish(
                    "OVERSTAY_EVIDENCE_STORED", result.slotId, "",
                    parking_timer::utcNow(), result.imagePath,
                    result.sessionId);
            }
        });
    if (!evidence_worker->start()) {
        if (http_server) http_server->stop();
        system_event_reporter.stop();
        database.close();
        return 1;
    }
    util::logInfo("parking evidence worker enabled: overstay_delay=" +
                  std::to_string(
                      config.parking_overstay_evidence_delay_seconds) + "s");

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
            std::move(slot_configs), config, channels, database,
            [&ocr_worker](int session_id) {
                ocr_worker.cancelSession(session_id);
            },
            *parking_timer, *timer_events, *evidence_worker,
            system_event_sink,
            [&capture_runtime, &hall_ocr_coordinator](
                const parking::ParkingTransitionResult& transition) {
                if (hall_ocr_coordinator)
                    hall_ocr_coordinator->onTransition(transition);
                if (capture_runtime) capture_runtime->onTransition(transition);
            });
    }

    rtsp_receiver.start();

    if (!rtsp_receiver.waitForInitialFrames(config.initial_frame_timeout_sec)) {
        g_running.store(false);
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        hall_service.reset();
        evidence_worker->stop();
        parking_timer.reset();
        timer_events.reset();
        system_event_reporter.stop();
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
        ocr_worker,
        [&hall_service](const std::string& line) {
            if (hall_service) hall_service->handleLine(line);
        }
    );
    capture_mqtt_bridge = &mqtt_bridge;

    if (!mqtt_bridge.start()) {
        g_running.store(false);
        bestshot_receiver.stop();
        hall_service.reset();
        evidence_worker->stop();
        ocr_worker.stop();
        parking_timer.reset();
        timer_events.reset();
        rtsp_receiver.stop();
        if (http_server) http_server->stop();
        system_event_reporter.stop();
        database.close();
        return 1;
    }

    if (capture_runtime) {
        capture_runtime->start();
        util::logInfo(
            "capture scheduler enabled (DRAFT protocol pending EVDA-138): "
            "topic_prefix=" + config.capture_topic_prefix +
            " offsets=" + config.capture_offsets_sec + "s retries=" +
            std::to_string(config.capture_max_retries));
    }

    // 촬영 runtime과 MQTT publisher가 준비된 뒤 실제 UART/LoRa 입력을 연다.
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
    if (sensor_link) sensor_link->stop();
    if (capture_runtime) capture_runtime->stop();
    mqtt_bridge.stop();
    bestshot_receiver.stop();
    hall_service.reset();
    evidence_worker->stop();
    ocr_worker.stop();
    parking_timer.reset();
    timer_events.reset();
    rtsp_receiver.stop();
    if (http_server) http_server->stop();
    system_event_reporter.stop();
    database.close();

    util::logInfo("pi-server stopped");

    return 0;
}

#include "app/AppConfig.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "event/FireAlarmManager.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "parking/ActiveParkingSessionIndex.hpp"
#include "parking/CaptureRequest.hpp"
#include "parking/CaptureScheduler.hpp"
#include "parking/CaptureSchedulerRuntime.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "parking/HallCaptureTypes.hpp"
#include "parking/ParkingSessionWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking_timer/TimerManager.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorLinkManager.hpp"
#include "snapshot/SnapshotStorage.hpp"
#include "util/Logger.hpp"
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

// 촬영 요청 draft 페이로드. 카메라 촬영 규약(EVDA-138)이 확정되면 이 한 곳과
// 토픽 prefix 만 교체한다. id/ROI 값은 설정에서 온 단순 값이라 별도 이스케이프는
// 하지 않는다.
std::string buildCapturePayload(const parking::CaptureRequest& request) {
    std::ostringstream oss;
    oss << "{"
        << "\"schema\":\"capture_request_draft_v0\","
        << "\"session_id\":\"" << request.sessionId << "\","
        << "\"slot_id\":\"" << request.slotId << "\","
        << "\"sensor_id\":\"" << request.sensorId << "\","
        << "\"camera_id\":\"" << request.target.cameraId << "\","
        << "\"channel_id\":\"" << request.target.channelId << "\","
        << "\"area_name\":\"" << request.target.areaName << "\","
        << "\"reason\":\"" << parking::toReasonString(request.reason) << "\","
        << "\"attempt\":" << request.attempt << ","
        << "\"roi\":{"
        << "\"x\":" << request.target.roiX << ","
        << "\"y\":" << request.target.roiY << ","
        << "\"w\":" << request.target.roiWidth << ","
        << "\"h\":" << request.target.roiHeight << "},"
        << "\"response_timeout_ms\":" << request.responseTimeout.count() << ","
        << "\"occupied_at\":\"" << util::isoString(request.sessionStartedAt)
        << "\","
        << "\"requested_at\":\"" << util::nowIsoString() << "\""
        << "}";
    return oss.str();
}

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channel_id
) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channel_id) return channel;
    return nullptr;
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
    // 촬영 스케줄러/런타임보다 먼저 선언한다. 저 둘의 콜백이 타이머와
    // coordinator 를 잡고 있으므로 역순 소멸에서 나중에 사라져야 한다.
    std::unique_ptr<parking_timer::TimerManager> violation_timer;
    std::unique_ptr<parking::HallCaptureCoordinator> hall_coordinator;
    std::unique_ptr<parking::CaptureScheduler> capture_scheduler;
    std::unique_ptr<parking::CaptureSchedulerRuntime> capture_runtime;

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
                    parking_slot_configs,
                    std::chrono::milliseconds(
                        config.parking_occupancy_confirm_ms));

            if (config.parking_occupancy_confirm_ms > 0) {
                util::logInfo(
                    "parking occupancy confirm gate enabled: threshold_ms=" +
                    std::to_string(config.parking_occupancy_confirm_ms));
            }

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

            // EV/PHEV 로 확정된 세션의 충전구역 점유 한도를 재는 타이머.
            // 위반 확정 시 TimerManager 가 PARKING_SESSION 을 VIOLATION 으로
            // 바꾸고 증거 IMAGE_LOG 를 남기므로, Qt 는 기존 HTTP 조회(I-10)로
            // 본다. 별도 MQTT 위반 토픽은 규약에 없으므로 만들지 않는다.
            violation_timer = std::make_unique<parking_timer::TimerManager>(
                database,
                [](const parking_timer::ViolationEvent& violation) {
                    util::logLine(
                        "PARKING_VIOLATION",
                        "slot=" + violation.slot_id +
                            " session_id=" + std::to_string(violation.log_id) +
                            " plate=" + violation.car_number +
                            " at=" + violation.violation_at);
                },
                [](const parking_timer::TimerError& error) {
                    util::logWarn("overtime timer error slot=" + error.slot_id +
                                  " session_id=" +
                                  std::to_string(error.log_id) + ": " +
                                  error.message);
                });

            // sink 3: 홀 세션 <-> PARKING_SESSION.session_id 연결 (EVDA-136).
            // 촬영본·OCR 결과·이미지 행·이벤트 행이 모두 여기서 연 하나의
            // session_id 에 매달린다. Core 는 DB/OCR/타이머 타입을 모르고,
            // 모든 실제 동작은 아래 포트로 주입한다.
            parking::HallCapturePorts hall_ports;
            hall_ports.openSessionRow =
                [&database](const std::string& slotId) -> std::optional<int> {
                int sessionId = -1;
                if (!database.openHallSession(slotId, &sessionId))
                    return std::nullopt;
                return sessionId;
            };
            hall_ports.closeSessionRow =
                [&database](int dbSessionId, const std::string& slotId) {
                    database.closeHallSession(dbSessionId, slotId);
                };
            hall_ports.writeImageLog =
                [&database](int dbSessionId,
                            const parking::CapturedImage& image) {
                    database.attachCaptureImage(
                        dbSessionId, image.slotId, image.originalPath,
                        image.enhancedPath,
                        parking::toEnhancementType(image.stage));
                };
            hall_ports.submitOcr =
                [&ocr_worker](int dbSessionId,
                              const parking::CapturedImage& image) {
                    ocr::HallCaptureTask task;
                    task.session_key = image.sessionId;
                    task.session_id = dbSessionId;
                    task.stage =
                        image.stage == parking::CaptureStage::Second60s ? 1 : 0;
                    task.slot_id = image.slotId;
                    task.image_path = image.originalPath;
                    task.enhanced_path = image.enhancedPath;
                    ocr_worker.enqueueHallCapture(task);
                };
            hall_ports.writeOcrFailure =
                [&database](int dbSessionId, const std::string& slotId,
                            int attempts) {
                    database.markPlateOcrUnresolved(dbSessionId, slotId,
                                                     attempts);
                };
            const auto overtime =
                std::chrono::seconds(std::max(1, config.parking_overtime_sec));
            hall_ports.registerEvTimer =
                [&violation_timer, overtime](int dbSessionId,
                                             const std::string& slotId,
                                             const std::string& plateNumber) {
                    // 이미 존재하는 session_id 를 그대로 쓴다. 여기서 새 세션을
                    // 만들면 같은 주차면에 활성 세션이 둘 생겨 부분 unique
                    // index(ux_parking_session_active_slot)에 걸린다.
                    violation_timer->schedule(
                        dbSessionId, slotId, plateNumber,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            overtime));
                };

            hall_coordinator =
                std::make_unique<parking::HallCaptureCoordinator>(
                    std::move(hall_ports), config.capture_ocr_max_attempts);

            parking::HallCaptureCoordinator* hall_coordinator_ptr =
                hall_coordinator.get();
            parking_worker->addSink(
                [hall_coordinator_ptr](
                    const parking::ParkingTransitionResult& transition) {
                    hall_coordinator_ptr->onTransition(transition);
                });

            // OCR 결과를 정책으로 되돌린다. 워커 스레드에서 불린다.
            ocr_worker.setHallCaptureCallback(
                [hall_coordinator_ptr](const ocr::HallCaptureResult& result) {
                    parking::HallOcrOutcome outcome;
                    outcome.sessionId = result.session_key;
                    outcome.stage = result.stage == 1
                                        ? parking::CaptureStage::Second60s
                                        : parking::CaptureStage::First30s;
                    outcome.recognized = result.recognized;
                    outcome.plateNumber = result.plate_number;
                    outcome.confidence = result.confidence;
                    outcome.classification = result.classification;
                    hall_coordinator_ptr->onOcrOutcome(outcome);
                });

            util::logInfo(
                "hall capture OCR linking enabled: max_attempts=" +
                std::to_string(config.capture_ocr_max_attempts) +
                " overtime_sec=" + std::to_string(config.parking_overtime_sec));

            // sink 4: 입차 후 촬영 스케줄러 (T0+30s / T0+60s). 카메라 촬영 규약
            // (EVDA-138) 미확정이라 발행 토픽/페이로드는 draft 이며 기본 비활성.
            // Core(CaptureScheduler)는 MQTT 를 모르고, 슬롯->채널->ROI 매핑과
            // 발행 경계는 여기(main)에서 주입한다 (FireAlarmManager 와 같은 방식).
            if (config.capture_sched_enabled) {
                parking::CaptureSchedulerConfig sched_config;
                sched_config.responseTimeout = std::chrono::milliseconds(
                    config.capture_response_timeout_ms);
                sched_config.retryInterval = std::chrono::milliseconds(
                    config.capture_retry_interval_ms);
                sched_config.maxRetries = config.capture_max_retries;

                // 슬롯 -> 카메라 채널 -> ROI: AppConfig::iva_areas 에서 해석.
                parking::CaptureTargetResolver resolver =
                    [&config](const std::string& slotId)
                    -> std::optional<parking::CaptureTarget> {
                    for (const auto& area : config.iva_areas) {
                        if (area.slot_id != slotId) {
                            continue;
                        }
                        parking::CaptureTarget target;
                        target.cameraId = config.camera_id;
                        target.channelId = area.channel_id;
                        target.areaName = area.area_name;
                        target.roiX = area.roi_x;
                        target.roiY = area.roi_y;
                        target.roiWidth = area.roi_width;
                        target.roiHeight = area.roi_height;
                        return target;
                    }
                    return std::nullopt;
                };

                capture_scheduler =
                    std::make_unique<parking::CaptureScheduler>(
                        std::move(sched_config), std::move(resolver));

                const std::string capture_prefix = config.capture_topic_prefix;
                capture_runtime =
                    std::make_unique<parking::CaptureSchedulerRuntime>(
                        *capture_scheduler,
                        [&mqtt_bridge, &channels, &snapshot_storage,
                         hall_coordinator_ptr, capture_prefix](
                            const parking::CaptureRequest& request) {
                            const std::string topic =
                                capture_prefix + "/" + request.slotId;
                            const bool published = mqtt_bridge.publish(
                                topic, buildCapturePayload(request));

                            // 카메라가 MQTT 촬영 요청을 실제로 받아 응답하는지는
                            // 아직 확인되지 않았다(촬영 규약 §1·§10). 지원이
                            // 확인되지 않은 명령을 성공한 것처럼 쓰지 않으려고,
                            // 촬영본은 이미 실기기에서 검증된 경로 - RTSP 최신
                            // 프레임의 슬롯 ROI crop - 에서 가져온다. 규약이
                            // 확정되면(EVDA-138) 아래 획득 부분만 "응답 수신 ->
                            // HTTPS 다운로드"로 바꾸면 되고, 그 뒤 파이프라인은
                            // 그대로 쓴다.
                            std::shared_ptr<camera::CameraChannel> channel =
                                findChannel(channels,
                                            request.target.channelId);
                            if (!channel) {
                                util::logError(
                                    "capture channel is not configured: " +
                                    request.target.channelId);
                                return false;
                            }

                            const parking::CaptureStage stage =
                                parking::toStage(request.reason);
                            snapshot::NormalizedRoi roi{
                                request.target.roiX, request.target.roiY,
                                request.target.roiWidth,
                                request.target.roiHeight};
                            std::string path =
                                snapshot_storage.saveSlotRoiSnapshot(
                                    channel, request.slotId, roi,
                                    parking::toEnhancementType(stage));
                            if (path.empty()) {
                                // 프레임을 못 얻었다. 스케줄러의 재시도 예산에
                                // 맡긴다.
                                return false;
                            }

                            parking::CapturedImage image;
                            image.sessionId = request.sessionId;
                            image.slotId = request.slotId;
                            image.stage = stage;
                            image.originalPath = std::move(path);
                            hall_coordinator_ptr->onCaptureImage(image);
                            return published;
                        });

                parking::CaptureSchedulerRuntime* capture_runtime_ptr =
                    capture_runtime.get();
                parking_worker->addSink(
                    [capture_runtime_ptr](
                        const parking::ParkingTransitionResult& transition) {
                        capture_runtime_ptr->onTransition(transition);
                    });
                capture_runtime->start();

                util::logInfo(
                    "capture scheduler enabled (DRAFT protocol pending "
                    "EVDA-138): topic_prefix=" +
                    capture_prefix + " offsets=30s,60s retries=" +
                    std::to_string(config.capture_max_retries));
            }

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
    // 촬영 스케줄러는 sensor_link 정지 후(더 이상 전이 없음) 멈추고,
    // publisher 가 잡은 mqtt_bridge 보다 먼저 멈춘다.
    if (capture_runtime) capture_runtime->stop();
    mqtt_bridge.stop();
    bestshot_receiver.stop();
    // OCR 워커를 join 한 뒤에야 hall coordinator 로 들어오는 콜백이 끊긴다.
    ocr_worker.stop();
    rtsp_receiver.stop();
    if (http_server) http_server->stop();
    // 위반 타이머 스레드는 DB 를 닫기 전에 반드시 먼저 세운다.
    violation_timer.reset();
    database.close();

    util::logInfo("pi-server stopped");

    return 0;
}

#include "app/AppConfig.hpp"
#include "app/RuntimeShutdown.hpp"
#include "auth/AuthService.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/CameraSnapshotApiClient.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "device/SensorLinkManager.hpp"
#include "event/FireAlarmEvent.hpp"
#include "event/FireAlarmService.hpp"
#include "event/FireDeliveryCoordinator.hpp"
#include "event/SystemEventReporter.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEventBridge.hpp"
#include "notification/TelegramApiClient.hpp"
#include "notification/TelegramChannelNotifier.hpp"
#include "parking/CaptureRequest.hpp"
#include "parking/CaptureScheduler.hpp"
#include "parking/CaptureSchedulerRuntime.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking/HallCaptureCoordinator.hpp"
#include "parking/HallCaptureExecutor.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/PlateIlluminator.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "ocr/GeminiOcrClient.hpp"
#include "ocr/OcrWorker.hpp"
#include "sensor/FireSensorMessage.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"
#include "snapshot/SnapshotStorage.hpp"
#include "sensor/HallParkingService.hpp"
#include "settings/OverstayThresholdService.hpp"
#include "settings/ParkingRoiSettingsService.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"
#include "util/TimeUtil.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
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

    // Snapshot API 전용 모드에서도 slot/channel 매핑에 사용할 논리 채널은
    // 필요하다. RTSP URL이 없는 채널은 수신 스레드에 넘기지 않는다.
    if (config.camera_snapshot_api_enabled) {
        for (const auto& area : config.iva_areas) {
            const bool exists = std::any_of(
                channels.begin(), channels.end(),
                [&area](const auto& channel) {
                    return channel && channel->channel_id == area.channel_id;
                });
            if (exists) continue;
            auto channel = std::make_shared<camera::CameraChannel>();
            channel->camera_id = config.camera_id;
            channel->channel_id = area.channel_id;
            channels.push_back(std::move(channel));
        }
    }

    return channels;
}

const app::IvaAreaConfig* findArea(const app::AppConfig& config,
                                   const std::string& slot_id) {
    for (const auto& area : config.iva_areas)
        if (area.slot_id == slot_id) return &area;
    return nullptr;
}

event::FireSignal toFireSignal(const sensor::FireSensorMessage& message) {
    event::FireSignal signal;
    signal.sensorId = message.sensorId;
    signal.detected = message.state == sensor::FireSensorState::Detected;
    signal.occurredAt = message.occurredAt;
    signal.sourceSequence = message.sequence;
    signal.sourceProtocolVersion = message.protocolVersion;
    signal.sourceBootId = message.bootId;
    signal.sourceTransport = message.transport;
    signal.rawPayload = message.raw;
    return signal;
}

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channel_id) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channel_id) return channel;
    return nullptr;
}

std::chrono::steady_clock::time_point restoreMonotonicStart(
    const std::string& utc_value,
    const std::chrono::seconds fallback_elapsed) {
    const auto steady_now = std::chrono::steady_clock::now();
    if (utc_value.size() < 19)
        return steady_now - fallback_elapsed;

    std::string seconds = utc_value.substr(0, 19);
    if (seconds[10] == 'T') seconds[10] = ' ';
    std::tm utc{};
    std::istringstream input(seconds);
    input >> std::get_time(&utc, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) return steady_now - fallback_elapsed;
    const std::time_t timestamp = timegm(&utc);
    if (timestamp == static_cast<std::time_t>(-1))
        return steady_now - fallback_elapsed;

    auto started = std::chrono::system_clock::from_time_t(timestamp);
    if (utc_value.size() >= 23 && utc_value[19] == '.') {
        try {
            started += std::chrono::milliseconds(
                std::stoi(utc_value.substr(20, 3)));
        } catch (...) {
            return steady_now - fallback_elapsed;
        }
    }
    const auto wall_now = std::chrono::system_clock::now();
    if (started >= wall_now) return steady_now;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(wall_now - started);
    return steady_now - elapsed;
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
           << ",\"roi_revision\":" << request.target.roiRevision
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
    const std::string_view detail,
    const int overstay_threshold_seconds,
    const std::optional<parking::AppliedParkingRoi>& applied_roi) {
    const auto* area = findArea(config, std::string(slot_id));
    std::string external_type(event_type);
    if (event_type == "VIOLATION_TRIGGERED") external_type = "OVERTIME_VIOLATION";
    else if (event_type == "DEPARTURE") external_type = "SLOT_VACATED";
    else if (event_type == "TIMER_STARTED") external_type = "PLATE_RECOGNIZED";
    std::string alarm_kind = "NONE";
    if (external_type == "OVERTIME_VIOLATION") alarm_kind = "OVERSTAY";
    else if (external_type == "NON_EV_ALERT") alarm_kind = "NON_EV";
    const bool alarm = alarm_kind != "NONE";
    const bool overstay_alarm = alarm_kind == "OVERSTAY";
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
           << (overstay_alarm ? overstay_threshold_seconds : 0) << ','
           << "\"alarm_kind\":\"" << alarm_kind << "\","
           << "\"alarm\":\"" << alarm_kind << "\","
           << "\"alarm_state\":\"" << (alarm ? "OPEN" : "NONE") << "\","
           << "\"roi_configured\":" << (applied_roi ? "true" : "false")
           << ",\"roi\":{\"x\":"
           << (applied_roi ? applied_roi->value.x : 0.0) << ','
           << "\"y\":" << (applied_roi ? applied_roi->value.y : 0.0) << ','
           << "\"width\":"
           << (applied_roi ? applied_roi->value.width : 0.0) << ','
           << "\"height\":"
           << (applied_roi ? applied_roi->value.height : 0.0) << "},"
           << "\"roi_revision\":"
           << (applied_roi ? applied_roi->revision : 0) << ','
           << "\"session_images_url\":\"/api/v1/parking-sessions/"
           << session_id << "/images\","
           << "\"evidence_path\":\"" << util::jsonEscape(std::string(detail)) << "\","
           << "\"timestamp\":\"" << util::jsonEscape(std::string(timestamp)) << "\"}";
    return output.str();
}

// Qt는 alarm_kind만으로 UI 경고 종류를 판단하고, error_code로 상세 원인을
// 표시한다. DB EVENT_LOG의 상세 event_type은 바꾸지 않는다.
std::string buildQtSystemEvent(const event::SystemEvent& system_event) {
    const std::string timestamp = util::nowIsoString();
    const std::string event_type =
        system_event.recovered ? "SENSOR_RECOVERED" : "SENSOR_ERROR";
    std::ostringstream output;
    output << "{\"event_id\":\"system-"
           << util::jsonEscape(event::toString(system_event.code)) << '-'
           << util::jsonEscape(timestamp) << "\","
           << "\"event_type\":\"" << event_type << "\","
           << "\"alarm_kind\":\""
           << util::jsonEscape(event::systemAlarmKind(system_event)) << "\","
           << "\"alarm\":\""
           << util::jsonEscape(event::systemAlarmKind(system_event)) << "\","
           << "\"alarm_state\":\""
           << util::jsonEscape(event::systemAlarmState(system_event)) << "\","
           << "\"error_code\":\""
           << util::jsonEscape(event::toString(system_event.code)) << "\","
           << "\"source\":\""
           << util::jsonEscape(event::toString(system_event.source)) << "\","
           << "\"severity\":\""
           << util::jsonEscape(event::toString(system_event.severity)) << "\","
           << "\"slot_id\":\"" << util::jsonEscape(system_event.slot_id)
           << "\",\"scope\":\""
           << (system_event.slot_id.empty() ? "SYSTEM" : "SLOT") << "\","
           << "\"transport\":\"" << util::jsonEscape(system_event.transport)
           << "\",\"device\":\"" << util::jsonEscape(system_event.device)
           << "\",\"message\":\"" << util::jsonEscape(system_event.message)
           << "\",\"retry_count\":" << system_event.retry_count
           << ",\"suppressed_count\":" << system_event.suppressed_count
           << ",\"dropped_count\":" << system_event.dropped_count
           << ",\"recovered\":"
           << (system_event.recovered ? "true" : "false")
           << ",\"timestamp\":\"" << util::jsonEscape(timestamp) << "\"}";
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

    // FFmpeg 은 우리 Logger 를 거치지 않고 직접 stderr 로 쓴다("[h264 @ ...]
    // mmco: unref short failure" 류). 손상 프레임마다 나오므로 실제 로그를
    // 덮어버린다. LOG_FFMPEG=false 면 libavutil 쪽에서 아예 막는다.
    // 주의: OpenCV 가 자체 FFmpeg 을 정적 링크한 빌드라면 별도 libavutil 을
    // 쓰므로 이 설정이 안 먹을 수 있다.
    if (!util::logEnabled("FFMPEG")) {
        av_log_set_level(AV_LOG_QUIET);
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    app::AppConfig config = app::AppConfig::loadFromEnv();
    // Process admission과 media worker stop을 분리한다. 종료 요청이 들어와도
    // 이미 수락된 Hall/capture/evidence 작업이 끝날 때까지 RTSP frame은 유지한다.
    std::atomic<bool> rtsp_running{true};
    std::atomic<bool> bestshot_running{true};

    std::vector<parking::ParkingSlotConfig> parking_slot_configs;
    try {
        parking_slot_configs =
            parking::ParkingSlotConfigLoader::loadFromFile(
                config.parking_slot_config_path);
    } catch (const std::exception& error) {
        util::logError("Parking slot config load failed: " +
                       std::string(error.what()));
        return 1;
    }

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
        util::logError("No camera acquisition channel configured");
        util::logError("HINT: configure Camera Snapshot API or an RTSP URL");
        return 1;
    }

    std::vector<std::shared_ptr<camera::CameraChannel>> rtsp_channels;
    for (const auto& channel : channels) {
        if (channel && !channel->rtsp_url.empty())
            rtsp_channels.push_back(channel);
    }
    const bool rtsp_capture_enabled =
        !rtsp_channels.empty() &&
        (!config.camera_snapshot_api_enabled ||
         config.camera_snapshot_api_rtsp_fallback);
    if (!config.camera_snapshot_api_enabled && !rtsp_capture_enabled) {
        util::logError("Camera Snapshot API and RTSP capture are both disabled");
        return 1;
    }

    // 초기화 순서: DB -> 선택적 RTSP -> BestShot -> MQTT. Snapshot API 전용
    // 모드는 RTSP 최초 프레임을 기다리지 않는다.
    database::EventDatabase database;

    if (!database.open(config.db_path)) {
        return 1;
    }
    try {
#if defined(VEDA_RUNTIME_SQL_DIR)
        const std::filesystem::path runtime_sql_dir{VEDA_RUNTIME_SQL_DIR};
#else
        const std::filesystem::path runtime_sql_dir{"db"};
#endif
        database.initialize(runtime_sql_dir / "schema.sql",
                            runtime_sql_dir / "seed.sql");
    } catch (const std::exception& error) {
        util::logError("Runtime DB initialization/migration failed: " +
                       std::string(error.what()));
        database.close();
        return 1;
    }

    settings::OverstayThresholdService overstay_settings(
        database, config.parking_overstay_threshold_seconds);
    if (!overstay_settings.initialize()) {
        database.close();
        return 1;
    }
    settings::ParkingRoiSettingsService roi_settings(database,
                                                      config.iva_areas);
    if (!roi_settings.initialize()) {
        database.close();
        return 1;
    }
    std::unique_ptr<auth::AuthService> auth_service;
    try {
        auth::AuthConfig auth_config;
        auth_config.session_ttl_seconds = config.auth_session_ttl_seconds;
        auth_config.login_window_seconds = config.auth_login_window_seconds;
        auth_config.login_max_failures = config.auth_login_max_failures;
        auth_config.login_cooldown_seconds =
            config.auth_login_cooldown_seconds;
        auth_service = std::make_unique<auth::AuthService>(database,
                                                           auth_config);
    } catch (const std::exception& error) {
        util::logError("App authentication initialization failed: " +
                       std::string(error.what()));
        database.close();
        return 1;
    }

    // 센서/통신 스레드는 DB/MQTT I/O를 직접 기다리지 않고 bounded reporter queue에
    // 기록한다. MQTT bridge는 뒤에서 생성되므로 atomic pointer로 준비 상태만 공유한다.
    std::atomic<mqtt::MqttEventBridge*> system_event_mqtt_bridge{nullptr};
    event::SystemEventReporter system_event_reporter(
        [&database, &system_event_mqtt_bridge](
            const event::SystemEvent& system_event, const std::string& message) {
            const bool stored = database.insertSystemEvent(
                event::toString(system_event.code), system_event.slot_id, message);
            if (!stored) return false;

            auto* bridge = system_event_mqtt_bridge.load(
                std::memory_order_acquire);
            if (bridge != nullptr) {
                const std::string target = system_event.slot_id.empty()
                    ? "system" : system_event.slot_id;
                const std::string payload = buildQtSystemEvent(system_event);
                const bool event_published = bridge->publishQtEvent(
                    "parking/v1/events/" + target, payload, 1, false);
                const bool state_published = bridge->publishQtEvent(
                    "parking/v1/state/" + target, payload, 1, true);
                if (!event_published || !state_published)
                    util::logWarn("Qt sensor alarm MQTT publish failed: code=" +
                                  event::toString(system_event.code));
            }
            return true;
        });
    event::SystemEventReporter* system_event_sink = nullptr;

    std::unique_ptr<notification::TelegramApiClient> telegram_api_client;
    std::unique_ptr<notification::TelegramChannelNotifier> telegram_notifier;
    notification::TelegramChannelNotifier* telegram_notifier_ptr{nullptr};

    if (config.telegram_enabled) {
        const bool telegram_enabled =
            !config.telegram_bot_token.empty() &&
            !config.telegram_channel.empty();

        telegram_api_client = std::make_unique<notification::TelegramApiClient>(
            notification::TelegramApiClient::Config{
                .enabled = telegram_enabled,
                .bot_token = config.telegram_bot_token,
                .channel = config.telegram_channel,
                .connect_timeout_ms = static_cast<long>(
                    std::max(1, config.telegram_connect_timeout_ms)),
                .request_timeout_ms = static_cast<long>(
                    std::max(1, config.telegram_request_timeout_ms))
            });

        const auto telegram_notifier_config =
            notification::TelegramChannelNotifier::Config{
                .enabled = telegram_enabled,
                .queue_capacity = static_cast<std::size_t>(
                    std::max(1, config.telegram_queue_capacity)),
                .retry_count = std::max(0, config.telegram_retry_count),
                .retry_delay_ms = std::max(1, config.telegram_retry_delay_ms)
            };

        telegram_notifier = std::make_unique<notification::TelegramChannelNotifier>(
            telegram_notifier_config, *telegram_api_client);

        if (telegram_notifier->start()) {
            telegram_notifier_ptr = telegram_notifier.get();
        } else {
            util::logWarn(
                "Telegram notifier disabled: MQTT publish path remains active");
        }
    }

    std::unique_ptr<http::ParkingHttpServer> http_server;

    camera::RtspStreamReceiver rtsp_receiver(
        rtsp_channels,
        config.preview_width,
        config.preview_height,
        config.rtsp_retry_delay_ms,
        config.empty_frame_delay_ms,
        config.max_consecutive_read_failures,
        rtsp_running
    );

    snapshot::SnapshotStorage snapshot_storage(
        config.snapshot_dir,
        config.snapshot_frame_wait_ms,
        rtsp_running
    );

    parking::ParkingTriggerCoordinator trigger_coordinator(
        database,
        config.bestshot_correlation_window_ms
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
                0.0, 0.0, 0.0, 0.0,
                area->snapshot_api_channel};
        };
        capture_scheduler = std::make_unique<parking::CaptureScheduler>(
            std::move(scheduler_config), std::move(resolver));
    }

    std::unique_ptr<parking_timer::EventManager> timer_events;
    std::unique_ptr<parking_timer::ParkingSlotManager> parking_timer;
    parking::EvidenceCaptureWorker* evidence_worker_for_timer = nullptr;
    if (config.parking_timer_enabled) {
        timer_events = std::make_unique<parking_timer::EventManager>();
        parking_timer = std::make_unique<parking_timer::ParkingSlotManager>(
            database, *timer_events,
            std::chrono::seconds(overstay_settings.thresholdSeconds()),
            [&database, &evidence_worker_for_timer](
                std::int64_t session_id, const std::string& slot_id,
                const std::string&) {
                // 새 증거 worker가 T0 기준 이미지를 이미 저장했다면 같은 파일을
                // 재사용한다. 이전 DB/촬영 실패 세션만 기존 Snapshot으로 보완한다.
                if (const auto existing = database.findEvidenceImagePath(
                        session_id, "OVERSTAY_EVIDENCE")) {
                    return *existing;
                }
                if (evidence_worker_for_timer != nullptr) {
                    evidence_worker_for_timer->expediteOverstay(session_id);
                }
                util::logWarn("Timer reached before overstay evidence was ready: "
                              "session=" + std::to_string(session_id) +
                              " slot=" + slot_id);
                return std::string{};
            });
        util::logInfo("parking timer enabled: timeout=" +
                      std::to_string(overstay_settings.thresholdSeconds()) + "s");
    }
    auto* const parking_timer_callback_target = parking_timer.get();
    auto* const timer_events_callback_target = timer_events.get();

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
        [parking_timer_callback_target](
            const ocr::OcrWorker::RecognitionResult& result) {
            if (parking_timer_callback_target) {
                parking_timer_callback_target->handleRecognizedSession(
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
                    parking_timer::utcNow(), image.roi,
                    image.roiRevision);
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
    hall_capture_ports.handleRecognizedSession =
        [parking_timer_callback_target](const std::int64_t session_id,
                                        const std::string& slot_id,
                                        const std::string& plate_number) {
            if (parking_timer_callback_target) {
                parking_timer_callback_target->handleRecognizedSession(
                    session_id, slot_id, plate_number);
            }
        };
    // 링크는 촬영 runtime보다 뒤에서 열리므로 포인터만 미리 잡아두고 배선한다.
    std::atomic<device::SensorLinkManager*> sensor_link_for_led{nullptr};
    std::unique_ptr<parking::PlateIlluminator> plate_illuminator;
    if (config.plate_led_enabled) {
        parking::PlateIlluminatorConfig led_config;
        led_config.enabled = true;
        led_config.nightStartHour = config.plate_led_night_start_hour;
        led_config.nightEndHour = config.plate_led_night_end_hour;
        led_config.settleDelay =
            std::chrono::milliseconds(config.plate_led_settle_ms);
        plate_illuminator = std::make_unique<parking::PlateIlluminator>(
            led_config,
            [&sensor_link_for_led](const std::string& payload,
                                   const std::uint32_t sequence) {
                auto* const link = sensor_link_for_led.load(
                    std::memory_order_acquire);
                return link != nullptr &&
                       link->sendAlertCommand(payload, sequence);
            });
        util::logInfo("plate LED enabled: night=" +
                      std::to_string(config.plate_led_night_start_hour) +
                      "h-" +
                      std::to_string(config.plate_led_night_end_hour) +
                      "h settle=" +
                      std::to_string(config.plate_led_settle_ms) + "ms");
        // 조명은 Pi가 직접 노출을 거는 예약 촬영 경로에만 붙는다. 둘 중 하나라도
        // 꺼져 있으면 명령이 한 건도 나가지 않으므로 조용히 넘어가지 않는다.
        if (!config.capture_sched_enabled || !config.hall_capture_ocr_enabled) {
            util::logWarn(
                "plate LED will never fire: PLATE_LED_ENABLED=true but "
                "CAPTURE_SCHED_ENABLED=" +
                std::string(config.capture_sched_enabled ? "true" : "false") +
                " HALL_CAPTURE_OCR_ENABLED=" +
                std::string(config.hall_capture_ocr_enabled ? "true"
                                                            : "false") +
                "; both must be true for scheduled captures to drive the LED");
        }
    }

    auto hall_ocr_coordinator =
        std::make_unique<parking::HallCaptureCoordinator>(
            std::move(hall_capture_ports), config.capture_ocr_max_attempts);
    auto* const hall_ocr_callback_target = hall_ocr_coordinator.get();
    auto* const plate_illuminator_callback_target = plate_illuminator.get();
    ocr_worker.setHallCaptureCallback(
        [hall_ocr_callback_target, plate_illuminator_callback_target](
            const ocr::HallCaptureResult& result) {
            if (!hall_ocr_callback_target) return;
            if (plate_illuminator_callback_target) {
                // 번호판을 읽지 못한 세션만 남은 예약 촬영을 LED 보정으로 다시
                // 시도한다. 요청 실패나 큐 거부는 조명으로 풀리지 않는다.
                if (result.plate_unreadable)
                    plate_illuminator_callback_target->markPlateUnreadable(
                        result.session_id);
                // 번호판을 확보했으면 남은 촬영은 증거 저장용이라 조명이 없다.
                else if (result.recognized)
                    plate_illuminator_callback_target->markResolved(
                        result.session_id);
            }
            hall_ocr_callback_target->onOcrOutcome({
                result.session_id,
                result.stage == 1 ? parking::CaptureStage::Second60s
                                  : parking::CaptureStage::First30s,
                result.recognized,
                result.plate_number,
                result.confidence,
                result.classification});
        });

    std::unique_ptr<camera::CameraSnapshotApiClient> camera_snapshot_api;
    if (config.camera_snapshot_api_enabled) {
        camera::CameraSnapshotApiConfig api_config;
        api_config.openApiBase = config.camera_open_api_base;
        api_config.imageBase = config.camera_image_base;
        api_config.username = config.camera_api_username;
        api_config.password = config.camera_api_password;
        api_config.imageServerPort = config.camera_image_server_port;
        api_config.connectTimeoutMs =
            config.camera_snapshot_connect_timeout_ms;
        api_config.requestTimeoutMs =
            config.camera_snapshot_request_timeout_ms;
        api_config.jpegTimeoutMs = config.camera_snapshot_jpeg_timeout_ms;
        api_config.maxRetries = config.camera_snapshot_max_retries;
        api_config.retryDelayMs = config.camera_snapshot_retry_delay_ms;
        camera_snapshot_api =
            std::make_unique<camera::CameraSnapshotApiClient>(
                std::move(api_config));
        if (!camera_snapshot_api->initialize()) {
            util::logError(
                "camera snapshot API initialization failed; scheduled "
                "captures will retry: " + camera_snapshot_api->lastError());
        }
    }

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
                    },
                    camera_snapshot_api.get(),
                    config.camera_snapshot_api_rtsp_fallback,
                    plate_illuminator.get(),
                    [&roi_settings](const std::string& slot_id) {
                        return roi_settings.resolveForUse(slot_id);
                    });
            auto* const hall_capture_executor_target =
                hall_capture_executor.get();
            capture_runtime =
                std::make_unique<parking::CaptureSchedulerRuntime>(
                    *capture_scheduler,
                    [hall_capture_executor_target](
                        const parking::CaptureRequest& request) {
                        return hall_capture_executor_target &&
                               hall_capture_executor_target->execute(request);
                    });
        } else {
            // 촬영을 카메라가 수행하므로 Pi는 노출 시점을 모른다. 점등해도
            // 노출과 어긋나기 때문에 이 경로에서는 조명을 쓰지 않는다.
            capture_runtime =
                std::make_unique<parking::CaptureSchedulerRuntime>(
                    *capture_scheduler,
                    [&capture_mqtt_bridge, topic_prefix, &roi_settings](
                        const parking::CaptureRequest& request) {
                        const auto roi = roi_settings.resolveForUse(
                            request.slotId);
                        if (!roi) return false;
                        parking::CaptureRequest applied = request;
                        applied.target.roiX = roi->value.x;
                        applied.target.roiY = roi->value.y;
                        applied.target.roiWidth = roi->value.width;
                        applied.target.roiHeight = roi->value.height;
                        applied.target.roiRevision = roi->revision;
                        return capture_mqtt_bridge != nullptr &&
                            capture_mqtt_bridge->publishApplicationEvent(
                                topic_prefix + "/" + applied.slotId,
                                buildCaptureRequestPayload(applied), 1,
                                false);
                    });
        }
    }

    parking::EvidenceCaptureWorker::Config evidence_config;
    evidence_config.overstayDelay = std::chrono::seconds(
        overstay_settings.thresholdSeconds());
    auto* const camera_snapshot_callback_target = camera_snapshot_api.get();
    auto evidence_worker = std::make_unique<parking::EvidenceCaptureWorker>(
        snapshot_storage, database, evidence_config,
        [&ocr_worker, timer_events_callback_target, &config](
            const parking::EvidenceCaptureResult& result) {
            if (!result.stored) return;
            if (result.reason == parking::EvidenceReason::OccupancyStart) {
                // 30/60초 홀 OCR이 활성화되면 차량이 자리를 잡은 뒤의 ROI를
                // 사용한다. 스케줄러가 꺼진 환경에서는 기존 시작 증거 OCR로
                // fallback하여 번호판 인식 기능이 사라지지 않게 한다.
                if (!(config.hall_capture_ocr_enabled &&
                      config.capture_sched_enabled)) {
                    ocr_worker.enqueue(static_cast<int>(result.sessionId),
                                       result.slotId, result.imagePath,
                                       result.enhancedImagePath);
                }
                if (timer_events_callback_target) {
                    (void)timer_events_callback_target->publish(
                        "OCCUPANCY_START_EVIDENCE_STORED", result.slotId, "",
                        parking_timer::utcNow(), result.imagePath,
                        result.sessionId);
                }
            } else if (timer_events_callback_target) {
                (void)timer_events_callback_target->publish(
                    "OVERSTAY_EVIDENCE_STORED", result.slotId, "",
                    parking_timer::utcNow(), result.imagePath,
                    result.sessionId);
            }
        },
        camera_snapshot_callback_target
            ? parking::EvidenceCaptureWorker::Capture{
                [camera_snapshot_callback_target, &snapshot_storage, &config](
                    const parking::EvidenceCaptureRequest& request,
                    const parking::EvidenceReason reason) {
                    camera::CameraGeneratedImages generated;
                    if (camera_snapshot_callback_target->generate(
                            request.snapshotApiChannel, generated)) {
                        auto paths = snapshot_storage.saveCameraApiHallCapture(
                            request.channel ? request.channel->channel_id : "",
                            request.sessionId, request.slotId,
                            parking::toString(reason), request.roi,
                            generated.originalJpeg, generated.enhancedJpeg);
                        if (!paths.originalPath.empty() &&
                            !paths.enhancedPath.empty()) {
                            util::logLine(
                                "CAMERA_SNAPSHOT_API",
                                "evidence stored session=" +
                                    std::to_string(request.sessionId) +
                                    " slot=" + request.slotId + " reason=" +
                                    parking::toString(reason) + " run_id=" +
                                    generated.runId);
                        }
                        return paths;
                    }
                    util::logError(
                        "camera snapshot API evidence failed session=" +
                        std::to_string(request.sessionId) + " slot=" +
                        request.slotId + " reason=" +
                        parking::toString(reason) + " error=" +
                        camera_snapshot_callback_target->lastError());
                    if (!config.camera_snapshot_api_rtsp_fallback) {
                        return snapshot::StoredImagePair{};
                    }
                    util::logWarn(
                        "falling back to RTSP evidence capture session=" +
                        std::to_string(request.sessionId) + " slot=" +
                        request.slotId);
                    return snapshot::StoredImagePair{
                        snapshot_storage.saveEvidenceSnapshot(
                            request.channel, request.sessionId, request.slotId,
                            parking::toString(reason), request.roi),
                        {}};
                }}
            : parking::EvidenceCaptureWorker::Capture{},
        [&roi_settings](const std::string& slot_id) {
            return roi_settings.resolveForUse(slot_id);
        });
    evidence_worker_for_timer = evidence_worker.get();

    auto* const evidence_policy_target = evidence_worker.get();
    auto* const timer_policy_target = parking_timer.get();
    overstay_settings.setApplyCallback(
        [evidence_policy_target, timer_policy_target](
            const std::chrono::milliseconds delay) {
            const auto evidence_count = evidence_policy_target
                ? evidence_policy_target->updateOverstayDelay(delay) : 0U;
            const auto timer_count = timer_policy_target
                ? timer_policy_target->updateParkingTimeout(delay) : 0U;
            util::logInfo("Overstay runtime policy applied: timers=" +
                          std::to_string(timer_count) + " evidence_jobs=" +
                          std::to_string(evidence_count));
        });

    bestshot::BestShotReceiver bestshot_receiver(
        channels, trigger_coordinator, ocr_worker,
        bestshot_running);

    const auto sensor_link_mode =
        device::SensorLinkManager::parseMode(config.sensor_link_mode);
    std::shared_ptr<sensor::HallParkingService> hall_service;

    mqtt::MqttEventBridge mqtt_bridge(
        config, channels, database, snapshot_storage,
        ocr_worker, parking_slot_configs, {}, {}, {},
        mqtt::makeMosquittoTransport(), telegram_notifier_ptr);
    if (!mqtt_bridge.bindParkingRoiResolver(
            [&roi_settings](const std::string& slot_id) {
                return roi_settings.resolveForUse(slot_id);
            })) {
        util::logError("MQTT runtime ROI resolver binding failed");
        return 1;
    }
    if (camera_snapshot_api) {
        auto* const camera_snapshot_api_target = camera_snapshot_api.get();
        if (!mqtt_bridge.bindCameraSnapshotGenerator(
                [camera_snapshot_api_target](
                    const int channel,
                    camera::CameraGeneratedImages& images) {
                    return camera_snapshot_api_target != nullptr &&
                        camera_snapshot_api_target->generate(channel, images);
                })) {
            util::logError("MQTT camera snapshot generator binding failed");
            return 1;
        }
    }
    capture_mqtt_bridge = &mqtt_bridge;

    std::shared_ptr<event::FireAlarmService> fire_alarm_service;
    std::shared_ptr<event::FireDeliveryCoordinator>
        fire_delivery_coordinator;
    auto fire_coordinator_slot = std::make_shared<
        std::weak_ptr<event::FireDeliveryCoordinator>>();
    if (config.fire_alarm_enabled) {
        const auto parsed = event::parseFireChannelBindingsStrict(
            config.fire_sensor_channel_map, config.fire_topic_prefix);
        if (!parsed.valid()) {
            util::logError("Fire mapping rejected: " + parsed.error);
            return 1;
        }

        event::FireAlarmService::Config fire_service_config;
        fire_service_config.cameraId = config.camera_id;
        fire_service_config.lifecycleTopicPrefix = "parking/v1/events";
        fire_alarm_service = std::make_shared<event::FireAlarmService>(
            database, std::move(fire_service_config), parsed.bindings,
            [fire_coordinator_slot](
                const event::FireCommandResult& result) {
                if (result.status ==
                        event::FireCommandStatus::DurablyCommitted ||
                    result.status == event::FireCommandStatus::Idempotent) {
                    if (const auto coordinator =
                            fire_coordinator_slot->lock()) {
                        coordinator->notifyOutboxChanged(result.channelId);
                    }
                } else if (result.status ==
                           event::FireCommandStatus::Failed) {
                    util::logError(
                        "Fire domain command failed after admission: " +
                        result.error);
                }
            },
            [fire_coordinator_slot](
                const std::string& channel_id,
                const std::uint64_t revision) {
                const auto coordinator = fire_coordinator_slot->lock();
                return coordinator && coordinator->isRevisionSynchronized(
                    channel_id, revision);
            },
            [fire_coordinator_slot](const std::string& channel_id) {
                const auto coordinator = fire_coordinator_slot->lock();
                return coordinator &&
                    coordinator->beginDomainMutation(channel_id);
            },
            [fire_coordinator_slot](const std::string& channel_id) {
                if (const auto coordinator = fire_coordinator_slot->lock())
                    coordinator->completeDomainMutation(channel_id);
            });
        if (!fire_alarm_service->initialize()) {
            util::logError(
                "Fire durable state initialization failed: " +
                fire_alarm_service->configurationError());
            return 1;
        }

        event::FireDeliveryCoordinator::Config delivery_config;
        for (const auto& binding : parsed.bindings)
            delivery_config.channelIds.push_back(binding.channelId);
        fire_delivery_coordinator =
            std::make_shared<event::FireDeliveryCoordinator>(
                database, std::move(delivery_config),
                [&mqtt_bridge](
                    const event::FireOutboxRecord& delivery,
                    mqtt::MqttPublishCorrelation correlation,
                    const mqtt::MqttConnectionEpoch expected_epoch) {
                    return mqtt_bridge.publishTrackedFire(
                        delivery.topic, delivery.payloadJson,
                        delivery.retain, std::move(correlation),
                        expected_epoch);
                },
                [&mqtt_bridge](const mqtt::MqttConnectionEpoch epoch) {
                    if (!mqtt_bridge.abortActiveEpoch(epoch)) {
                        util::logWarn(
                            "MQTT Fire epoch abort raced with reconnect: " +
                            std::to_string(epoch));
                    }
                });
        *fire_coordinator_slot = fire_delivery_coordinator;
        util::logInfo(
            "fire alarm durable delivery configured: topic_prefix=" +
            config.fire_topic_prefix + " bindings=" +
            std::to_string(fire_alarm_service->bindingCount()));
    }

    const sensor::SensorProtocolParser fire_line_parser;
    std::unique_ptr<device::SensorLinkManager> sensor_link;
    bool fire_alarm_service_started{};
    bool fire_delivery_coordinator_started{};

    // 모든 partial-start 실패와 정상 종료가 같은 경로를 사용한다. 이 guard는
    // 아래 owner보다 나중에 선언되어 owner destructor보다 먼저 실행된다.
    app::RuntimeShutdown runtime_shutdown({
        [&] {
            g_running.store(false, std::memory_order_release);
            if (http_server) http_server->closeIngress();
            mqtt_bridge.closeIngress();
            if (http_server && !http_server->stop())
                throw std::runtime_error("HTTP callback drain failed");
            overstay_settings.setApplyCallback({});
        },
        [&] {
            if (!mqtt_bridge.quiesceIngress())
                throw std::runtime_error("MQTT application drain failed");
        },
        [&] {
            sensor_link_for_led.store(nullptr, std::memory_order_release);
            if (sensor_link) sensor_link->stop();
        },
        [&] {
            if (hall_service &&
                !hall_service->stop(std::chrono::seconds(30))) {
                throw std::runtime_error(
                    "Hall command admission drain failed");
            }
            hall_service.reset();
        },
        [&] {
            if (capture_runtime) {
                capture_runtime->stop();
                capture_runtime.reset();
            }
        },
        [&] {
            bestshot_running.store(false, std::memory_order_release);
            if (config.bestshot_enabled) bestshot_receiver.stop();
        },
        [&] { evidence_worker->stop(); },
        [&] { ocr_worker.stop(); },
        [&] {
            parking_timer.reset();
            timer_events.reset();
        },
        [&] {
            rtsp_running.store(false, std::memory_order_release);
            rtsp_receiver.stop();
        },
        [&] {
            if (fire_alarm_service_started && fire_alarm_service) {
                fire_alarm_service->closeIngress();
                if (!fire_alarm_service->stop()) {
                    throw std::runtime_error(
                        "Fire command commit drain failed");
                }
                fire_alarm_service_started = false;
            }
            if (fire_delivery_coordinator_started &&
                fire_delivery_coordinator) {
                if (!fire_delivery_coordinator->drainDeliveries(
                        std::chrono::seconds(5))) {
                    util::logWarn(
                        "Fire delivery deadline expired; durable outbox "
                        "will resume after restart");
                }
                if (!mqtt_bridge.quiesceTransportObservers()) {
                    throw std::runtime_error(
                        "MQTT transport observer drain failed");
                }
                if (!fire_delivery_coordinator->stop()) {
                    throw std::runtime_error(
                        "Fire delivery coordinator join failed");
                }
                fire_delivery_coordinator_started = false;
            }
        },
        [&] {
            if (!system_event_reporter.stop())
                throw std::runtime_error("system event reporter join failed");
            if (telegram_notifier) {
                telegram_notifier->stop();
                telegram_notifier.reset();
            }
        },
        [&] {
            capture_mqtt_bridge = nullptr;
            system_event_mqtt_bridge.store(nullptr,
                                           std::memory_order_release);
            if (!mqtt_bridge.stop())
                throw std::runtime_error("MQTT transport join failed");
        },
        [&] {
            fire_delivery_coordinator.reset();
            fire_alarm_service.reset();
            sensor_link.reset();
            http_server.reset();
        },
        [&] { database.close(); }});

    const auto shutdown_and_return = [&](const int result) {
        g_running.store(false);
        if (!runtime_shutdown.shutdown()) {
            util::logError(
                "runtime lifetime barrier failed; terminating without "
                "destroying callback targets");
            std::_Exit(EXIT_FAILURE);
        }
        return result;
    };

    if (fire_alarm_service && !fire_alarm_service->start()) {
        util::logError("Fire domain command worker could not be started");
        return shutdown_and_return(1);
    }
    fire_alarm_service_started = fire_alarm_service != nullptr;
    if (fire_delivery_coordinator &&
        !fire_delivery_coordinator->start()) {
        util::logError("Fire delivery coordinator could not be started");
        return shutdown_and_return(1);
    }
    fire_delivery_coordinator_started =
        fire_delivery_coordinator != nullptr;

    if (parking_timer && !parking_timer->start()) {
        util::logError("Parking timer worker could not be started");
        return shutdown_and_return(1);
    }

    system_event_sink =
        system_event_reporter.start() ? &system_event_reporter : nullptr;
    if (system_event_sink == nullptr)
        util::logError("System event reporter disabled after start failure");

    auto* const capture_transition_target = capture_runtime.get();
    if (parking_timer && (config.hall_mqtt_input_enabled ||
                          sensor_link_mode != device::SensorLinkMode::Disabled ||
                          config.parking_occupancy_source == "CAMERA_IVA" ||
                          config.parking_occupancy_source == "HALL" ||
                          config.parking_occupancy_source == "HYBRID_OR")) {
        hall_service = std::make_shared<sensor::HallParkingService>(
            parking_slot_configs, config, channels, database,
            [&ocr_worker](const int session_id) {
                ocr_worker.cancelSession(session_id);
            },
            *parking_timer, *timer_events, *evidence_worker,
            system_event_sink,
            [capture_transition_target, hall_ocr_callback_target,
             plate_illuminator_callback_target](
                const parking::ParkingTransitionResult& transition) {
                if (hall_ocr_callback_target)
                    hall_ocr_callback_target->onTransition(transition);
                if (capture_transition_target)
                    capture_transition_target->onTransition(transition);
                if (plate_illuminator_callback_target &&
                    transition.code ==
                        parking::ParkingTransitionCode::SessionCompleted) {
                    plate_illuminator_callback_target->forget(
                        transition.sessionId);
                }
            });
    }

    const bool hall_mqtt_required = config.hall_mqtt_input_enabled &&
        (config.parking_occupancy_source == "HALL" ||
         config.parking_occupancy_source == "HYBRID_OR");
    const bool iva_handler_required =
        config.parking_occupancy_source == "CAMERA_IVA" ||
        config.parking_occupancy_source == "HALL" ||
        config.parking_occupancy_source == "HYBRID_OR";
    if ((hall_mqtt_required || iva_handler_required) && !hall_service) {
        util::logError(
            "parking occupancy callback target could not be constructed");
        return shutdown_and_return(1);
    }

    if (hall_service) {
        std::weak_ptr<sensor::HallParkingService> weak_hall = hall_service;
        const bool sensor_bound = mqtt_bridge.bindSensorMessageHandler(
            [weak_hall](const std::string& line) {
                if (const auto target = weak_hall.lock())
                    target->handleLine(line);
            });
        const bool iva_bound = mqtt_bridge.bindIvaOccupancyHandler(
            [weak_hall](const event::IvaOccupancySignal& signal) {
                const auto target = weak_hall.lock();
                return target && target->handleCameraIvaSignal(signal);
            });
        if (!sensor_bound || !iva_bound) {
            util::logError("Hall MQTT callback binding failed while stopped");
            return shutdown_and_return(1);
        }
    }
    if (fire_alarm_service && fire_delivery_coordinator) {
        std::weak_ptr<event::FireAlarmService> weak_fire =
            fire_alarm_service;
        std::weak_ptr<event::FireDeliveryCoordinator> weak_delivery =
            fire_delivery_coordinator;
        const bool facts_bound = mqtt_bridge.bindTransportFactHandler(
            [weak_delivery](const mqtt::MqttTransportFact& fact) {
                const auto target = weak_delivery.lock();
                return target && target->enqueueTransportFact(fact);
            });
        const bool egress_bound = mqtt_bridge.bindRegularEgressAdmission(
            [weak_delivery]() -> mqtt::RegularEgressPermit {
                const auto target = weak_delivery.lock();
                if (!target) return {};
                const auto grant = target->beginRegularEgress();
                if (!grant) return {};
                return mqtt::RegularEgressPermit(
                    grant->epoch, grant->generation,
                    [target, grant = *grant] {
                        return target->isRegularEgressGrantCurrent(grant);
                    },
                    [target] { target->completeRegularEgress(); });
            });
        const bool ack_bound = mqtt_bridge.bindFireAckHandler(
            [weak_fire, weak_delivery](const std::string& channel_id,
                                       const std::string& alarm_id) {
                const auto service = weak_fire.lock();
                const auto delivery = weak_delivery.lock();
                if (!service || !delivery) return false;
                const auto result = service->submitAcknowledge(
                    channel_id, alarm_id);
                return result.status == event::FireCommandStatus::Queued;
            });
        const bool clear_bound = mqtt_bridge.bindFireClearHandler(
            [weak_fire](const std::string& channel_id) {
                const auto service = weak_fire.lock();
                if (!service) return false;
                const auto result = service->submitManualClear(channel_id);
                return result.status == event::FireCommandStatus::Queued;
            });
        if (!facts_bound || !egress_bound || !ack_bound || !clear_bound) {
            util::logError("Fire ACK callback binding failed while stopped");
            return shutdown_and_return(1);
        }
    }

    if (!evidence_worker->start()) return shutdown_and_return(1);
    util::logInfo("parking evidence worker enabled: overstay_delay=" +
                  std::to_string(overstay_settings.thresholdSeconds()) + "s");

    if (rtsp_capture_enabled) rtsp_receiver.start();
    if (rtsp_capture_enabled &&
        !rtsp_receiver.waitForInitialFrames(config.initial_frame_timeout_sec)) {
        return shutdown_and_return(1);
    }
    if (!rtsp_capture_enabled) {
        util::logInfo(
            "RTSP capture disabled: Camera Snapshot API is the image source");
    }

    // 재시작 전에 생성된 ACTIVE 세션은 메모리 evidence queue가 사라졌으므로
    // 원래 entry_time(T0)을 steady_clock으로 환산해 timer보다 먼저 복원한다.
    std::size_t restored_evidence{};
    std::size_t failed_evidence_restore{};
    for (const auto& record : database.listLogs()) {
        if (record.departed_at.has_value() ||
            (record.status != "PARKED" && record.status != "ACTIVE")) {
            continue;
        }
        const auto* area = findArea(config, record.slot_id);
        auto channel = area ? findChannel(channels, area->channel_id) : nullptr;
        if (area == nullptr || !channel) {
            ++failed_evidence_restore;
            util::logError("evidence restore mapping missing: session=" +
                           std::to_string(record.id) + " slot=" +
                           record.slot_id);
            continue;
        }
        const auto fallback_elapsed = std::chrono::seconds(
            overstay_settings.thresholdSeconds());
        if (evidence_worker->restoreSession({
                record.id, record.slot_id, std::move(channel),
                {},
                restoreMonotonicStart(record.parked_at, fallback_elapsed),
                area->snapshot_api_channel})) {
            ++restored_evidence;
        } else {
            ++failed_evidence_restore;
        }
    }
    util::logInfo("parking evidence restored active sessions=" +
                  std::to_string(restored_evidence) + " failed=" +
                  std::to_string(failed_evidence_restore));

    ocr_worker.start();
    if (config.bestshot_enabled) {
        bestshot_receiver.start();
        util::logInfo("BestShot receiver enabled");
    } else {
        util::logInfo("BestShot receiver disabled (BESTSHOT_ENABLED=false)");
    }
    if (capture_runtime) {
        capture_runtime->start();
        util::logInfo(
            "capture scheduler enabled (DRAFT protocol pending EVDA-138): "
            "topic_prefix=" + config.capture_topic_prefix +
            " offsets=" + config.capture_offsets_sec + "s retries=" +
            std::to_string(config.capture_max_retries));
    }

    if (timer_events) {
        timer_events->setPublisher(
            [&mqtt_bridge, &config, &database, &overstay_settings,
             &roi_settings](
                const std::string_view event_type, const std::int64_t session_id,
                const std::string_view slot_id, const std::string_view plate,
                const std::string_view timestamp, const std::string_view detail) {
                if (slot_id.empty() || session_id < 0) return true;
                const auto applied_roi = roi_settings.resolveForUse(
                    std::string(slot_id));
                if (!applied_roi) {
                    // ROI 미설정은 Qt 상태 이벤트 발행 실패가 아니다. 실패로
                    // 반환하면 이미 커밋된 점유 effect가 계속 재시도되어 동일
                    // 촬영 예약과 로그가 반복된다. crop/OCR 경로는 별도로 ROI를
                    // 검증하므로 여기서는 좌표 없는 상태 이벤트를 한 번 발행한다.
                    util::logWarn(
                        "Qt parking event published without ROI: slot=" +
                        std::string(slot_id) +
                        " roi_configured=false");
                }
                const std::string payload = buildQtParkingEvent(
                    config, database, event_type, session_id, slot_id, plate,
                    timestamp, detail,
                    overstay_settings.thresholdSeconds(), applied_roi);
                const std::string event_topic =
                    "parking/v1/events/" + std::string(slot_id);
                const std::string state_topic =
                    "parking/v1/state/" + std::string(slot_id);
                const bool event_published = mqtt_bridge.publishQtEvent(
                    event_topic, payload, 1, false);
                const bool state_published =
                    event_type == "EARLY_DEPARTURE_IMAGES_DELETED" ||
                    mqtt_bridge.publishQtEvent(
                        state_topic, payload, 1, true);
                return event_published && state_published;
            });
    }
    if (parking_timer) {
        const auto restored = parking_timer->restoreActiveSessions();
        util::logInfo("parking timer restored active sessions=" +
                      std::to_string(restored));
    }

    if (!mqtt_bridge.start()) return shutdown_and_return(1);
    system_event_mqtt_bridge.store(&mqtt_bridge, std::memory_order_release);

    // HTTP는 runtime 복원과 MQTT readiness 뒤에 열고, 물리 센서 ingress는
    // 모든 callback target이 준비된 가장 마지막 단계에서 연다.
    if (config.http_api_enabled) {
        http::ServerConfig http_config;
        http_config.listen_address = config.http_listen_address;
        http_config.port = config.http_port;
        http_config.tls_certificate_path = config.http_tls_certificate_path;
        http_config.tls_private_key_path = config.http_tls_private_key_path;
        http_config.data_root = config.http_data_root;
        http_config.max_image_bytes = static_cast<std::size_t>(
            std::max(1, config.http_max_image_mb)) * 1024U * 1024U;
        http_config.require_tls = config.http_require_tls;
        http_server = std::make_unique<http::ParkingHttpServer>(
            database, *auth_service, http_config, &overstay_settings,
            &roi_settings);
        if (!http_server->start()) return shutdown_and_return(1);
    }

    // 화재와 홀센서는 같은 STM32 UART 링크를 공유하므로 하나만 열고,
    // 수신 라인을 접두사(FIRE:/SENSOR:)로 분리한다.
    if ((hall_service || fire_alarm_service) &&
        sensor_link_mode != device::SensorLinkMode::Disabled) {
        device::SensorLinkManager::Config sensor_config;
        sensor_config.mode = sensor_link_mode;
        sensor_config.uart.device_path = config.sensor_uart_device;
        sensor_config.uart.baud_rate = config.sensor_uart_baud_rate;
        sensor_config.uart.read_timeout_ms = config.sensor_uart_read_timeout_ms;
        sensor_config.reconnect_delay_ms = config.sensor_uart_reconnect_ms;
        sensor_link = std::make_unique<device::SensorLinkManager>(
            std::move(sensor_config),
            [weak_hall = std::weak_ptr<sensor::HallParkingService>(hall_service),
             weak_fire = std::weak_ptr<event::FireAlarmService>(
                 fire_alarm_service),
             &fire_line_parser, &config](
                const std::string& line, const std::string& transport) {
                if (sensor::SensorProtocolParser::isFireLine(line)) {
                    const auto fire_target = weak_fire.lock();
                    if (!fire_target) return;
                    std::string error;
                    auto message = fire_line_parser.parseFire(
                        line, std::chrono::system_clock::now(), &error);
                    if (!message) {
                        util::logWarn(
                            "fire line rejected: " + error + " | " + line);
                        return;
                    }
                    message->transport = transport.empty() ? "uart" : transport;
                    message->raw = line;
                    const auto admission = fire_target->submitSignal(
                        toFireSignal(*message));
                    if (admission.status !=
                        event::FireCommandStatus::Queued) {
                        util::logError(
                            "fire line was not queued for durable commit: " +
                            admission.error);
                    }
                    return;
                }
                if (config.parking_occupancy_source == "HALL" ||
                    config.parking_occupancy_source == "HYBRID_OR") {
                    if (const auto hall_target = weak_hall.lock())
                        hall_target->handleLine(line, transport);
                }
            }, system_event_sink);
        if (!sensor_link->start()) {
            util::logError("Sensor UART/LoRa link could not be started");
            return shutdown_and_return(1);
        }
        sensor_link_for_led.store(sensor_link.get(), std::memory_order_release);
    }

    util::logInfo("waiting for camera MQTT events...");
    util::logInfo("press Ctrl+C to stop");

    // 실제 작업은 각 모듈의 작업 스레드가 수행하고 main은 종료 신호를 기다린다.
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!runtime_shutdown.shutdown()) {
        util::logError(
            "runtime lifetime barrier failed; terminating without destroying "
            "callback targets");
        std::_Exit(EXIT_FAILURE);
    }

    util::logInfo("pi-server stopped");

    return 0;
}

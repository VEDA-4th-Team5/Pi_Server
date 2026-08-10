#include "app/AppConfig.hpp"
#include "bestshot/BestShotReceiver.hpp"
#include "camera/CameraChannel.hpp"
#include "camera/CameraSnapshotApiClient.hpp"
#include "camera/RtspStreamReceiver.hpp"
#include "database/EventDatabase.hpp"
#include "device/SensorLinkManager.hpp"
#include "event/FireAlarmEvent.hpp"
#include "event/FireAlarmManager.hpp"
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
#include "sensor/SensorLinkManager.hpp"
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
// signal handler?€ ëª¨ë“  ?‘ì—… ?¤ë ˆ?œê? ?¨ê»˜ ë³´ëŠ” ?„ì—­ ì¢…ë£Œ ?Œë˜ê·¸ì´??
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

std::vector<std::shared_ptr<camera::CameraChannel>> createCameraChannels(
    const app::AppConfig& config
) {
    std::vector<std::shared_ptr<camera::CameraChannel>> channels;

    // shared_ptrë¥??°ë?ë¡?RTSP, MQTT, BestShot ëª¨ë“ˆ??ê°™ì? ì±„ë„ ?íƒœë¥?ê³µìœ ?œë‹¤.
    for (const auto& rtsp_config : config.rtsp_channels) {
        auto channel = std::make_shared<camera::CameraChannel>();

        channel->camera_id = config.camera_id;
        channel->channel_id = rtsp_config.channel_id;
        channel->rtsp_url = rtsp_config.rtsp_url;

        channels.push_back(channel);
    }

    // Snapshot API ?„ìš© ëª¨ë“œ?ì„œ??slot/channel ë§¤í•‘???¬ìš©???¼ë¦¬ ì±„ë„?€
    // ?„ìš”?˜ë‹¤. RTSP URL???†ëŠ” ì±„ë„?€ ?˜ì‹  ?¤ë ˆ?œì— ?˜ê¸°ì§€ ?ŠëŠ”??
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

// EVDA-138?ì„œ ì¹´ë©”???‘ë‹µ ê·œì•½???•ì •?˜ê¸° ???¬ìš©?˜ëŠ” MQTT ?”ì²­ ì´ˆì•ˆ?´ë‹¤.
// publish ?±ê³µ?€ Broker ?‘ìˆ˜ë§??˜ë??˜ë©° ?¤ì œ ì¹´ë©”??ì´¬ì˜ ?±ê³µ???»í•˜ì§€ ?ŠëŠ”??
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
    const std::string_view detail,
    const int overstay_threshold_seconds) {
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

// Qt??alarm_kindë§Œìœ¼ë¡?UI ê²½ê³  ì¢…ë¥˜ë¥??ë‹¨?˜ê³ , error_codeë¡??ì„¸ ?ì¸??
// ?œì‹œ?œë‹¤. DB EVENT_LOG???ì„¸ event_type?€ ë°”ê¾¸ì§€ ?ŠëŠ”??
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
    // OpenCVê°€ ?´ë??ìœ¼ë¡??¬ìš©?˜ëŠ” FFmpeg??TCP ?„ì†¡ê³??€?„ì•„?ƒì„ ì§€?•í•œ??
    // UDPë³´ë‹¤ ì§€?°ì? ì¡°ê¸ˆ ?????ˆì?ë§?CCTV ?¤íŠ¸ë¦¼ì˜ ?¨í‚· ?ì‹¤?????ˆì •?ì´??
    setenv(
        "OPENCV_FFMPEG_CAPTURE_OPTIONS",
        "rtsp_transport;tcp|stimeout;5000000|max_delay;500000",
        0
    );

    // FFmpeg ?€ ?°ë¦¬ Logger ë¥?ê±°ì¹˜ì§€ ?Šê³  ì§ì ‘ stderr ë¡??´ë‹¤("[h264 @ ...]
    // mmco: unref short failure" ë¥?. ?ìƒ ?„ë ˆ?„ë§ˆ???˜ì˜¤ë¯€ë¡??¤ì œ ë¡œê·¸ë¥?
    // ??–´ë²„ë¦°?? LOG_FFMPEG=false ë©?libavutil ìª½ì—???„ì˜ˆ ë§‰ëŠ”??
    // ì£¼ì˜: OpenCV ê°€ ?ì²´ FFmpeg ???•ì  ë§í¬??ë¹Œë“œ?¼ë©´ ë³„ë„ libavutil ??
    // ?°ë?ë¡????¤ì •????ë¨¹ì„ ???ˆë‹¤.
    if (!util::logEnabled("FFMPEG")) {
        av_log_set_level(AV_LOG_QUIET);
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    app::AppConfig config = app::AppConfig::loadFromEnv();

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

    // ì´ˆê¸°???œì„œ: DB -> ? íƒ??RTSP -> BestShot -> MQTT. Snapshot API ?„ìš©
    // ëª¨ë“œ??RTSP ìµœì´ˆ ?„ë ˆ?„ì„ ê¸°ë‹¤ë¦¬ì? ?ŠëŠ”??
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

    // ?¼ì„œ/?µì‹  ?¤ë ˆ?œëŠ” DB/MQTT I/Oë¥?ì§ì ‘ ê¸°ë‹¤ë¦¬ì? ?Šê³  bounded reporter queue??
    // ê¸°ë¡?œë‹¤. MQTT bridge???¤ì—???ì„±?˜ë?ë¡?atomic pointerë¡?ì¤€ë¹??íƒœë§?ê³µìœ ?œë‹¤.
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
    event::SystemEventReporter* system_event_sink =
        system_event_reporter.start() ? &system_event_reporter : nullptr;
    if (system_event_sink == nullptr)
        util::logError("System event reporter disabled after start failure");


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
                area->roi_x, area->roi_y, area->roi_width, area->roi_height,
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
                // ??ì¦ê±° workerê°€ T0 ê¸°ì? ?´ë?ì§€ë¥??´ë? ?€?¥í–ˆ?¤ë©´ ê°™ì? ?Œì¼??
                // ?¬ì‚¬?©í•œ?? ?´ì „ DB/ì´¬ì˜ ?¤íŒ¨ ?¸ì…˜ë§?ê¸°ì¡´ Snapshot?¼ë¡œ ë³´ì™„?œë‹¤.
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
    hall_capture_ports.handleRecognizedSession =
        [&parking_timer](const std::int64_t session_id,
                         const std::string& slot_id,
                         const std::string& plate_number) {
            if (parking_timer) {
                parking_timer->handleRecognizedSession(
                    session_id, slot_id, plate_number);
            }
        };
    // ë§í¬??ì´¬ì˜ runtimeë³´ë‹¤ ?¤ì—???´ë¦¬ë¯€ë¡??¬ì¸?°ë§Œ ë¯¸ë¦¬ ?¡ì•„?ê³  ë°°ì„ ?œë‹¤.
    device::SensorLinkManager* sensor_link_for_led = nullptr;
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
                if (sensor_link_for_led == nullptr) return false;
                return sensor_link_for_led->sendAlertCommand(payload, sequence);
            });
        util::logInfo("plate LED enabled: night=" +
                      std::to_string(config.plate_led_night_start_hour) +
                      "h-" +
                      std::to_string(config.plate_led_night_end_hour) +
                      "h settle=" +
                      std::to_string(config.plate_led_settle_ms) + "ms");
        // ì¡°ëª…?€ Piê°€ ì§ì ‘ ?¸ì¶œ??ê±°ëŠ” ?ˆì•½ ì´¬ì˜ ê²½ë¡œ?ë§Œ ë¶™ëŠ”?? ??ì¤??˜ë‚˜?¼ë„
        // êº¼ì ¸ ?ˆìœ¼ë©?ëª…ë ¹????ê±´ë„ ?˜ê?ì§€ ?Šìœ¼ë¯€ë¡?ì¡°ìš©???˜ì–´ê°€ì§€ ?ŠëŠ”??
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
    ocr_worker.setHallCaptureCallback(
        [&hall_ocr_coordinator, &plate_illuminator](
            const ocr::HallCaptureResult& result) {
            if (!hall_ocr_coordinator) return;
            if (plate_illuminator) {
                // ë²ˆí˜¸?ì„ ?½ì? ëª»í•œ ?¸ì…˜ë§??¨ì? ?ˆì•½ ì´¬ì˜??LED ë³´ì •?¼ë¡œ ?¤ì‹œ
                // ?œë„?œë‹¤. ?”ì²­ ?¤íŒ¨????ê±°ë???ì¡°ëª…?¼ë¡œ ?€ë¦¬ì? ?ŠëŠ”??
                if (result.plate_unreadable)
                    plate_illuminator->markPlateUnreadable(result.session_id);
                // ë²ˆí˜¸?ì„ ?•ë³´?ˆìœ¼ë©??¨ì? ì´¬ì˜?€ ì¦ê±° ?€?¥ìš©?´ë¼ ì¡°ëª…???†ë‹¤.
                else if (result.recognized)
                    plate_illuminator->markResolved(result.session_id);
            }
            hall_ocr_coordinator->onOcrOutcome({
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
                        return roi_settings.roiForSlot(slot_id);
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
            // ì´¬ì˜??ì¹´ë©”?¼ê? ?˜í–‰?˜ë?ë¡?Pi???¸ì¶œ ?œì ??ëª¨ë¥¸?? ?ë“±?´ë„
            // ?¸ì¶œê³??´ê¸‹?˜ê¸° ?Œë¬¸????ê²½ë¡œ?ì„œ??ì¡°ëª…???°ì? ?ŠëŠ”??
            capture_runtime =
                std::make_unique<parking::CaptureSchedulerRuntime>(
                    *capture_scheduler,
                    [&capture_mqtt_bridge, topic_prefix](
                        const parking::CaptureRequest& request) {
                        return capture_mqtt_bridge != nullptr &&
                            capture_mqtt_bridge->publishApplicationEvent(
                                topic_prefix + "/" + request.slotId,
                                buildCaptureRequestPayload(request), 1,
                                false);
                    });
        }
    }

    parking::EvidenceCaptureWorker::Config evidence_config;
    evidence_config.overstayDelay = std::chrono::seconds(
        overstay_settings.thresholdSeconds());
    auto evidence_worker = std::make_unique<parking::EvidenceCaptureWorker>(
        snapshot_storage, database, evidence_config,
        [&ocr_worker, &timer_events, &config](
            const parking::EvidenceCaptureResult& result) {
            if (!result.stored) return;
            if (result.reason == parking::EvidenceReason::OccupancyStart) {
                // 30/60ì´??€ OCR???œì„±?”ë˜ë©?ì°¨ëŸ‰???ë¦¬ë¥??¡ì? ?¤ì˜ ROIë¥?
                // ?¬ìš©?œë‹¤. ?¤ì?ì¤„ëŸ¬ê°€ êº¼ì§„ ?˜ê²½?ì„œ??ê¸°ì¡´ ?œì‘ ì¦ê±° OCRë¡?
                // fallback?˜ì—¬ ë²ˆí˜¸???¸ì‹ ê¸°ëŠ¥???¬ë¼ì§€ì§€ ?Šê²Œ ?œë‹¤.
                if (!(config.hall_capture_ocr_enabled &&
                      config.capture_sched_enabled)) {
                    ocr_worker.enqueue(static_cast<int>(result.sessionId),
                                       result.slotId, result.imagePath,
                                       result.enhancedImagePath);
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
        },
        camera_snapshot_api
            ? parking::EvidenceCaptureWorker::Capture{
                [&camera_snapshot_api, &snapshot_storage, &config,
                 &roi_settings](
                    const parking::EvidenceCaptureRequest& request,
                    const parking::EvidenceReason reason) {
                    const auto current_roi =
                        roi_settings.roiForSlot(request.slotId);
                    if (!current_roi) {
                        util::logError(
                            "evidence ROI is not configured: session=" +
                            std::to_string(request.sessionId) + " slot=" +
                            request.slotId);
                        return snapshot::StoredImagePair{};
                    }
                    camera::CameraGeneratedImages generated;
                    if (camera_snapshot_api->generate(
                            request.snapshotApiChannel, generated)) {
                        auto paths = snapshot_storage.saveCameraApiHallCapture(
                            request.channel ? request.channel->channel_id : "",
                            request.sessionId, request.slotId,
                            parking::toString(reason), *current_roi,
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
                        camera_snapshot_api->lastError());
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
                            parking::toString(reason), *current_roi),
                        {}};
                }}
            : parking::EvidenceCaptureWorker::Capture{});
    if (!evidence_worker->start()) {
        if (http_server) http_server->stop();
        system_event_reporter.stop();
        database.close();
        return 1;
    }
    evidence_worker_for_timer = evidence_worker.get();
    util::logInfo("parking evidence worker enabled: overstay_delay=" +
                  std::to_string(overstay_settings.thresholdSeconds()) + "s");

    overstay_settings.setApplyCallback(
        [&parking_timer, &evidence_worker](const std::chrono::milliseconds delay) {
            const auto evidence_count = evidence_worker
                ? evidence_worker->updateOverstayDelay(delay) : 0U;
            const auto timer_count = parking_timer
                ? parking_timer->updateParkingTimeout(delay) : 0U;
            util::logInfo("Overstay runtime policy applied: timers=" +
                          std::to_string(timer_count) + " evidence_jobs=" +
                          std::to_string(evidence_count));
        });

    bestshot::BestShotReceiver bestshot_receiver(
        channels, database, trigger_coordinator, ocr_worker, g_running);

    const auto sensor_link_mode =
        device::SensorLinkManager::parseMode(config.sensor_link_mode);
    std::unique_ptr<sensor::HallParkingService> hall_service;
    if (parking_timer && (config.hall_mqtt_input_enabled ||
                          sensor_link_mode != device::SensorLinkMode::Disabled ||
                          config.parking_occupancy_source == "CAMERA_IVA")) {
        hall_service = std::make_unique<sensor::HallParkingService>(
            parking_slot_configs, config, channels, database,
            [&ocr_worker](int session_id) {
                ocr_worker.cancelSession(session_id);
            },
            *parking_timer, *timer_events, *evidence_worker,
            system_event_sink,
            [&capture_runtime, &hall_ocr_coordinator, &plate_illuminator](
                const parking::ParkingTransitionResult& transition) {
                if (hall_ocr_coordinator)
                    hall_ocr_coordinator->onTransition(transition);
                if (capture_runtime) capture_runtime->onTransition(transition);
                if (plate_illuminator &&
                    transition.code ==
                        parking::ParkingTransitionCode::SessionCompleted) {
                    plate_illuminator->forget(transition.sessionId);
                }
            });
    }

    if (rtsp_capture_enabled) rtsp_receiver.start();

    if (rtsp_capture_enabled &&
        !rtsp_receiver.waitForInitialFrames(config.initial_frame_timeout_sec)) {
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
    if (!rtsp_capture_enabled) {
        util::logInfo(
            "RTSP capture disabled: Camera Snapshot API is the image source");
    }

    // ?¬ì‹œ???„ì— ?ì„±??ACTIVE ?¸ì…˜?€ ë©”ëª¨ë¦?evidence queueê°€ ?¬ë¼ì¡Œìœ¼ë¯€ë¡?
    // ?ë˜ entry_time(T0)??steady_clock?¼ë¡œ ?˜ì‚°??timerë³´ë‹¤ ë¨¼ì? ë³µì›?œë‹¤.
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
                {area->roi_x, area->roi_y,
                 area->roi_width, area->roi_height},
                restoreMonotonicStart(record.parked_at,
                                      fallback_elapsed),
                area->snapshot_api_channel})) {
            ++restored_evidence;
        } else {
            ++failed_evidence_restore;
        }
    }
    util::logInfo("parking evidence restored active sessions=" +
                  std::to_string(restored_evidence) + " failed=" +
                  std::to_string(failed_evidence_restore));

    // ?¤ì • ë³€ê²½ìœ¼ë¡?ì¦‰ì‹œ ë§Œë£Œ?˜ëŠ” ?œì„± ?¸ì…˜???¤ì œ FrameBufferë¥??¬ìš©?????ˆë„ë¡?
    // ìµœì´ˆ RTSP frameê³?evidence ë³µì›??ì¤€ë¹„ëœ ?¤ìŒ ?¸ë? PUT ?”ì²­??ë°›ëŠ”??
    if (config.http_api_enabled) {
        http::ServerConfig http_config;
        http_config.listen_address = config.http_listen_address;
        http_config.port = config.http_port;
        http_config.tls_certificate_path = config.http_tls_certificate_path;
        http_config.tls_private_key_path = config.http_tls_private_key_path;
        http_config.data_root = config.http_data_root;
        http_config.max_image_bytes = static_cast<std::size_t>(
            std::max(1, config.http_max_image_mb)) * 1024U * 1024U;
        http_server = std::make_unique<http::ParkingHttpServer>(
            database, http_config, &overstay_settings, &roi_settings);
        if (!http_server->start()) {
            g_running.store(false);
            if (telegram_notifier) telegram_notifier->stop();
            rtsp_receiver.stop();
            hall_service.reset();
            evidence_worker->stop();
            parking_timer.reset();
            timer_events.reset();
            system_event_reporter.stop();
            database.close();
            return 1;
        }
    }

    ocr_worker.start();
    bestshot_receiver.start();

    std::unique_ptr<event::FireAlarmManager> fire_alarm_manager;
    mqtt::MqttEventBridge mqtt_bridge(
        config,
        channels,
        database,
        snapshot_storage,
        trigger_coordinator,
        ocr_worker,
        parking_slot_configs,
        [&hall_service](const std::string& line) {
            if (hall_service) hall_service->handleLine(line);
        },
        [&fire_alarm_manager](const std::string& channel_id,
                              const std::string& alarm_id) {
            return fire_alarm_manager &&
                   fire_alarm_manager->acknowledge(channel_id, alarm_id);
        },
        [&hall_service](const event::IvaOccupancySignal& signal) {
            return hall_service &&
                   hall_service->handleCameraIvaSignal(signal);
        },
        telegram_notifier_ptr
    );
    capture_mqtt_bridge = &mqtt_bridge;

    if (!mqtt_bridge.start()) {
        g_running.store(false);
        if (telegram_notifier) telegram_notifier->stop();
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
    system_event_mqtt_bridge.store(&mqtt_bridge, std::memory_order_release);

    if (capture_runtime) {
        capture_runtime->start();
        util::logInfo(
            "capture scheduler enabled (DRAFT protocol pending EVDA-138): "
            "topic_prefix=" + config.capture_topic_prefix +
            " offsets=" + config.capture_offsets_sec + "s retries=" +
            std::to_string(config.capture_max_retries));
    }

    // MQTT publisherê°€ ì¤€ë¹„ëœ ?¤ì—ë§?ë°œí–‰ ì½œë°±??ê±????ˆìœ¼ë¯€ë¡?mqtt_bridge
    // ?œì‘ ?´í›„???ì„±?œë‹¤. ?”ì¬ ìµœì¢… ?ë‹¨?€ ?˜ì? ?Šê³  ?„ë³´ ?´ë²¤?¸ë§Œ ?¬ë¦°??
    // (ê´€?œì‹¤ ?¬ëŒ???•ì •) ??event::FireAlarmManager ê³„ì•½?€ë¡?
    if (config.fire_alarm_enabled) {
        fire_alarm_manager = std::make_unique<event::FireAlarmManager>(
            config.camera_id,
            config.default_channel_id,
            config.fire_topic_prefix,
            event::parseFireSensorBindings(config.fire_sensor_channel_map),
            [&mqtt_bridge](const std::string& topic,
                           const std::string& payload) {
                const auto separator = topic.find_last_of('/');
                const std::string target =
                    separator == std::string::npos ? "unmapped"
                                                   : topic.substr(separator + 1);

                // ?”ì¬ ?„ìš© ? í”½?€ ìµœì‹  ?íƒœ ë³µì›??retained ë©”ì‹œì§€?? ì£¼ì°¨ ?íƒœ
                // ? í”½?ëŠ” ?”ì¬ë¥??ì? ?Šê³  ?µí•© ?´ë²¤??? í”½ë§??¨ê»˜ ë°œí–‰?œë‹¤.
                const bool fire_state_published =
                    mqtt_bridge.publishApplicationEvent(topic, payload, 1, true);
                const bool event_published = mqtt_bridge.publishQtEvent(
                    "parking/v1/events/" + target, payload, 1, false);

                if (!fire_state_published || !event_published) {
                    util::logError(
                        "fire alarm MQTT fan-out failed: channel=" + target +
                        " state=" +
                        (fire_state_published ? "ok" : "failed") +
                        " event=" + (event_published ? "ok" : "failed"));
                }
                return fire_state_published && event_published;
            });
        util::logInfo(
            "fire alarm enabled: topic_prefix=" + config.fire_topic_prefix +
            " bindings=" + std::to_string(fire_alarm_manager->bindingCount()));
    }

    // ì´¬ì˜ runtimeê³?MQTT publisherê°€ ì¤€ë¹„ëœ ???¤ì œ UART/LoRa ?…ë ¥???°ë‹¤.
    // ?”ì¬?€ ?€?¼ì„œ??ê°™ì? STM32 UART ë§í¬ë¥?ê³µìœ ?˜ë?ë¡?SensorLinkManager??
    // ?˜ë‚˜ë§??´ê³ , ?˜ì‹  ?¼ì¸???‘ë‘??FIRE:/SENSOR:)ë¡?ê°ˆë¼ ë³´ë‚¸??
    const sensor::SensorProtocolParser fire_line_parser;
    std::unique_ptr<device::SensorLinkManager> sensor_link;
    if ((hall_service || fire_alarm_manager) &&
        sensor_link_mode != device::SensorLinkMode::Disabled) {
        device::SensorLinkManager::Config sensor_config;
        sensor_config.mode = sensor_link_mode;
        sensor_config.uart.device_path = config.sensor_uart_device;
        sensor_config.uart.baud_rate = config.sensor_uart_baud_rate;
        sensor_config.uart.read_timeout_ms = config.sensor_uart_read_timeout_ms;
        sensor_config.reconnect_delay_ms = config.sensor_uart_reconnect_ms;
        sensor_link = std::make_unique<device::SensorLinkManager>(
            std::move(sensor_config),
            [&hall_service, &fire_alarm_manager, &fire_line_parser, &config](
                const std::string& line, const std::string& transport) {
                if (sensor::SensorProtocolParser::isFireLine(line)) {
                    if (!fire_alarm_manager) return;
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
                    fire_alarm_manager->onFireSignal(toFireSignal(*message));
                    return;
                }
                if (hall_service &&
                    config.parking_occupancy_source == "HALL") {
                    hall_service->handleLine(line, transport);
                }
            }, system_event_sink);
        if (!sensor_link->start()) {
            util::logError("Sensor UART/LoRa link could not be started");
            sensor_link.reset();
        }
        sensor_link_for_led = sensor_link.get();
    }

    if (timer_events) {
        timer_events->setPublisher(
            [&mqtt_bridge, &config, &database, &overstay_settings](
                const std::string_view event_type, const std::int64_t session_id,
                const std::string_view slot_id, const std::string_view plate,
                const std::string_view timestamp, const std::string_view detail) {
                if (slot_id.empty() || session_id < 0) return;
                const std::string payload = buildQtParkingEvent(
                    config, database, event_type, session_id, slot_id, plate,
                    timestamp, detail,
                    overstay_settings.thresholdSeconds());
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

    // ?¤ì œ ?‘ì—…?€ ê°?ëª¨ë“ˆ???‘ì—… ?¤ë ˆ?œê? ?˜í–‰?˜ê³  main?€ ì¢…ë£Œ ? í˜¸ë¥?ê¸°ë‹¤ë¦°ë‹¤.
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ?ì„±????ˆœ?¼ë¡œ ?•ë¦¬?˜ì—¬ ?¬ìš© ì¤‘ì¸ ?ì›??ë¨¼ì? ?¬ë¼ì§€??ê²ƒì„ ë§‰ëŠ”??
    // ì´¬ì˜ runtime?€ LED ëª…ë ¹?¼ë¡œ sensor_linkë¥??°ë?ë¡?ë§í¬ë³´ë‹¤ ë¨¼ì? ë©ˆì¶˜??
    if (capture_runtime) capture_runtime->stop();
    if (sensor_link) sensor_link->stop();
    fire_alarm_manager.reset();
    // reporter queueë¥?MQTTê°€ ?´ì•„ ?ˆì„ ??ëª¨ë‘ ë¹„ìš´ ??bridge ?˜ëª…??ì¢…ë£Œ?œë‹¤.
    if (telegram_notifier) telegram_notifier->stop();
    telegram_notifier.reset();
    system_event_reporter.stop();
    system_event_mqtt_bridge.store(nullptr, std::memory_order_release);
    mqtt_bridge.stop();
    bestshot_receiver.stop();
    hall_service.reset();
    evidence_worker->stop();
    ocr_worker.stop();
    parking_timer.reset();
    timer_events.reset();
    rtsp_receiver.stop();
    if (http_server) http_server->stop();
    database.close();

    util::logInfo("pi-server stopped");

    return 0;
}

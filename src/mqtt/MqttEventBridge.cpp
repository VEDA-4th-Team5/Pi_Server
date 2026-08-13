#include "mqtt/MqttEventBridge.hpp"

#include "database/EventDatabase.hpp"
#include "event/CameraEventParser.hpp"
#include "event/EventPayloadBuilder.hpp"
#include "event/IvaEventResolver.hpp"
#include "notification/TelegramChannelNotifier.hpp"
#include "ocr/PlateImageEnhancer.hpp"
#include "util/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace mqtt {

namespace {

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channel_id) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channel_id) return channel;
    return nullptr;
}

bool hasTopicSegment(const std::string& topic, const std::string& segment) {
    if (segment.empty()) return false;
    std::size_t begin{};
    while (begin <= topic.size()) {
        const std::size_t end = topic.find('/', begin);
        if (topic.substr(begin, end - begin) == segment) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

event::IvaOccupancyAction toOccupancyAction(const std::string& action) {
    if (action == "ENTER") return event::IvaOccupancyAction::Enter;
    if (action == "INTRUSION") return event::IvaOccupancyAction::Intrusion;
    if (action == "EXIT") return event::IvaOccupancyAction::Exit;
    return event::IvaOccupancyAction::Unsupported;
}

std::optional<std::chrono::system_clock::time_point> parseCameraUtc(
    const std::string& value) {
    if (value.size() < 20) return std::nullopt;
    std::tm utc{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) return std::nullopt;

    std::size_t cursor = 19;
    std::chrono::milliseconds fraction{};
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        int milliseconds{};
        int digits{};
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9') {
            if (digits < 3)
                milliseconds = milliseconds * 10 + (value[cursor] - '0');
            ++digits;
            ++cursor;
        }
        if (digits == 0) return std::nullopt;
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
        fraction = std::chrono::milliseconds(milliseconds);
    }

    int offset_seconds{};
    if (cursor < value.size() &&
        (value[cursor] == 'Z' || value[cursor] == 'z')) {
        ++cursor;
    } else if (cursor + 6 == value.size() &&
               (value[cursor] == '+' || value[cursor] == '-') &&
               value[cursor + 3] == ':') {
        const auto digit = [&value](const std::size_t index) -> int {
            return value[index] >= '0' && value[index] <= '9'
                ? value[index] - '0' : -1;
        };
        const int h1 = digit(cursor + 1);
        const int h2 = digit(cursor + 2);
        const int m1 = digit(cursor + 4);
        const int m2 = digit(cursor + 5);
        if (h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0) return std::nullopt;
        const int hours = h1 * 10 + h2;
        const int minutes = m1 * 10 + m2;
        if (hours > 23 || minutes > 59) return std::nullopt;
        offset_seconds = (hours * 60 + minutes) * 60;
        if (value[cursor] == '-') offset_seconds = -offset_seconds;
        cursor += 6;
    } else {
        return std::nullopt;
    }
    if (cursor != value.size()) return std::nullopt;

#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&utc);
#else
    const std::time_t seconds = timegm(&utc);
#endif
    if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(seconds) + fraction -
        std::chrono::seconds(offset_seconds);
}

}

MqttEventBridge::MqttEventBridge(
    const app::AppConfig& config,
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    database::EventDatabase& database,
    snapshot::SnapshotStorage& snapshot_storage,
    ocr::OcrWorker& ocr_worker,
    std::vector<parking::ParkingSlotConfig> parking_slot_configs,
    SensorMessageHandler sensor_message_handler,
    FireAckHandler fire_ack_handler,
    IvaOccupancyHandler iva_occupancy_handler,
    std::unique_ptr<IMqttTransport> transport,
    notification::TelegramChannelNotifier* telegram_notifier
)
    : config_(config),
      channels_(channels),
      database_(database),
      snapshot_storage_(snapshot_storage),
      ocr_worker_(ocr_worker),
      parking_slot_configs_(std::move(parking_slot_configs)),
      sensor_message_handler_(std::move(sensor_message_handler)),
      fire_ack_handler_(std::move(fire_ack_handler)),
      iva_occupancy_handler_(std::move(iva_occupancy_handler)),
      telegram_notifier_(telegram_notifier),
      endpoint_(std::move(transport)) {
    endpoint_.bindApplicationHandler(
        [this](const std::string& topic, const std::string& payload) {
            onMessage(topic, payload);
        });
    IMqttTransport::ObserverCallbacks observers;
    observers.onFact = [this](const MqttTransportFact& fact) {
        if (transport_fact_handler_ && !transport_fact_handler_(fact)) {
            util::logError(
                "MQTT transport fact queue rejected an observation");
        }
    };
    endpoint_.bindTransportObservers(std::move(observers));
}

std::string stableEventIdentity(const std::string& topic,
                                const std::string& payload,
                                const std::string& discriminator) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&hash](const std::string_view value) {
        for (const unsigned char byte : value) {
            hash ^= static_cast<std::uint64_t>(byte);
            hash *= 1099511628211ULL;
        }
    };
    add(topic);
    add("\n");
    add(payload);
    add("\n");
    add(discriminator);
    std::ostringstream output;
    output << "camera-mqtt:" << std::hex << std::setw(16)
           << std::setfill('0') << hash;
    return output.str();
}

MqttEventBridge::~MqttEventBridge() {
    if (!stop()) std::terminate();
}

bool MqttEventBridge::bindSensorMessageHandler(SensorMessageHandler handler) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !handler)
        return false;
    sensor_message_handler_ = std::move(handler);
    return true;
}

bool MqttEventBridge::bindFireAckHandler(FireAckHandler handler) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !handler)
        return false;
    fire_ack_handler_ = std::move(handler);
    return true;
}

bool MqttEventBridge::bindIvaOccupancyHandler(IvaOccupancyHandler handler) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !handler)
        return false;
    iva_occupancy_handler_ = std::move(handler);
    return true;
}

bool MqttEventBridge::bindTransportFactHandler(
    TransportFactHandler handler) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !handler)
        return false;
    transport_fact_handler_ = std::move(handler);
    return true;
}

bool MqttEventBridge::bindRegularEgressAdmission(
    RegularEgressAdmission admission) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !admission)
        return false;
    regular_egress_admission_ = std::move(admission);
    return true;
}

bool MqttEventBridge::bindParkingRoiResolver(RoiResolver resolver) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !resolver)
        return false;
    roi_resolver_ = std::move(resolver);
    return true;
}

bool MqttEventBridge::bindCameraSnapshotGenerator(
    CameraSnapshotGenerator generator) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !generator)
        return false;
    camera_snapshot_generator_ = std::move(generator);
    return true;
}

bool MqttEventBridge::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (config_.fire_alarm_enabled && !fire_ack_handler_) {
        util::logError("MQTT start rejected: Fire ACK target is not bound");
        return false;
    }
    if (config_.fire_alarm_enabled && !transport_fact_handler_) {
        util::logError(
            "MQTT start rejected: Fire delivery fact target is not bound");
        return false;
    }
    if (config_.fire_alarm_enabled && !regular_egress_admission_) {
        util::logError(
            "MQTT start rejected: Fire-priority egress gate is not bound");
        return false;
    }
    if (config_.hall_mqtt_input_enabled &&
        config_.parking_occupancy_source == "HALL" &&
        !sensor_message_handler_) {
        util::logError("MQTT start rejected: Hall target is not bound");
        return false;
    }
    if (config_.parking_occupancy_source == "CAMERA_IVA" &&
        !iva_occupancy_handler_) {
        util::logError("MQTT start rejected: CAMERA_IVA target is not bound");
        return false;
    }

    std::vector<MqttSubscription> subscriptions;
    if (config_.hall_mqtt_input_enabled &&
        config_.parking_occupancy_source == "HALL") {
        subscriptions.push_back({config_.hall_mqtt_topic, 1});
    }
    if (config_.fire_alarm_enabled) {
        subscriptions.push_back(
            {config_.fire_command_topic_prefix + "/+", 1});
    }
    subscriptions.push_back({config_.mqtt_event_sub_topic, 0});

    if (!endpoint_.start(
            {"pi-server-" + config_.camera_id, config_.mqtt_host,
             config_.mqtt_port, 60},
            subscriptions)) {
        util::logError("MQTT transport start failed");
        return false;
    }

    {
        std::ostringstream oss;
        oss << "MQTT connected: "
            << config_.mqtt_host
            << ":"
            << config_.mqtt_port;

        util::logInfo(oss.str());
    }

    util::logInfo("MQTT subscribed: " + config_.mqtt_event_sub_topic);
    if (config_.hall_mqtt_input_enabled)
        util::logInfo("Hall MQTT subscribed: " + config_.hall_mqtt_topic);
    if (config_.fire_alarm_enabled && fire_ack_handler_)
        util::logInfo("Fire ACK MQTT subscribed: " +
                      config_.fire_command_topic_prefix + "/+");

    return true;
}

void MqttEventBridge::closeIngress() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    endpoint_.closeIngress();
}

bool MqttEventBridge::quiesceIngress(
    const std::chrono::milliseconds timeout) {
    std::lock_guard lock(lifecycle_mutex_);
    const bool drained = endpoint_.quiesceIngress(timeout);
    if (!drained)
        util::logError("MQTT application callback drain timed out");
    return drained;
}

bool MqttEventBridge::quiesceTransportObservers(
    const std::chrono::milliseconds timeout) {
    std::lock_guard lock(lifecycle_mutex_);
    const bool drained = endpoint_.quiesceTransportObservers(timeout);
    if (!drained)
        util::logError("MQTT transport observer drain timed out");
    return drained;
}

bool MqttEventBridge::abortActiveEpoch(
    const MqttConnectionEpoch expectedEpoch) noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    return endpoint_.abortActiveEpoch(expectedEpoch);
}

bool MqttEventBridge::stop(const std::chrono::milliseconds timeout) {
    std::lock_guard lock(lifecycle_mutex_);
    const bool stopped = endpoint_.stop(timeout);
    if (!stopped)
        util::logError("MQTT transport callback join failed or timed out");
    return stopped;
}

MqttEndpointState MqttEventBridge::state() const noexcept {
    return endpoint_.state();
}

void MqttEventBridge::onMessage(const std::string& raw_topic,
                                const std::string& raw_payload) {

    if (config_.hall_mqtt_input_enabled &&
        raw_topic == config_.hall_mqtt_topic) {
        if (sensor_message_handler_) sensor_message_handler_(raw_payload);
        return;
    }

    const std::string fire_command_prefix =
        config_.fire_command_topic_prefix + "/";
    if (config_.fire_alarm_enabled && fire_ack_handler_ &&
        raw_topic.rfind(fire_command_prefix, 0) == 0) {
        const std::string channel_id =
            raw_topic.substr(fire_command_prefix.size());
        if (channel_id.empty() || channel_id.find('/') != std::string::npos) {
            util::logWarn("Fire ACK rejected: invalid command topic " +
                          raw_topic);
            return;
        }
        try {
            const auto body = nlohmann::json::parse(raw_payload);
            if (!body.is_object() ||
                body.value("command", std::string{}) != "ALARM_ACK") {
                util::logWarn("Fire ACK rejected: unsupported command");
                return;
            }
            const std::string payload_channel =
                body.value("channel_id", std::string{});
            if (!payload_channel.empty() && payload_channel != channel_id) {
                util::logWarn("Fire ACK rejected: topic/payload channel mismatch");
                return;
            }
            const std::string alarm_id =
                body.value("alarm_id",
                           body.value("event_id", std::string{}));
            if (!fire_ack_handler_(channel_id, alarm_id)) {
                util::logWarn("Fire ACK was not applied: channel=" + channel_id +
                              " alarm_id=" + alarm_id);
            }
        } catch (const std::exception& error) {
            util::logWarn("Fire ACK rejected: invalid JSON: " +
                          std::string(error.what()));
        }
        return;
    }

    // 한 PUBLISH에 여러 ONVIF NotificationMessage가 묶여도 각각 처리한다.
    auto camera_events = event::CameraEventParser::parseMany(
        raw_topic, raw_payload, config_.default_channel_id);
    if (camera_events.size() > 1) {
        util::logLine("CAMERA_EVENT",
                      "bundled notifications parsed count=" +
                          std::to_string(camera_events.size()) +
                          " topic=" + raw_topic);
    }
    // parseMany preserves the NotificationMessage order from the camera.
    // Reordering by action reverses valid INTRUSION(T1)->EXIT(T2) bundles and
    // makes the older occupancy state win, so enqueue facts causally as sent.
    for (auto& camera_event : camera_events) {
        processCameraEvent(std::move(camera_event));
    }
}

void MqttEventBridge::processCameraEvent(event::CameraEvent camera_event) {
    const std::string& raw_topic = camera_event.raw_topic;

    // IVA Area 이벤트는 모든 채널이 아니라 해당 주차면의 채널/ROI만 증거로 저장한다.
    if (camera_event.is_iva_area_event) {
        if (!camera_event.protocol_valid) {
            util::logWarn("IVA protocol rejected: " +
                          camera_event.protocol_error + " topic=" + raw_topic);
            return;
        }
        std::string mapping_error;
        const auto target = event::IvaEventResolver::resolve(
            config_.camera_id, camera_event, parking_slot_configs_,
            config_.iva_areas, &mapping_error);
        if (!target) {
            util::logWarn(
                "IVA area event rejected: " + mapping_error +
                " token=" + camera_event.video_source_token +
                " topic=" + raw_topic);
            return;
        }
        if (camera_event.is_smart_parking_iva) {
            if (camera_event.declared_camera_id != config_.camera_id ||
                camera_event.source_id != config_.camera_id ||
                camera_event.slot_id != target->slotId ||
                !hasTopicSegment(raw_topic, target->slotId) ||
                !hasTopicSegment(raw_topic,
                    camera_event.action == "ENTER"
                        ? "enter"
                        : (camera_event.action == "INTRUSION"
                               ? "intrusion"
                               : "exit"))) {
                util::logWarn(
                    "IVA protocol rejected: topic/payload/config mismatch "
                    "topic=" + raw_topic + " slot=" + target->slotId);
                return;
            }
        }

        const auto iva_action = toOccupancyAction(camera_event.action);
        if (config_.parking_occupancy_source == "CAMERA_IVA") {
            if (!iva_occupancy_handler_) {
                util::logError("IVA occupancy handler is not configured");
                return;
            }
            // The fixed smart-parking publication has neither an event id nor
            // a source timestamp. It cannot distinguish broker redelivery
            // from a later vehicle lifecycle, so only raw WiseAI observations
            // are allowed to mutate occupancy.
            if (camera_event.is_smart_parking_iva ||
                !camera_event.timestamp_from_source ||
                camera_event.object_id.empty()) {
                util::logWarn(
                    "IVA occupancy rejected: source timestamp/object identity "
                    "is missing topic=" + raw_topic);
                return;
            }
            const auto source_time = parseCameraUtc(camera_event.timestamp);
            if (!source_time) {
                util::logWarn(
                    "IVA occupancy rejected: invalid camera UtcTime topic=" +
                    raw_topic);
                return;
            }
            event::IvaOccupancySignal signal;
            signal.slotId = target->slotId;
            signal.cameraId = config_.camera_id;
            signal.channelId = target->channelId;
            signal.videoSourceToken = camera_event.video_source_token;
            signal.ruleName = target->ruleName;
            signal.objectId = camera_event.object_id;
            signal.action = iva_action;
            signal.authoritativeExit = true;
            signal.occupancyAuthority = true;
            signal.sourceIdentity = stableEventIdentity(
                raw_topic, camera_event.raw_payload,
                target->slotId + "|" + camera_event.video_source_token +
                    "|" + target->ruleName + "|" +
                    camera_event.object_id + "|" + camera_event.action + "|" +
                    camera_event.timestamp);
            signal.occurredAt = *source_time;
            signal.occurredAtFromSource = true;
            if (!iva_occupancy_handler_(signal)) {
                util::logWarn("IVA occupancy event was not accepted: slot=" +
                              target->slotId + " action=" +
                              camera_event.action);
            }
            return;
        }

        // HALL 모드에서는 IVA가 세션을 종료하지 않고 촬영 후보로만 동작한다.
        if (iva_action != event::IvaOccupancyAction::Intrusion ||
            !camera_event.is_active) {
            util::logInfo("IVA action ignored in HALL mode: action=" +
                          camera_event.action + " topic=" + raw_topic);
            return;
        }
        if (!iva_occupancy_handler_ || camera_event.is_smart_parking_iva ||
            !camera_event.timestamp_from_source ||
            camera_event.object_id.empty()) {
            util::logWarn(
                "IVA correlation rejected: exact source time/ObjectId or "
                "durable handler is missing topic=" + raw_topic);
            return;
        }
        const auto source_time = parseCameraUtc(camera_event.timestamp);
        if (!source_time) {
            util::logWarn(
                "IVA correlation rejected: invalid camera UtcTime topic=" +
                raw_topic);
            return;
        }
        event::IvaOccupancySignal correlation_signal;
        correlation_signal.slotId = target->slotId;
        correlation_signal.cameraId = config_.camera_id;
        correlation_signal.channelId = target->channelId;
        correlation_signal.videoSourceToken = camera_event.video_source_token;
        correlation_signal.ruleName = target->ruleName;
        correlation_signal.objectId = camera_event.object_id;
        correlation_signal.action = iva_action;
        correlation_signal.authoritativeExit = false;
        correlation_signal.occupancyAuthority = false;
        correlation_signal.sourceIdentity = stableEventIdentity(
            raw_topic, camera_event.raw_payload,
            target->slotId + "|" + camera_event.video_source_token + "|" +
                target->ruleName + "|" + camera_event.object_id + "|" +
                camera_event.action + "|" + camera_event.timestamp);
        correlation_signal.occurredAt = *source_time;
        correlation_signal.occurredAtFromSource = true;
        if (!iva_occupancy_handler_(correlation_signal)) {
            util::logWarn("IVA correlation was not durably accepted: slot=" +
                          target->slotId + " object=" +
                          camera_event.object_id);
            return;
        }
        std::shared_ptr<camera::CameraChannel> channel =
            findChannel(channels_, target->channelId);
        if (!channel) {
            util::logError("IVA mapped RTSP channel is not configured: " +
                           target->channelId);
            return;
        }

        camera_event.rule_name = target->ruleName;
        camera_event.iva_area_id = target->areaName;
        camera_event.slot_id = target->slotId;
        // BestShot metadata가 뒤이어 도착하면 같은 주차면으로 연결할 pending을 만든다.
        if (!roi_resolver_) {
            util::logError("IVA snapshot rejected: runtime ROI resolver is "
                           "not configured");
            return;
        }
        const auto applied_roi = roi_resolver_(target->slotId);
        if (!applied_roi) {
            util::logError("IVA snapshot rejected: runtime ROI is missing: "
                           "slot=" + target->slotId);
            return;
        }
        std::string snapshot_path;
        std::string enhanced_path;
        bool camera_api_snapshot_saved = false;
        if (camera_snapshot_generator_) {
            int snapshot_api_channel = 0;
            for (const auto& area : config_.iva_areas) {
                if (area.slot_id == target->slotId &&
                    area.area_name == target->areaName &&
                    area.channel_id == target->channelId) {
                    snapshot_api_channel = area.snapshot_api_channel;
                    break;
                }
            }
            camera::CameraGeneratedImages generated;
            if (camera_snapshot_generator_(snapshot_api_channel, generated)) {
                const auto paths = snapshot_storage_.saveCameraApiIvaSnapshot(
                    channel->channel_id, target->slotId, applied_roi->value,
                    generated.originalJpeg, generated.enhancedJpeg);
                snapshot_path = paths.originalPath;
                enhanced_path = paths.enhancedPath;
                camera_api_snapshot_saved = !snapshot_path.empty() &&
                    !enhanced_path.empty();
                if (camera_api_snapshot_saved) {
                    util::logLine(
                        "CAMERA_SNAPSHOT_API",
                        "IVA snapshot stored slot=" + target->slotId +
                        " area=" + target->areaName +
                        " channel=" + std::to_string(snapshot_api_channel) +
                        " run_id=" + generated.runId);
                }
            } else {
                util::logError(
                    "camera snapshot API IVA capture failed slot=" +
                    target->slotId + " error=" +
                    (generated.runId.empty() ? "generator error" :
                     "empty run result"));
            }
        }
        if (!camera_api_snapshot_saved) {
            if (camera_snapshot_generator_ &&
                !config_.camera_snapshot_api_rtsp_fallback) {
                util::logError(
                    "IVA snapshot rejected: camera API failed and RTSP "
                    "fallback is disabled slot=" + target->slotId);
                return;
            }
            snapshot_path = snapshot_storage_.saveIvaAreaSnapshot(
                channel, target->slotId, applied_roi->value);
            if (!snapshot_path.empty())
                enhanced_path = ocr::enhanceIvaSceneImage(snapshot_path);
        }
        std::string payload_json = event::EventPayloadBuilder::buildJson(
            config_.camera_id, channel->channel_id, camera_event, snapshot_path,
            &*applied_roi);

        database::EventRecord record;
        record.camera_id = config_.camera_id;
        record.channel_id = channel->channel_id;
        record.slot_id = target->slotId;
        record.source_type = camera_event.source_type;
        record.source_id = camera_event.source_id;
        record.event_type = camera_event.event_type;
        record.severity = camera_event.severity;
        record.confidence = 1.0;
        record.snapshot_path = snapshot_path;
        record.raw_topic = camera_event.raw_topic;
        record.raw_payload = camera_event.raw_payload;
        record.payload_json = payload_json;
        record.created_at = camera_event.timestamp;
        record.applied_roi = applied_roi->value;
        record.roi_revision = applied_roi->revision;
        database_.insertEvent(record);
        if (!enhanced_path.empty()) {
            database_.attachEnhancedPlateImage(snapshot_path, enhanced_path);
            ocr_worker_.enqueueScene(target->slotId, snapshot_path, enhanced_path);
        }
        // IVA snapshot은 입차 구역 증빙과 BestShot 연결에 사용한다.
        // Gemini에는 ROI 원본과 개선본을 함께 보내며, 이후 Plate BestShot OCR이
        // 도착하면 주차 세션의 최종 판독값으로 사용한다.

        const std::string event_topic =
            "parking/v1/events/" + target->slotId;
        const std::string state_topic =
            "parking/v1/state/" + target->slotId;
        // QoS1 packet identifiers cannot be reused while in flight. QoS0
        // completion callbacks could otherwise alias a durable Fire PUBACK.
        const bool event_published =
            publishQtEvent(event_topic, payload_json, 1, false);
        const bool state_published =
            publishQtEvent(state_topic, payload_json, 1, true);
        if (!event_published || !state_published) {
            util::logWarn("Qt IVA MQTT publish failed: slot=" +
                          target->slotId);
        }
        util::logLine("IVA_SNAPSHOT", "slot=" + target->slotId +
                      " area=" + target->areaName +
                      " rule=" + target->ruleName +
                      " token=" + camera_event.video_source_token +
                      " channel=" + target->channelId +
                      " snapshot=" + snapshot_path +
                      " enhanced=" + enhanced_path);
        return;
    }

    // Motion/ObjectDetection 등 일반 ONVIF 이벤트는 IVA Intrusion과 동시에 여러 건
    // 발생한다. 이 이벤트까지 저장하면 같은 차량이 snapshots/ch1에 중복 저장되므로,
    // 주차 증거 이미지는 위 snapshots/ch1/EVxx/scene 경로에서만 생성한다.
    util::logLine("CAMERA_EVENT", "ignored non-IVA snapshot event: topic=" +
                  camera_event.raw_topic + " type=" + camera_event.event_type);
}

bool MqttEventBridge::publish(
    const std::string& topic,
    const std::string& payload,
    const int qos,
    const bool retain
) {
    RegularEgressPermit permit;
    if (config_.fire_alarm_enabled) {
        if (!regular_egress_admission_)
            return false;
        permit = regular_egress_admission_();
        if (!permit) {
            util::logWarn(
                "MQTT regular publish rejected until Fire retained sync: "
                "topic=" + topic);
            return false;
        }
        if (!permit.isCurrent()) {
            util::logWarn(
                "MQTT regular publish rejected after Fire readiness changed: "
                "topic=" + topic);
            return false;
        }
    }
    const auto result = config_.fire_alarm_enabled
        ? endpoint_.publishInEpoch(
              topic, payload, qos, retain, permit.epoch())
        : endpoint_.publish(topic, payload, qos, retain);
    if (!result.accepted) {
        util::logError("MQTT publish was not accepted: topic=" + topic);
        return false;
    }
    return true;
}

bool MqttEventBridge::publishQtEvent(const std::string& topic,
                                     const std::string& payload,
                                     const int qos,
                                     const bool retain) {
    const bool published = publish(topic, payload, qos, retain);
    if (!published) {
        return false;
    }

    if (telegram_notifier_) {
        notification::TelegramMessage message{
            .topic = topic,
            .payload = payload,
            .qos = qos,
            .retain = retain,
        };
        if (!telegram_notifier_->enqueue(std::move(message))) {
            util::logWarn("Failed to enqueue Telegram event: topic=" + topic);
        }
    }

    return true;
}

bool MqttEventBridge::publishApplicationEvent(const std::string& topic,
                                              const std::string& payload,
                                              const int qos,
                                              const bool retain) {
    return publish(topic, payload, qos, retain);
}

MqttTrackedPublishResult MqttEventBridge::publishTrackedFire(
    const std::string& topic,
    const std::string& payload,
    const bool retain,
    MqttPublishCorrelation correlation,
    const MqttConnectionEpoch expectedEpoch) {
    return endpoint_.publishTracked(
        topic, payload, retain, std::move(correlation), expectedEpoch);
}

}

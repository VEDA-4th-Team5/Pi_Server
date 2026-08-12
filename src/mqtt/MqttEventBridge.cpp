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
#include <stdexcept>
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

int ivaProcessingOrder(const event::CameraEvent& cameraEvent) {
    // 한 PUBLISH에 EXIT와 INTRUSION이 함께 있으면 EXIT 후보를 먼저 만들고
    // 마지막 INTRUSION이 이를 취소하게 하여 점유 상태가 우선하도록 한다.
    if (cameraEvent.action == "EXIT") return 0;
    if (cameraEvent.action == "ENTER") return 1;
    if (cameraEvent.action == "INTRUSION") return 2;
    return 1;
}

}

MqttEventBridge::MqttEventBridge(
    const app::AppConfig& config,
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    database::EventDatabase& database,
    snapshot::SnapshotStorage& snapshot_storage,
    parking::ParkingTriggerCoordinator& trigger_coordinator,
    ocr::OcrWorker& ocr_worker,
    std::vector<parking::ParkingSlotConfig> parking_slot_configs,
    SensorMessageHandler sensor_message_handler,
    FireAckHandler fire_ack_handler,
    IvaOccupancyHandler iva_occupancy_handler,
    notification::TelegramChannelNotifier* telegram_notifier,
    std::unique_ptr<IMqttTransport> transport
)
    : config_(config),
      channels_(channels),
      database_(database),
      snapshot_storage_(snapshot_storage),
      trigger_coordinator_(trigger_coordinator),
      ocr_worker_(ocr_worker),
      parking_slot_configs_(std::move(parking_slot_configs)),
      sensor_message_handler_(std::move(sensor_message_handler)),
      fire_ack_handler_(std::move(fire_ack_handler)),
      iva_occupancy_handler_(std::move(iva_occupancy_handler)),
      telegram_notifier_(telegram_notifier),
      endpoint_(std::move(transport)) {
    if (!endpoint_.bindApplicationHandler(
            [this](const std::string& topic, const std::string& payload) {
                onMessage(topic, payload);
            })) {
        throw std::runtime_error("MQTT application callback could not be bound");
    }

    IMqttTransport::ObserverCallbacks observers;
    observers.onFact = [this](const MqttTransportFact& fact) {
        if (transport_fact_handler_ && !transport_fact_handler_(fact)) {
            util::logError("MQTT transport fact queue rejected an observation");
        }
    };
    if (!endpoint_.bindTransportObservers(std::move(observers))) {
        throw std::runtime_error("MQTT transport observers could not be bound");
    }
}

MqttEventBridge::~MqttEventBridge() {
    if (!stop()) std::terminate();
}

bool MqttEventBridge::bindFireAckHandler(FireAckHandler handler) {
    std::lock_guard lock(lifecycle_mutex_);
    if (endpoint_.state() != MqttEndpointState::Constructed || !handler)
        return false;
    fire_ack_handler_ = std::move(handler);
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

bool MqttEventBridge::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (config_.fire_alarm_enabled && !fire_ack_handler_) {
        util::logError("MQTT start rejected: Fire ACK target is not bound");
        return false;
    }
    std::vector<MqttSubscription> subscriptions;
    // One server-specific client owns camera, Fire command, and event topics.
    if (config_.hall_mqtt_input_enabled &&
        config_.parking_occupancy_source == "HALL") {
        subscriptions.push_back({config_.hall_mqtt_topic, 1});
    }

    if (config_.fire_alarm_enabled && fire_ack_handler_) {
        subscriptions.push_back({config_.fire_command_topic_prefix + "/+", 1});
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
    endpoint_.closeIngress();
}

bool MqttEventBridge::quiesceIngress(const std::chrono::milliseconds timeout) {
    const bool drained = endpoint_.quiesceIngress(timeout);
    if (!drained) util::logError("MQTT application callback drain timed out");
    return drained;
}

bool MqttEventBridge::quiesceTransportObservers(
    const std::chrono::milliseconds timeout) {
    const bool drained = endpoint_.quiesceTransportObservers(timeout);
    if (!drained) util::logError("MQTT transport observer drain timed out");
    return drained;
}

bool MqttEventBridge::abortActiveEpoch(
    const MqttConnectionEpoch expected_epoch) noexcept {
    return endpoint_.abortActiveEpoch(expected_epoch);
}

bool MqttEventBridge::stop(const std::chrono::milliseconds timeout) {
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
        std::stable_sort(camera_events.begin(), camera_events.end(),
                         [](const auto& left, const auto& right) {
                             return ivaProcessingOrder(left) <
                                    ivaProcessingOrder(right);
                         });
    }
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
            event::IvaOccupancySignal signal;
            signal.slotId = target->slotId;
            signal.cameraId = config_.camera_id;
            signal.videoSourceToken = camera_event.video_source_token;
            signal.ruleName = target->ruleName;
            signal.objectId = camera_event.object_id;
            signal.action = iva_action;
            // smart-parking-iva-v1 Publication은 고정 payload라 실제 WiseAI
            // Action과 무관하게 발행될 수 있다. INTRUSION은 보조 입력으로
            // 허용하되 EXIT는 Raw WiseAI만 출차 권한을 갖는다.
            signal.authoritativeExit = !camera_event.is_smart_parking_iva;
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
        if (!trigger_coordinator_.recordCameraIva(target->slotId,
                                                  target->channelId))
            return;
        snapshot::NormalizedRoi roi{target->roiX, target->roiY,
                                    target->roiWidth, target->roiHeight};
        std::string snapshot_path = snapshot_storage_.saveIvaAreaSnapshot(
            channel, target->slotId, roi);
        std::string enhanced_path;
        if (!snapshot_path.empty())
            enhanced_path = ocr::enhanceIvaSceneImage(snapshot_path);
        std::string payload_json = event::EventPayloadBuilder::buildJson(
            config_.camera_id, channel->channel_id, camera_event, snapshot_path);

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
        database_.insertEvent(record);
        if (!enhanced_path.empty()) {
            database_.attachEnhancedPlateImage(snapshot_path, enhanced_path);
            ocr_worker_.enqueueScene(target->slotId, snapshot_path, enhanced_path);
        }
        // IVA snapshot은 입차 구역 증빙과 BestShot 연결에 사용한다.
        // Gemini에는 ROI 원본과 개선본을 함께 보내며, 이후 Plate BestShot OCR이
        // 도착하면 주차 세션의 최종 판독값으로 사용한다.

        std::string qt_topic = config_.qt_event_topic_prefix + "/" +
            config_.camera_id + "/" + channel->channel_id + "/event";
        publish(qt_topic, payload_json, 0, false);
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
    RegularEgressAdmission admission;
    {
        std::lock_guard lock(lifecycle_mutex_);
        admission = regular_egress_admission_;
    }
    if (admission) permit = admission();
    if (admission && (!permit || !permit.isCurrent())) {
        util::logWarn("MQTT regular publish rejected until Fire retained sync: " +
                      topic);
        return false;
    }

    const auto result = permit
        ? endpoint_.publishInEpoch(topic, payload, qos, retain, permit.epoch())
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
    const MqttConnectionEpoch expected_epoch) {
    return endpoint_.publishTracked(
        topic, payload, retain, std::move(correlation), expected_epoch);
}

}

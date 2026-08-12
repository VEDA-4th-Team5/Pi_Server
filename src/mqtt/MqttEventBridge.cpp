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
    notification::TelegramChannelNotifier* telegram_notifier
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
      mosq_(nullptr) {
}

bool MqttEventBridge::start() {
    // libmosquitto 전역 초기화 후 이 서버 전용 client id로 broker에 접속한다.
    mosquitto_lib_init();

    std::string client_id = "pi-server-" + config_.camera_id;

    mosq_ = mosquitto_new(client_id.c_str(), true, this);

    if (!mosq_) {
        util::logError("mosquitto_new failed");
        return false;
    }

    mosquitto_message_callback_set(mosq_, MqttEventBridge::onMessageStatic);

    int rc = mosquitto_connect(
        mosq_,
        config_.mqtt_host.c_str(),
        config_.mqtt_port,
        60
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        util::logError(std::string("MQTT connect failed: ") + mosquitto_strerror(rc));
        return false;
    }

    if (config_.hall_mqtt_input_enabled &&
        config_.parking_occupancy_source == "HALL") {
        rc = mosquitto_subscribe(
            mosq_, nullptr, config_.hall_mqtt_topic.c_str(), 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            util::logError(std::string("Hall MQTT subscribe failed: ") +
                           mosquitto_strerror(rc));
            return false;
        }
    }

    if (config_.fire_alarm_enabled && fire_ack_handler_) {
        const std::string command_filter =
            config_.fire_command_topic_prefix + "/+";
        rc = mosquitto_subscribe(
            mosq_, nullptr, command_filter.c_str(), 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            util::logError(std::string("Fire ACK MQTT subscribe failed: ") +
                           mosquitto_strerror(rc));
            return false;
        }
    }

    rc = mosquitto_subscribe(
        mosq_,
        nullptr,
        config_.mqtt_event_sub_topic.c_str(),
        0
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        util::logError(std::string("MQTT subscribe failed: ") + mosquitto_strerror(rc));
        return false;
    }

    rc = mosquitto_loop_start(mosq_);

    if (rc != MOSQ_ERR_SUCCESS) {
        util::logError(std::string("MQTT loop start failed: ") + mosquitto_strerror(rc));
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

void MqttEventBridge::stop() {
    if (mosq_) {
        mosquitto_loop_stop(mosq_, true);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }

    mosquitto_lib_cleanup();
}

void MqttEventBridge::onMessageStatic(
    mosquitto* mosq,
    void* userdata,
    const mosquitto_message* message
) {
    if (!userdata) {
        return;
    }

    auto* self = static_cast<MqttEventBridge*>(userdata);
    self->onMessage(mosq, message);
}

void MqttEventBridge::onMessage(mosquitto* mosq, const mosquitto_message* message) {
    (void)mosq;

    if (!message || !message->topic) {
        return;
    }

    std::string raw_topic = message->topic;
    std::string raw_payload;

    if (message->payload && message->payloadlen > 0) {
        raw_payload.assign(
            static_cast<const char*>(message->payload),
            message->payloadlen
        );
    }


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
    if (!mosq_) {
        return false;
    }

    int rc = mosquitto_publish(
        mosq_,
        nullptr,
        topic.c_str(),
        static_cast<int>(payload.size()),
        payload.c_str(),
        qos,
        retain
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        util::logError(std::string("MQTT publish failed: ") + mosquitto_strerror(rc));
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

}

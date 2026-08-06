#include "mqtt/MqttEventBridge.hpp"

#include "database/EventDatabase.hpp"
#include "event/CameraEventParser.hpp"
#include "event/EventPayloadBuilder.hpp"
#include "event/IvaEventResolver.hpp"
#include "ocr/PlateImageEnhancer.hpp"
#include "util/Logger.hpp"

#include <nlohmann/json.hpp>

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
    FireAckHandler fire_ack_handler
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

    if (config_.hall_mqtt_input_enabled) {
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

    // 카메라마다 다른 raw topic을 서버 내부의 공통 이벤트 형식으로 정규화한다.
    event::CameraEvent camera_event =
        event::CameraEventParser::parse(
            raw_topic,
            raw_payload,
            config_.default_channel_id
        );

    // IVA Area 이벤트는 모든 채널이 아니라 해당 주차면의 채널/ROI만 증거로 저장한다.
    if (camera_event.is_iva_area_event) {
        // 동일 topic의 false/off 알림은 영역 해제 이벤트이므로 입차 사진을 만들지 않는다.
        if (!camera_event.is_active) {
            util::logInfo("IVA area inactive event ignored: " + raw_topic);
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
    return publish(topic, payload, qos, retain);
}

bool MqttEventBridge::publishApplicationEvent(const std::string& topic,
                                              const std::string& payload,
                                              const int qos,
                                              const bool retain) {
    return publish(topic, payload, qos, retain);
}

}

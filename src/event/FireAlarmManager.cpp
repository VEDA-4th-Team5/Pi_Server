#include "event/FireAlarmManager.hpp"

#include "event/EventPayloadBuilder.hpp"
#include "util/Logger.hpp"

#include <sstream>
#include <utility>

namespace event {
namespace {

// 매핑되지 않은 센서도 토픽 세그먼트가 비지 않도록 이 값으로 보낸다.
constexpr const char* kUnmappedChannelTopic = "unmapped";

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

std::vector<FireSensorBinding> parseFireSensorBindings(
    const std::string& spec) {
    std::vector<FireSensorBinding> bindings;

    std::istringstream stream(spec);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        const auto trimmed = trim(entry);
        if (trimmed.empty()) {
            continue;
        }

        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) {
            util::logWarn("fire sensor mapping ignored (no '='): " + trimmed);
            continue;
        }

        FireSensorBinding binding;
        binding.sensorId = trim(trimmed.substr(0, equals));

        binding.channelId = trim(trimmed.substr(equals + 1));

        if (binding.sensorId.empty() || binding.channelId.empty()) {
            util::logWarn("fire sensor mapping ignored (empty id): " + trimmed);
            continue;
        }

        bindings.push_back(std::move(binding));
    }

    return bindings;
}

FireAlarmManager::FireAlarmManager(
    std::string cameraId,
    std::string defaultChannelId,
    std::string topicPrefix,
    std::vector<FireSensorBinding> bindings,
    Publisher publisher)
    : camera_id_(std::move(cameraId)),
      default_channel_id_(std::move(defaultChannelId)),
      topic_prefix_(std::move(topicPrefix)),
      bindings_(std::move(bindings)),
      publisher_(std::move(publisher)) {
}

std::size_t FireAlarmManager::bindingCount() const {
    return bindings_.size();
}

const FireSensorBinding* FireAlarmManager::findBinding(
    const std::string& sensorId) const {
    for (const auto& binding : bindings_) {
        if (binding.sensorId == sensorId) {
            return &binding;
        }
    }
    return nullptr;
}

bool FireAlarmManager::onFireSignal(const FireSignal& signal) {
    if (signal.sensorId.empty()) {
        return false;
    }

    const FireSensorBinding* binding = findBinding(signal.sensorId);
    std::string channel_id;
    if (binding == nullptr) {
        // 설정 누락으로 화재 신호를 버리지는 않되 임의 채널로 귀속하지 않는다.
        util::logError(
            "fire sensor is not mapped to a channel: " + signal.sensorId);
    } else {
        channel_id = binding->channelId;
    }

    std::string alarm_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = states_[signal.sensorId];

        // 재전송/역순 도착 패킷은 상태를 되돌리면 안 된다.
        if (signal.sourceSequence && state.lastSequence &&
            *signal.sourceSequence <= *state.lastSequence) {
            util::logWarn(
                "fire signal dropped (stale sequence): sensor=" +
                signal.sensorId);
            return false;
        }
        if (signal.sourceSequence) {
            state.lastSequence = signal.sourceSequence;
        }

        if (state.seen && state.detected == signal.detected) {
            return false;  // 상태 변화가 없으면 관제 화면을 흔들지 않는다.
        }

        state.seen = true;
        state.detected = signal.detected;
        state.channelId = channel_id;
        if (signal.detected) {
            state.acknowledged = false;
            state.activeSignal = signal;
            state.activeAlarmId =
                EventPayloadBuilder::buildFireEventId(signal);
        }
        alarm_id = state.activeAlarmId;
        if (!signal.detected) {
            state.acknowledged = false;
            state.activeAlarmId.clear();
        }
    }

    const std::string topic =
        topic_prefix_ + "/" +
        (channel_id.empty() ? kUnmappedChannelTopic : channel_id);
    const std::string event_id =
        EventPayloadBuilder::buildFireEventId(signal);
    if (alarm_id.empty()) alarm_id = event_id;
    const std::string payload = EventPayloadBuilder::buildFireJson(
        camera_id_, channel_id, signal,
        signal.detected ? FireAlarmLifecycle::Open
                        : FireAlarmLifecycle::Resolved,
        event_id, alarm_id);

    if (!publisher_ || !publisher_(topic, payload)) {
        util::logError("fire alarm publish failed: " + topic);
        return false;
    }

    util::logLine(
        "FIRE_ALARM",
        std::string(signal.detected ? "SUSPECTED" : "CLEARED") +
            " sensor=" + signal.sensorId +
            " channel=" + (channel_id.empty() ? "-" : channel_id) +
            " topic=" + topic +
            " raw=" + signal.rawPayload);

    return true;
}

bool FireAlarmManager::acknowledge(const std::string& channelId,
                                   const std::string& alarmId) {
    if (channelId.empty() || alarmId.empty()) {
        util::logWarn("fire ACK rejected: channel_id and event_id are required");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [sensor_id, state] : states_) {
        if (!state.detected || state.channelId != channelId ||
            state.activeAlarmId != alarmId) {
            continue;
        }
        if (state.acknowledged) {
            util::logInfo("duplicate fire ACK ignored: channel=" + channelId +
                          " alarm_id=" + alarmId);
            return true;
        }

        FireSignal ack_signal = state.activeSignal;
        ack_signal.occurredAt = std::chrono::system_clock::now();
        const std::string topic = topic_prefix_ + "/" + channelId;
        const std::string payload = EventPayloadBuilder::buildFireJson(
            camera_id_, channelId, ack_signal,
            FireAlarmLifecycle::Acknowledged, alarmId + "-ack", alarmId);
        if (!publisher_ || !publisher_(topic, payload)) {
            util::logError("fire ACK publish failed: " + topic);
            return false;
        }

        state.acknowledged = true;
        util::logLine("FIRE_ALARM",
                      "ACKNOWLEDGED sensor=" + sensor_id +
                          " channel=" + channelId +
                          " alarm_id=" + alarmId);
        return true;
    }

    util::logWarn("fire ACK rejected: active alarm not found channel=" +
                  channelId + " event_id=" + alarmId);
    return false;
}

}  // namespace event

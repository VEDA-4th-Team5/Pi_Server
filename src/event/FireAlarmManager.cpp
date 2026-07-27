#include "event/FireAlarmManager.hpp"

#include "event/EventPayloadBuilder.hpp"
#include "util/Logger.hpp"

#include <sstream>
#include <utility>

namespace event {
namespace {

// 매핑되지 않은 센서도 토픽 세그먼트가 비지 않도록 이 값으로 보낸다.
constexpr const char* kUnmappedSlotTopic = "unmapped";

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

        auto target = trim(trimmed.substr(equals + 1));
        const auto colon = target.find(':');
        if (colon == std::string::npos) {
            binding.slotId = target;
        } else {
            binding.slotId = trim(target.substr(0, colon));
            binding.channelId = trim(target.substr(colon + 1));
        }

        if (binding.sensorId.empty() || binding.slotId.empty()) {
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
    }

    const FireSensorBinding* binding = findBinding(signal.sensorId);
    std::string slot_id;
    std::string channel_id = default_channel_id_;

    if (binding == nullptr) {
        // 설정 누락으로 화재 신호를 버리지는 않는다. 주차면 없이라도 올린다.
        util::logError(
            "fire sensor is not mapped to a slot: " + signal.sensorId);
    } else {
        slot_id = binding->slotId;
        if (!binding->channelId.empty()) {
            channel_id = binding->channelId;
        }
    }

    const std::string topic =
        topic_prefix_ + "/" + (slot_id.empty() ? kUnmappedSlotTopic : slot_id);
    const std::string payload = EventPayloadBuilder::buildFireJson(
        camera_id_, channel_id, slot_id, signal);

    if (!publisher_ || !publisher_(topic, payload)) {
        util::logError("fire alarm publish failed: " + topic);
        return false;
    }

    util::logLine(
        "FIRE_ALARM",
        std::string(signal.detected ? "SUSPECTED" : "CLEARED") +
            " sensor=" + signal.sensorId +
            " slot=" + (slot_id.empty() ? "-" : slot_id) +
            " topic=" + topic +
            " raw=" + signal.rawPayload);

    return true;
}

}  // namespace event

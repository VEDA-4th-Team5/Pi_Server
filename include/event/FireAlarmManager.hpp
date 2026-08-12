#pragma once

#include "event/FireAlarmEvent.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace event {

struct FireSensorBinding {
    std::string sensorId;
    std::string channelId;
};

[[nodiscard]] std::vector<FireSensorBinding> parseFireSensorBindings(
    const std::string& spec);

// Compatibility publisher used by the current main.cpp runtime until the
// FireAlarmService/FireDeliveryCoordinator wiring replaces this path.
class FireAlarmManager {
public:
    using Publisher =
        std::function<bool(const std::string& topic,
                           const std::string& payload)>;

    FireAlarmManager(
        std::string cameraId,
        std::string defaultChannelId,
        std::string topicPrefix,
        std::vector<FireSensorBinding> bindings,
        Publisher publisher);

    bool onFireSignal(const FireSignal& signal);
    bool acknowledge(const std::string& channelId,
                     const std::string& alarmId);
    [[nodiscard]] std::size_t bindingCount() const;

private:
    struct SensorState {
        bool detected{false};
        bool seen{false};
        bool acknowledged{false};
        std::optional<std::uint64_t> lastSequence;
        std::uint64_t fireRevision{};
        std::string channelId;
        std::string activeAlarmId;
        FireSignal activeSignal;
    };

    const FireSensorBinding* findBinding(const std::string& sensorId) const;

    std::string camera_id_;
    std::string default_channel_id_;
    std::string topic_prefix_;
    std::vector<FireSensorBinding> bindings_;
    Publisher publisher_;

    mutable std::mutex mutex_;
    std::map<std::string, SensorState> states_;
};

}  // namespace event

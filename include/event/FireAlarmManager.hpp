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
    std::string slotId;
    std::string channelId;  // 비면 기본 채널을 쓴다.
};

// "FIRE01=EV01:ch01,FIRE02=EV02" 형식을 파싱한다. channel 부분은 생략 가능.
[[nodiscard]] std::vector<FireSensorBinding> parseFireSensorBindings(
    const std::string& spec);

// 화재 후보 신호를 관제실로 올려보내는 Core 계층 정책이다.
//
// 여기서 하지 않는 일: 화재를 확정하지 않고, 자동 조치를 하지 않으며,
// 카메라 교차검증 결과를 기다리지 않는다. 최종 판단은 관제실(사람)의 몫이다.
// 이 클래스는 후보 이벤트와 근거만 전달하고, 중복 신호를 줄인다.
class FireAlarmManager {
public:
    // 하위 계층(MQTT)을 직접 알지 않도록 발행은 콜백으로 주입받는다.
    using Publisher =
        std::function<bool(const std::string& topic,
                           const std::string& payload)>;

    FireAlarmManager(
        std::string cameraId,
        std::string defaultChannelId,
        std::string topicPrefix,
        std::vector<FireSensorBinding> bindings,
        Publisher publisher);

    // 같은 상태가 반복되면 발행하지 않는다(STM32 디바운싱 이후의 2차 방어).
    // 상태가 바뀐 경우에만 true 를 돌려준다.
    bool onFireSignal(const FireSignal& signal);

    [[nodiscard]] std::size_t bindingCount() const;

private:
    struct SensorState {
        bool detected{false};
        bool seen{false};
        std::optional<std::uint64_t> lastSequence;
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

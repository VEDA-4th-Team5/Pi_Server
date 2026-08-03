#pragma once

#include "parking/ParkingSensorEvent.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "sensor/SensorProtocolMessage.hpp"

#include <optional>
#include <string>

namespace sensor {

/** @brief transport-neutral 센서 메시지에 설정 기반 slot_id를 부여한다. */
class ParkingSensorEventAdapter {
public:
    explicit ParkingSensorEventAdapter(
        const parking::SensorSlotIndex& slotIndex);

    /** @brief 미등록 sensor_id면 nullopt와 선택적 오류 문자열을 반환한다. */
    [[nodiscard]] std::optional<parking::ParkingSensorEvent> adapt(
        const SensorProtocolMessage& message,
        std::string* error = nullptr) const;

private:
    const parking::SensorSlotIndex& slotIndex_;
};

}  // namespace sensor

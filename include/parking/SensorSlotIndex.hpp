#pragma once

#include "parking/ParkingSlotConfig.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace parking {

struct SensorSlotMatch {
    std::string slotId;
    std::string sensorId;
};

class SensorSlotIndex {
public:
    explicit SensorSlotIndex(
        const std::vector<ParkingSlotConfig>& configs);

    [[nodiscard]] std::optional<SensorSlotMatch> findBySensorId(
        const std::string& sensorId) const;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, SensorSlotMatch> bySensorId_;
};

}  // namespace parking

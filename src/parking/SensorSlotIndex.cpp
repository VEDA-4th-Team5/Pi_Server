#include "parking/SensorSlotIndex.hpp"

#include <stdexcept>

namespace parking {

SensorSlotIndex::SensorSlotIndex(
    const std::vector<ParkingSlotConfig>& configs) {
    for (const auto& config : configs) {
        if (!config.enabled) {
            continue;
        }
        if (config.sensorId.empty()) {
            throw std::invalid_argument(
                "enabled slot has an empty sensor id: " +
                config.slotId);
        }

        const auto inserted = bySensorId_.emplace(
            config.sensorId,
            SensorSlotMatch{
                config.slotId,
                config.sensorId
            });

        if (!inserted.second) {
            throw std::invalid_argument(
                "duplicate enabled sensor id: " +
                config.sensorId);
        }
    }
}

std::optional<SensorSlotMatch> SensorSlotIndex::findBySensorId(
    const std::string& sensorId) const {
    const auto it = bySensorId_.find(sensorId);
    if (it == bySensorId_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::size_t SensorSlotIndex::size() const noexcept {
    return bySensorId_.size();
}

}  // namespace parking

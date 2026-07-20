#pragma once

#include "parking/ParkingOccupancySession.hpp"
#include "parking/ParkingSlotConfig.hpp"

#include <optional>
#include <utility>

namespace parking {

struct ParkingSlot {
    explicit ParkingSlot(ParkingSlotConfig value)
        : config(std::move(value)) {}

    [[nodiscard]] bool occupied() const noexcept {
        return activeSession.has_value();
    }

    ParkingSlotConfig config;
    std::optional<ParkingOccupancySession> activeSession;
};

}  // namespace parking

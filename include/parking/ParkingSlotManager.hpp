#pragma once

#include "parking/ParkingSensorEvent.hpp"
#include "parking/ParkingSlot.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace parking {

enum class ParkingTransitionCode {
    SessionStarted,
    SessionCompleted,
    DuplicateOccupiedIgnored,
    DuplicateVacantIgnored,
    UnknownSlot,
    DisabledSlot,
    SensorMismatch,
    InvalidTimestamp
};

struct ParkingTransitionResult {
    ParkingTransitionCode code{
        ParkingTransitionCode::UnknownSlot};
    std::string slotId;
    std::string sessionId;
    std::string message;
    std::optional<ParkingOccupancySession> session;

    [[nodiscard]] bool changed() const noexcept {
        return code == ParkingTransitionCode::SessionStarted ||
               code == ParkingTransitionCode::SessionCompleted;
    }
};

class ParkingSlotManager {
public:
    explicit ParkingSlotManager(
        std::vector<ParkingSlotConfig> configs);

    [[nodiscard]] ParkingTransitionResult handle(
        const ParkingSensorEvent& event);

    [[nodiscard]] const ParkingSlot* findSlot(
        const std::string& slotId) const noexcept;

    [[nodiscard]] std::size_t slotCount() const noexcept;
    [[nodiscard]] std::size_t activeSlotCount() const noexcept;

private:
    [[nodiscard]] static std::string createSessionId(
        const std::string& slotId,
        std::chrono::system_clock::time_point startedAt);

    std::unordered_map<std::string, ParkingSlot> slots_;
};

[[nodiscard]] const char* toString(
    ParkingTransitionCode code) noexcept;

}  // namespace parking

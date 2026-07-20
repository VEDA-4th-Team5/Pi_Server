#pragma once

#include "parking/ParkingSlotManager.hpp"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace parking {

// Thread-safe read model of currently active ParkingOccupancySession IDs.
// ParkingSlotManager remains the aggregate owner. This index is updated from
// ParkingSessionWorker::TransitionSink and is queried by IVA workers.
class ActiveParkingSessionIndex {
public:
    void apply(const ParkingTransitionResult& transition);

    void setActive(
        const std::string& slotId,
        const std::string& parkingSessionId);

    void clearActive(
        const std::string& slotId,
        const std::string& expectedParkingSessionId = {});

    [[nodiscard]] std::optional<std::string> findActiveSessionId(
        const std::string& slotId) const;

    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> bySlotId_;
};

}  // namespace parking

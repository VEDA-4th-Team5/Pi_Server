#include "parking/ParkingOccupancyConfirmationGate.hpp"

namespace parking {

ParkingOccupancyConfirmationGate::ParkingOccupancyConfirmationGate(
    const std::chrono::milliseconds confirmThreshold)
    : confirmThreshold_(confirmThreshold) {}

ParkingOccupancyConfirmationGate::Decision
ParkingOccupancyConfirmationGate::evaluate(
    const ParkingSensorEvent& event, const bool slotAlreadyOccupied) {
    if (event.state == ParkingSensorState::Occupied) {
        if (slotAlreadyOccupied) return Decision::Forward;

        const auto found = pendingBySlot_.find(event.slotId);
        if (found == pendingBySlot_.end()) {
            pendingBySlot_.emplace(
                event.slotId,
                Pending{event, event.receivedMonotonic + confirmThreshold_});
            return Decision::Suppress;
        }

        if (event.receivedMonotonic >= found->second.deadline) {
            pendingBySlot_.erase(found);
            return Decision::Forward;
        }
        return Decision::Suppress;
    }

    if (!slotAlreadyOccupied) {
        pendingBySlot_.erase(event.slotId);
        return Decision::Suppress;
    }
    return Decision::Forward;
}

std::optional<std::chrono::steady_clock::time_point>
ParkingOccupancyConfirmationGate::nextDeadline() const {
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& [slotId, pending] : pendingBySlot_) {
        (void)slotId;
        if (!earliest || pending.deadline < *earliest)
            earliest = pending.deadline;
    }
    return earliest;
}

std::vector<ParkingSensorEvent>
ParkingOccupancyConfirmationGate::takeDue(
    const std::chrono::steady_clock::time_point monotonicNow,
    const std::chrono::system_clock::time_point wallNow) {
    std::vector<ParkingSensorEvent> due;
    for (auto it = pendingBySlot_.begin(); it != pendingBySlot_.end();) {
        if (it->second.deadline > monotonicNow) {
            ++it;
            continue;
        }
        ParkingSensorEvent event = it->second.firstEvent;
        event.occurredAt = wallNow;
        event.receivedMonotonic = monotonicNow;
        due.push_back(std::move(event));
        it = pendingBySlot_.erase(it);
    }
    return due;
}

}  // namespace parking

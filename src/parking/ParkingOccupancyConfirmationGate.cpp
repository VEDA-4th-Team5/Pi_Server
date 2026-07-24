#include "parking/ParkingOccupancyConfirmationGate.hpp"

namespace parking {

ParkingOccupancyConfirmationGate::ParkingOccupancyConfirmationGate(
    std::chrono::milliseconds confirmThreshold)
    : confirmThreshold_(confirmThreshold) {
}

ParkingOccupancyConfirmationGate::Decision
ParkingOccupancyConfirmationGate::evaluate(
    const ParkingSensorEvent& event, bool slotAlreadyOccupied) {
    if (event.state == ParkingSensorState::Occupied) {
        if (slotAlreadyOccupied) {
            // Already confirmed; this is just a repeated poll heartbeat.
            // ParkingSlotManager collapses it to DuplicateOccupiedIgnored.
            return Decision::Forward;
        }

        const auto found = pendingBySlot_.find(event.slotId);
        if (found == pendingBySlot_.end()) {
            // First sighting of this occupancy attempt: start the clock but
            // do not start a session yet.
            pendingBySlot_.emplace(event.slotId, Pending{event.occurredAt});
            return Decision::Suppress;
        }

        if (event.occurredAt - found->second.firstSeenAt >=
            confirmThreshold_) {
            // Held long enough: confirm now. T0 is this event's own
            // timestamp — the instant Pi actually knows it is real.
            pendingBySlot_.erase(found);
            return Decision::Forward;
        }
        return Decision::Suppress;  // still within the confirmation window
    }

    // Vacant: an already-confirmed slot has a real departure to complete.
    // An unconfirmed one was only ever a candidate — discard it, nothing
    // was ever started so there is nothing to complete.
    if (!slotAlreadyOccupied) {
        pendingBySlot_.erase(event.slotId);
        return Decision::Suppress;
    }
    return Decision::Forward;
}

}  // namespace parking

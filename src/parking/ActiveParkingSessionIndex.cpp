#include "parking/ActiveParkingSessionIndex.hpp"

#include <mutex>
#include <utility>

namespace parking {

void ActiveParkingSessionIndex::apply(
    const ParkingTransitionResult& transition) {
    switch (transition.code) {
    case ParkingTransitionCode::SessionStarted:
        if (!transition.slotId.empty() &&
            !transition.sessionId.empty()) {
            setActive(
                transition.slotId,
                transition.sessionId);
        }
        break;

    case ParkingTransitionCode::SessionCompleted:
        if (!transition.slotId.empty()) {
            clearActive(
                transition.slotId,
                transition.sessionId);
        }
        break;

    default:
        break;
    }
}

void ActiveParkingSessionIndex::setActive(
    const std::string& slotId,
    const std::string& parkingSessionId) {
    if (slotId.empty() || parkingSessionId.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    bySlotId_[slotId] = parkingSessionId;
}

void ActiveParkingSessionIndex::clearActive(
    const std::string& slotId,
    const std::string& expectedParkingSessionId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    const auto found = bySlotId_.find(slotId);
    if (found == bySlotId_.end()) {
        return;
    }

    if (!expectedParkingSessionId.empty() &&
        found->second != expectedParkingSessionId) {
        return;
    }

    bySlotId_.erase(found);
}

std::optional<std::string>
ActiveParkingSessionIndex::findActiveSessionId(
    const std::string& slotId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const auto found = bySlotId_.find(slotId);
    if (found == bySlotId_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t ActiveParkingSessionIndex::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return bySlotId_.size();
}

void ActiveParkingSessionIndex::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    bySlotId_.clear();
}

}  // namespace parking

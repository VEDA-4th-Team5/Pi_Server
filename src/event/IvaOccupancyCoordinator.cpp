#include "event/IvaOccupancyCoordinator.hpp"

#include <algorithm>
#include <utility>

namespace event {

IvaOccupancyCoordinator::IvaOccupancyCoordinator(
    const std::vector<parking::ParkingSlotConfig>& slots,
    const std::chrono::milliseconds exitConfirmDelay)
    : exitConfirmDelay_(std::max(std::chrono::milliseconds(1),
                                 exitConfirmDelay)),
      aggregator_(slots) {}

IvaCoordinationResult IvaOccupancyCoordinator::handle(
    IvaOccupancySignal signal,
    const std::chrono::steady_clock::time_point now) {
    IvaCoordinationResult result;
    result.signal = signal;

    if (signal.action == IvaOccupancyAction::Enter) {
        result.code = IvaCoordinationCode::IgnoredEnter;
        return result;
    }
    if (signal.action == IvaOccupancyAction::Exit &&
        !signal.authoritativeExit) {
        result.code = IvaCoordinationCode::IgnoredCustomExit;
        return result;
    }
    if (signal.action != IvaOccupancyAction::Intrusion &&
        signal.action != IvaOccupancyAction::Exit) {
        result.code = IvaCoordinationCode::Unsupported;
        return result;
    }

    const bool active = signal.action == IvaOccupancyAction::Intrusion;
    std::lock_guard lock(mutex_);
    const auto aggregate = aggregator_.update(
        signal.slotId, signal.cameraId, signal.videoSourceToken,
        signal.ruleName, active);

    if (signal.action == IvaOccupancyAction::Intrusion) {
        const bool canceled = pendingBySlot_.erase(signal.slotId) != 0;
        if (aggregate) {
            result.activeAreaCount = aggregate->activeAreaCount;
            result.knownAreaCount = aggregate->knownAreaCount;
            result.configuredAreaCount = aggregate->configuredAreaCount;
            result.code = canceled ? IvaCoordinationCode::ExitCanceled
                                   : IvaCoordinationCode::Occupied;
        } else {
            result.code = canceled ? IvaCoordinationCode::ExitCanceled
                                   : IvaCoordinationCode::Duplicate;
        }
        return result;
    }

    if (!aggregate || aggregate->occupied) {
        result.code = IvaCoordinationCode::Duplicate;
        return result;
    }
    result.activeAreaCount = aggregate->activeAreaCount;
    result.knownAreaCount = aggregate->knownAreaCount;
    result.configuredAreaCount = aggregate->configuredAreaCount;
    if (pendingBySlot_.contains(signal.slotId)) {
        result.code = IvaCoordinationCode::Duplicate;
        return result;
    }
    pendingBySlot_.emplace(
        signal.slotId, PendingExit{signal, now + exitConfirmDelay_});
    result.code = IvaCoordinationCode::ExitPending;
    return result;
}

std::optional<std::chrono::steady_clock::time_point>
IvaOccupancyCoordinator::nextDeadline() const {
    std::lock_guard lock(mutex_);
    if (pendingBySlot_.empty()) return std::nullopt;
    return std::min_element(
               pendingBySlot_.begin(), pendingBySlot_.end(),
               [](const auto& left, const auto& right) {
                   return left.second.deadline < right.second.deadline;
               })
        ->second.deadline;
}

std::vector<IvaOccupancySignal> IvaOccupancyCoordinator::takeDue(
    const std::chrono::steady_clock::time_point now) {
    std::vector<IvaOccupancySignal> due;
    std::lock_guard lock(mutex_);
    for (auto it = pendingBySlot_.begin(); it != pendingBySlot_.end();) {
        if (it->second.deadline > now) {
            ++it;
            continue;
        }
        due.push_back(std::move(it->second.signal));
        it = pendingBySlot_.erase(it);
    }
    return due;
}

}  // namespace event

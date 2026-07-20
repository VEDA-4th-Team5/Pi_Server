#include "parking/ParkingOccupancySession.hpp"

#include <stdexcept>
#include <utility>

namespace parking {

ParkingOccupancySession::ParkingOccupancySession(
    std::string sessionId,
    std::string slotId,
    std::string sensorId,
    std::chrono::system_clock::time_point startedAt)
    : sessionId_(std::move(sessionId)),
      slotId_(std::move(slotId)),
      sensorId_(std::move(sensorId)),
      startedAt_(startedAt) {
    if (sessionId_.empty()) {
        throw std::invalid_argument("parking session id is empty");
    }
    if (slotId_.empty()) {
        throw std::invalid_argument("parking session slot id is empty");
    }
    if (sensorId_.empty()) {
        throw std::invalid_argument("parking session sensor id is empty");
    }
}

const std::string&
ParkingOccupancySession::sessionId() const noexcept {
    return sessionId_;
}

const std::string&
ParkingOccupancySession::slotId() const noexcept {
    return slotId_;
}

const std::string&
ParkingOccupancySession::sensorId() const noexcept {
    return sensorId_;
}

ParkingSessionState
ParkingOccupancySession::state() const noexcept {
    return state_;
}

bool ParkingOccupancySession::active() const noexcept {
    return state_ == ParkingSessionState::Active;
}

std::chrono::system_clock::time_point
ParkingOccupancySession::startedAt() const noexcept {
    return startedAt_;
}

const std::optional<std::chrono::system_clock::time_point>&
ParkingOccupancySession::endedAt() const noexcept {
    return endedAt_;
}

void ParkingOccupancySession::complete(
    std::chrono::system_clock::time_point endedAt) {
    if (!active()) {
        throw std::logic_error(
            "parking session is already completed");
    }
    if (endedAt < startedAt_) {
        throw std::invalid_argument(
            "parking session end time precedes start time");
    }

    endedAt_ = endedAt;
    state_ = ParkingSessionState::Completed;
}

}  // namespace parking

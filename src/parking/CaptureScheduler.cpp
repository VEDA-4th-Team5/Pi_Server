#include "parking/CaptureScheduler.hpp"

#include <algorithm>
#include <utility>

namespace parking {

CaptureScheduler::CaptureScheduler(CaptureSchedulerConfig config,
                                   CaptureTargetResolver resolver)
    : config_(std::move(config)), resolver_(std::move(resolver)) {
}

CaptureScheduler::ScheduleReport CaptureScheduler::onTransition(
    const ParkingTransitionResult& transition) {
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduleReport report;

    if (transition.code == ParkingTransitionCode::SessionCompleted) {
        // The car left before every capture fired: drop the remaining ones.
        // Nothing to shoot in an empty slot, and this bounds memory.
        sessions_.erase(transition.sessionId);
        return report;
    }

    if (transition.code != ParkingTransitionCode::SessionStarted ||
        !transition.session.has_value()) {
        return report;
    }

    // Dedup: one schedule per session id. A repeated OCCUPIED poll never
    // reaches here (it collapses to DuplicateOccupiedIgnored upstream), but we
    // guard anyway so the same session is never double-booked.
    if (sessions_.find(transition.sessionId) != sessions_.end()) {
        report.duplicate = true;
        return report;
    }

    const std::string& slotId = transition.slotId;
    std::optional<CaptureTarget> target =
        resolver_ ? resolver_(slotId) : std::nullopt;
    if (!target.has_value()) {
        report.unmappedSlot = true;
        return report;
    }

    const ParkingOccupancySession& occupancy = *transition.session;
    SessionState state;
    state.slotId = slotId;
    state.sensorId = occupancy.sensorId();
    state.startedAt = occupancy.startedAt();
    state.target = std::move(*target);

    for (const auto& offset : config_.offsets) {
        CaptureState capture;
        capture.reason = offset.reason;
        capture.phase = Phase::Pending;
        capture.attempt = 1;
        capture.scheduledFor = state.startedAt + offset.delay;
        state.captures.push_back(capture);
    }

    report.scheduled = static_cast<int>(state.captures.size());
    sessions_.emplace(transition.sessionId, std::move(state));
    return report;
}

std::vector<CaptureRequest> CaptureScheduler::due(
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CaptureRequest> ready;

    for (auto& [sessionId, session] : sessions_) {
        for (auto& capture : session.captures) {
            if (capture.phase != Phase::Pending ||
                capture.scheduledFor > now) {
                continue;
            }
            capture.phase = Phase::AwaitingResponse;
            ready.push_back(buildRequest(sessionId, session, capture));
        }
    }
    return ready;
}

DispatchOutcome CaptureScheduler::onDispatchResult(
    const CaptureRequest& request,
    bool accepted,
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto sessionIt = sessions_.find(request.sessionId);
    if (sessionIt == sessions_.end()) {
        return DispatchOutcome::Unknown;  // session already completed/cancelled
    }

    SessionState& session = sessionIt->second;
    auto captureIt = std::find_if(
        session.captures.begin(), session.captures.end(),
        [&request](const CaptureState& c) { return c.reason == request.reason; });
    if (captureIt == session.captures.end() ||
        captureIt->phase != Phase::AwaitingResponse) {
        return DispatchOutcome::Unknown;  // stale/duplicate result
    }

    DispatchOutcome outcome;
    if (accepted) {
        captureIt->phase = Phase::Done;
        outcome = DispatchOutcome::Done;
    } else if (captureIt->attempt < 1 + config_.maxRetries) {
        // No response within the timeout: retry after retryInterval.
        captureIt->attempt += 1;
        captureIt->phase = Phase::Pending;
        captureIt->scheduledFor = now + config_.retryInterval;
        outcome = DispatchOutcome::WillRetry;
    } else {
        captureIt->phase = Phase::Done;  // give up; never blocks the session
        outcome = DispatchOutcome::GaveUp;
    }

    // Free the session once every capture is settled so state stays bounded.
    const bool allSettled = std::all_of(
        session.captures.begin(), session.captures.end(),
        [](const CaptureState& c) { return c.phase == Phase::Done; });
    if (allSettled) {
        sessions_.erase(sessionIt);
    }
    return outcome;
}

std::optional<std::chrono::system_clock::time_point>
CaptureScheduler::nextDeadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::chrono::system_clock::time_point> earliest;

    for (const auto& [sessionId, session] : sessions_) {
        (void)sessionId;
        for (const auto& capture : session.captures) {
            if (capture.phase != Phase::Pending) {
                continue;
            }
            if (!earliest.has_value() || capture.scheduledFor < *earliest) {
                earliest = capture.scheduledFor;
            }
        }
    }
    return earliest;
}

std::size_t CaptureScheduler::trackedSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

CaptureRequest CaptureScheduler::buildRequest(
    const std::string& sessionId,
    const SessionState& session,
    const CaptureState& capture) const {
    CaptureRequest request;
    request.sessionId = sessionId;
    request.slotId = session.slotId;
    request.sensorId = session.sensorId;
    request.target = session.target;
    request.reason = capture.reason;
    request.attempt = capture.attempt;
    request.sessionStartedAt = session.startedAt;
    request.scheduledFor = capture.scheduledFor;
    request.responseTimeout = config_.responseTimeout;
    return request;
}

}  // namespace parking

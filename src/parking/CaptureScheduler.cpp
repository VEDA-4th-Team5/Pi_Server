#include "parking/CaptureScheduler.hpp"

#include <algorithm>
#include <utility>

namespace parking {

CaptureScheduler::CaptureScheduler(CaptureSchedulerConfig config,
                                   CaptureTargetResolver resolver)
    : config_(std::move(config)), resolver_(std::move(resolver)) {}

CaptureScheduler::ScheduleReport CaptureScheduler::onTransition(
    const ParkingTransitionResult& transition) {
    std::lock_guard lock(mutex_);
    ScheduleReport report;

    if (transition.code == ParkingTransitionCode::SessionCompleted) {
        sessions_.erase(transition.sessionId);
        return report;
    }
    if (transition.code != ParkingTransitionCode::SessionStarted ||
        !transition.session) {
        return report;
    }
    if (sessions_.contains(transition.sessionId)) {
        report.duplicate = true;
        return report;
    }

    auto target = resolver_ ? resolver_(transition.slotId) : std::nullopt;
    if (!target) {
        report.unmappedSlot = true;
        return report;
    }

    const auto& occupancy = *transition.session;
    SessionState state;
    state.slotId = transition.slotId;
    state.sensorId = occupancy.sensorId();
    state.startedAt = occupancy.startedAt();
    state.startedAtMonotonic = occupancy.startedAtMonotonic();
    state.target = std::move(*target);
    for (const auto& offset : config_.offsets) {
        state.captures.push_back({
            offset.reason,
            Phase::Pending,
            1,
            state.startedAtMonotonic + offset.delay});
    }
    report.scheduled = static_cast<int>(state.captures.size());
    sessions_.emplace(transition.sessionId, std::move(state));
    return report;
}

std::vector<CaptureRequest> CaptureScheduler::due(
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::vector<CaptureRequest> ready;
    for (auto& [sessionId, session] : sessions_) {
        for (auto& capture : session.captures) {
            if (capture.phase != Phase::Pending || capture.scheduledFor > now)
                continue;
            capture.phase = Phase::Dispatching;
            ready.push_back(buildRequest(sessionId, session, capture));
        }
    }
    return ready;
}

DispatchOutcome CaptureScheduler::onDispatchResult(
    const CaptureRequest& request, const bool published,
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto sessionIt = sessions_.find(request.sessionId);
    if (sessionIt == sessions_.end()) return DispatchOutcome::Unknown;

    auto& captures = sessionIt->second.captures;
    const auto captureIt = std::find_if(
        captures.begin(), captures.end(),
        [&request](const CaptureState& state) {
            return state.reason == request.reason;
        });
    if (captureIt == captures.end() ||
        captureIt->phase != Phase::Dispatching) {
        return DispatchOutcome::Unknown;
    }

    DispatchOutcome outcome;
    if (published) {
        captureIt->phase = Phase::Done;
        outcome = DispatchOutcome::Done;
    } else if (captureIt->attempt < 1 + config_.maxRetries) {
        ++captureIt->attempt;
        captureIt->phase = Phase::Pending;
        captureIt->scheduledFor = now + config_.retryInterval;
        outcome = DispatchOutcome::WillRetry;
    } else {
        captureIt->phase = Phase::Done;
        outcome = DispatchOutcome::GaveUp;
    }

    const bool settled = std::all_of(
        captures.begin(), captures.end(),
        [](const CaptureState& state) { return state.phase == Phase::Done; });
    if (settled) sessions_.erase(sessionIt);
    return outcome;
}

std::optional<std::chrono::steady_clock::time_point>
CaptureScheduler::nextDeadline() const {
    std::lock_guard lock(mutex_);
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& [sessionId, session] : sessions_) {
        (void)sessionId;
        for (const auto& capture : session.captures) {
            if (capture.phase != Phase::Pending) continue;
            if (!earliest || capture.scheduledFor < *earliest)
                earliest = capture.scheduledFor;
        }
    }
    return earliest;
}

std::size_t CaptureScheduler::trackedSessions() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

CaptureRequest CaptureScheduler::buildRequest(
    const std::string& sessionId, const SessionState& session,
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

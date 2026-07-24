#pragma once

#include "parking/CaptureRequest.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace parking {

// One (reason, delay) pair to schedule after T0. Defaults are the T0+30s /
// T0+60s captures agreed for the hall-occupancy path (EVDA-135).
struct CaptureOffset {
    CaptureReason reason;
    std::chrono::seconds delay;
};

struct CaptureSchedulerConfig {
    std::vector<CaptureOffset> offsets{
        {CaptureReason::HallOccupied30s, std::chrono::seconds(30)},
        {CaptureReason::HallOccupied60s, std::chrono::seconds(60)}};

    // Isolation policy (AC): a capture that gets no response within
    // responseTimeout is retried at retryInterval, at most maxRetries times.
    // A failing capture never blocks the session or the 1-hour timer; it only
    // reschedules itself here.
    std::chrono::milliseconds responseTimeout{3000};
    std::chrono::milliseconds retryInterval{2000};
    int maxRetries{2};
};

// What happened to a dispatched request once its result came back.
enum class DispatchOutcome {
    Done,      // accepted, no further work
    WillRetry, // failed but rescheduled
    GaveUp,    // failed and retries exhausted
    Unknown    // request no longer tracked (session ended / stale)
};

// L1 Core scheduling policy for the T0+30s / T0+60s captures.
//
// Pure and deterministic: it owns no thread, no clock, and no I/O. The caller
// supplies "now" to due()/onDispatchResult(), so the whole retry/dedup policy
// is unit-testable without sleeping. CaptureSchedulerRuntime drives it from a
// timer thread and performs the actual publish through an injected callback,
// keeping MQTT out of Core (same boundary style as FireAlarmManager).
//
// All deadline math (scheduledFor, due(), onDispatchResult(), nextDeadline())
// runs on steady_clock, not system_clock: a Pi without an RTC can have its
// wall clock jump when NTP syncs while pi-server is already running, which
// would otherwise fire captures early/late or never. The session's T0
// (ParkingOccupancySession::startedAt, system_clock) is still what gets
// reported as CaptureRequest::sessionStartedAt for logs/MQTT -- only the
// "when is this due" bookkeeping is monotonic. See
// ParkingOccupancyConfirmationGate.hpp for the same split applied to the
// confirm-gate hold-time check.
//
// Thread-safety: every public method is mutex-guarded so the session worker
// thread (onTransition) and the runtime timer thread (due/onDispatchResult)
// can call concurrently.
class CaptureScheduler {
public:
    CaptureScheduler(CaptureSchedulerConfig config,
                     CaptureTargetResolver resolver);

    struct ScheduleReport {
        int scheduled{0};        // captures newly queued for this transition
        bool unmappedSlot{false};// SessionStarted but the slot has no target
        bool duplicate{false};   // session already scheduled (no double-book)
    };

    // Feed one slot-state-machine transition. On SessionStarted it queues the
    // configured offsets once per session; on SessionCompleted it cancels any
    // not-yet-sent captures for that session (an emptied slot has no plate to
    // shoot). Fast and non-blocking so it is safe as a worker sink.
    ScheduleReport onTransition(const ParkingTransitionResult& transition);

    // Every capture whose scheduledFor <= now and that is ready to send. Each
    // returned request is marked awaiting-response and will not surface again
    // until onDispatchResult() reports back.
    [[nodiscard]] std::vector<CaptureRequest> due(
        std::chrono::steady_clock::time_point now);

    // Report the dispatch result for a request previously returned by due().
    DispatchOutcome onDispatchResult(
        const CaptureRequest& request,
        bool accepted,
        std::chrono::steady_clock::time_point now);

    // Earliest scheduledFor across all pending captures, for the timer wait.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    nextDeadline() const;

    [[nodiscard]] std::size_t trackedSessions() const;

private:
    enum class Phase {
        Pending,
        AwaitingResponse,
        Done
    };

    struct CaptureState {
        CaptureReason reason;
        Phase phase{Phase::Pending};
        int attempt{1};
        std::chrono::steady_clock::time_point scheduledFor;
    };

    struct SessionState {
        std::string slotId;
        std::string sensorId;
        std::chrono::system_clock::time_point startedAt;  // T0, wall time
        std::chrono::steady_clock::time_point
            startedAtMonotonic;  // deadline anchor, steady_clock
        CaptureTarget target;
        std::vector<CaptureState> captures;
    };

    CaptureRequest buildRequest(const std::string& sessionId,
                                const SessionState& session,
                                const CaptureState& capture) const;

    mutable std::mutex mutex_;
    CaptureSchedulerConfig config_;
    CaptureTargetResolver resolver_;
    std::unordered_map<std::string, SessionState> sessions_;
};

}  // namespace parking

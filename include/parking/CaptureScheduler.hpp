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

struct CaptureOffset {
    CaptureReason reason;
    std::chrono::milliseconds delay;
};

struct CaptureSchedulerConfig {
    std::vector<CaptureOffset> offsets{
        {CaptureReason::HallOccupied30s, std::chrono::seconds(30)},
        {CaptureReason::HallOccupied60s, std::chrono::seconds(60)}};
    std::chrono::milliseconds responseTimeout{3000};
    std::chrono::milliseconds retryInterval{2000};
    int maxRetries{2};
};

enum class DispatchOutcome {
    Done,
    WillRetry,
    GaveUp,
    Unknown
};

// 세션 전이에서 T0+30초/60초 요청을 계산하는 순수 정책이다. 모든 deadline은
// steady_clock이고 실제 MQTT 발행은 CaptureSchedulerRuntime에 위임한다.
class CaptureScheduler {
public:
    CaptureScheduler(CaptureSchedulerConfig config,
                     CaptureTargetResolver resolver);

    struct ScheduleReport {
        int scheduled{0};
        bool unmappedSlot{false};
        bool duplicate{false};
    };

    ScheduleReport onTransition(const ParkingTransitionResult& transition);
    [[nodiscard]] std::vector<CaptureRequest> due(
        std::chrono::steady_clock::time_point now);
    DispatchOutcome onDispatchResult(
        const CaptureRequest& request,
        bool published,
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    nextDeadline() const;
    [[nodiscard]] std::size_t trackedSessions() const;

private:
    enum class Phase { Pending, Dispatching, Done };

    struct CaptureState {
        CaptureReason reason;
        Phase phase{Phase::Pending};
        int attempt{1};
        std::chrono::steady_clock::time_point scheduledFor;
    };

    struct SessionState {
        std::string slotId;
        std::string sensorId;
        std::chrono::system_clock::time_point startedAt;
        std::chrono::steady_clock::time_point startedAtMonotonic;
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

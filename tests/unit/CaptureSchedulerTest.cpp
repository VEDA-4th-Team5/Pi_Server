#include "parking/CaptureScheduler.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::ParkingTransitionResult startTransition(
    const std::string& databaseSessionId,
    const std::chrono::system_clock::time_point wall,
    const std::chrono::steady_clock::time_point monotonic) {
    parking::ParkingTransitionResult transition;
    transition.code = parking::ParkingTransitionCode::SessionStarted;
    transition.slotId = "EV01";
    transition.sessionId = databaseSessionId;
    transition.session.emplace(
        "domain-session", "EV01", "HALL01", wall, monotonic);
    return transition;
}

parking::ParkingTransitionResult completeTransition(
    const std::string& databaseSessionId) {
    parking::ParkingTransitionResult transition;
    transition.code = parking::ParkingTransitionCode::SessionCompleted;
    transition.slotId = "EV01";
    transition.sessionId = databaseSessionId;
    return transition;
}

parking::CaptureTargetResolver resolver() {
    return [](const std::string& slotId)
        -> std::optional<parking::CaptureTarget> {
        if (slotId != "EV01") return std::nullopt;
        return parking::CaptureTarget{
            "cam01", "ch01", "EV01", 0.1, 0.2, 0.3, 0.4};
    };
}

}  // namespace

int main() {
    try {
        const auto wall = std::chrono::system_clock::now();
        const auto monotonic = std::chrono::steady_clock::now();

        parking::CaptureSchedulerConfig config;
        config.offsets = {
            {parking::CaptureReason::HallOccupied30s, 30s},
            {parking::CaptureReason::HallOccupied60s, 60s}};
        config.retryInterval = 2s;
        config.maxRetries = 2;
        parking::CaptureScheduler scheduler(config, resolver());

        const auto started = startTransition("42", wall, monotonic);
        const auto report = scheduler.onTransition(started);
        require(report.scheduled == 2, "two captures were not scheduled");
        require(scheduler.onTransition(started).duplicate,
                "duplicate session was scheduled twice");
        require(scheduler.due(monotonic + 29s).empty(),
                "30-second request fired early");

        auto due30 = scheduler.due(monotonic + 30s);
        require(due30.size() == 1 && due30.front().sessionId == "42" &&
                    due30.front().reason ==
                        parking::CaptureReason::HallOccupied30s,
                "30-second request is incorrect");
        require(scheduler.onDispatchResult(
                    due30.front(), true, monotonic + 30s) ==
                    parking::DispatchOutcome::Done,
                "successful publish was not completed");

        auto due60 = scheduler.due(monotonic + 60s);
        require(due60.size() == 1 &&
                    due60.front().reason ==
                        parking::CaptureReason::HallOccupied60s,
                "60-second request is incorrect");
        require(scheduler.onDispatchResult(
                    due60.front(), true, monotonic + 60s) ==
                    parking::DispatchOutcome::Done,
                "60-second publish was not completed");
        require(scheduler.trackedSessions() == 0,
                "settled session remained tracked");

        const auto canceled = startTransition("43", wall, monotonic);
        scheduler.onTransition(canceled);
        scheduler.onTransition(completeTransition("43"));
        require(scheduler.due(monotonic + 90s).empty(),
                "departure did not cancel pending requests");

        parking::CaptureSchedulerConfig retryConfig;
        retryConfig.offsets = {
            {parking::CaptureReason::HallOccupied30s, 0ms}};
        retryConfig.retryInterval = 2s;
        retryConfig.maxRetries = 1;
        parking::CaptureScheduler retryScheduler(retryConfig, resolver());
        retryScheduler.onTransition(startTransition("44", wall, monotonic));
        auto firstAttempt = retryScheduler.due(monotonic);
        require(firstAttempt.size() == 1 && firstAttempt.front().attempt == 1,
                "first publish attempt missing");
        require(retryScheduler.onDispatchResult(
                    firstAttempt.front(), false, monotonic) ==
                    parking::DispatchOutcome::WillRetry,
                "failed publish was not rescheduled");
        require(retryScheduler.due(monotonic + 1999ms).empty(),
                "retry fired before retry interval");
        auto secondAttempt = retryScheduler.due(monotonic + 2s);
        require(secondAttempt.size() == 1 && secondAttempt.front().attempt == 2,
                "second publish attempt missing");
        require(retryScheduler.onDispatchResult(
                    secondAttempt.front(), false, monotonic + 2s) ==
                    parking::DispatchOutcome::GaveUp,
                "retry limit was not enforced");

        std::cout << "[PASS] capture scheduler DB-session/30s/60s/cancel/retry\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] capture scheduler: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

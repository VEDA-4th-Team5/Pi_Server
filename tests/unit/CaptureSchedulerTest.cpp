// Unit test for parking::CaptureScheduler (EVDA-135).
//
// Drives the pure T0+30s / T0+60s capture scheduling policy deterministically
// (time is injected, no threads, no sleeping) and asserts the acceptance
// criteria: exactly one 30s and one 60s request succeed per session, requests
// carry the correct slot/channel/ROI/reason, a no-response is retried at the
// configured interval up to the cap, an emptied slot cancels pending captures,
// and an unmapped slot schedules nothing without breaking the session.
//
// All deadline math (due/onDispatchResult/nextDeadline) runs on steady_clock,
// not system_clock -- see CaptureScheduler.hpp for why. Every session below
// carries both a system_clock T0 (t0, reported as sessionStartedAt) and a
// steady_clock anchor (t0m, what deadlines are actually computed from); all
// due()/onDispatchResult() calls use offsets from t0m, never t0.

#include "parking/CaptureScheduler.hpp"
#include "parking/ParkingOccupancySession.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

parking::ParkingTransitionResult makeStarted(
    std::string sessionId,
    std::string slotId,
    std::string sensorId,
    std::chrono::system_clock::time_point t0,
    std::chrono::steady_clock::time_point t0m) {
    parking::ParkingTransitionResult result;
    result.code = parking::ParkingTransitionCode::SessionStarted;
    result.slotId = slotId;
    result.sessionId = sessionId;
    result.session.emplace(std::move(sessionId), std::move(slotId),
                           std::move(sensorId), t0, t0m);
    return result;
}

parking::ParkingTransitionResult makeCompleted(
    const std::string& sessionId, const std::string& slotId) {
    parking::ParkingTransitionResult result;
    result.code = parking::ParkingTransitionCode::SessionCompleted;
    result.slotId = slotId;
    result.sessionId = sessionId;
    return result;
}

// Resolver that maps EV01 -> cam01/ch01 with a non-trivial ROI, and refuses
// anything else (to exercise the unmapped-slot path).
parking::CaptureTargetResolver ev01Resolver() {
    return [](const std::string& slotId)
               -> std::optional<parking::CaptureTarget> {
        if (slotId != "EV01") {
            return std::nullopt;
        }
        parking::CaptureTarget target;
        target.cameraId = "cam01";
        target.channelId = "ch01";
        target.areaName = "EV01";
        target.roiX = 0.10;
        target.roiY = 0.20;
        target.roiWidth = 0.30;
        target.roiHeight = 0.40;
        return target;
    };
}

parking::CaptureSchedulerConfig fastConfig() {
    parking::CaptureSchedulerConfig config;  // 30s/60s offsets, 2 retries.
    config.responseTimeout = std::chrono::milliseconds(3000);
    config.retryInterval = std::chrono::milliseconds(2000);
    config.maxRetries = 2;
    return config;
}

using parking::CaptureReason;
using parking::DispatchOutcome;

}  // namespace

int main() {
    try {
        const auto t0 = std::chrono::system_clock::now();
        const auto t0m = std::chrono::steady_clock::now();

        // 1) Happy path: OCCUPIED(T0) schedules exactly two captures, and the
        //    30s one is not due before T0+30s.
        {
            parking::CaptureScheduler scheduler(fastConfig(), ev01Resolver());
            const auto report = scheduler.onTransition(
                makeStarted("S1", "EV01", "HALL01", t0, t0m));
            require(report.scheduled == 2, "expected 2 captures scheduled");
            require(!report.unmappedSlot, "EV01 should be mapped");
            require(scheduler.trackedSessions() == 1, "one session tracked");

            require(scheduler.due(t0m + std::chrono::seconds(29)).empty(),
                    "nothing should be due before T0+30s");

            auto due30 = scheduler.due(t0m + std::chrono::seconds(30));
            require(due30.size() == 1, "exactly one capture due at T0+30s");
            const auto& r30 = due30.front();
            require(r30.reason == CaptureReason::HallOccupied30s,
                    "first capture must be the 30s one");
            require(std::string(parking::toReasonString(r30.reason)) ==
                        "HALL_OCCUPIED_30S",
                    "reason string mismatch for 30s");
            require(r30.slotId == "EV01" && r30.sensorId == "HALL01",
                    "request carries wrong slot/sensor");
            require(r30.target.cameraId == "cam01" &&
                        r30.target.channelId == "ch01",
                    "request carries wrong camera/channel");
            require(r30.target.roiWidth == 0.30 && r30.target.roiHeight == 0.40,
                    "request carries wrong ROI");
            require(r30.attempt == 1, "first attempt should be 1");
            require(r30.sessionStartedAt == t0,
                    "sessionStartedAt must be the wall-clock T0, not steady");

            // Same instant, already dispatched -> not surfaced again.
            require(scheduler.due(t0m + std::chrono::seconds(30)).empty(),
                    "awaiting-response capture must not surface twice");

            // Accept 30s; then the 60s becomes due and is independent.
            require(scheduler.onDispatchResult(
                        r30, true, t0m + std::chrono::seconds(31)) ==
                        DispatchOutcome::Done,
                    "accepted 30s should be Done");

            auto due60 = scheduler.due(t0m + std::chrono::seconds(60));
            require(due60.size() == 1 &&
                        due60.front().reason == CaptureReason::HallOccupied60s,
                    "exactly the 60s capture due at T0+60s");
            require(std::string(parking::toReasonString(
                        due60.front().reason)) == "HALL_OCCUPIED_60S",
                    "reason string mismatch for 60s");
            require(scheduler.onDispatchResult(
                        due60.front(), true, t0m + std::chrono::seconds(61)) ==
                        DispatchOutcome::Done,
                    "accepted 60s should be Done");

            // Both settled -> session freed, and never fires again.
            require(scheduler.trackedSessions() == 0,
                    "session should be freed once both captures succeed");
            require(scheduler.due(t0m + std::chrono::seconds(120)).empty(),
                    "no capture should fire after both succeeded");
        }

        // 2) Dedup: a duplicate SessionStarted for the same id books nothing.
        {
            parking::CaptureScheduler scheduler(fastConfig(), ev01Resolver());
            scheduler.onTransition(makeStarted("S2", "EV01", "HALL01", t0, t0m));
            const auto again = scheduler.onTransition(
                makeStarted("S2", "EV01", "HALL01", t0, t0m));
            require(again.scheduled == 0 && again.duplicate,
                    "duplicate session must not double-book");
            require(scheduler.trackedSessions() == 1, "still one session");
        }

        // 3) Retry policy: no-response is retried at +2s, at most twice, then
        //    gives up — and the 60s capture is unaffected.
        {
            parking::CaptureScheduler scheduler(fastConfig(), ev01Resolver());
            scheduler.onTransition(makeStarted("S3", "EV01", "HALL01", t0, t0m));

            auto a1 = scheduler.due(t0m + std::chrono::seconds(30));
            require(a1.size() == 1 && a1.front().attempt == 1, "attempt 1 due");
            // No response within 3s -> retry.
            require(scheduler.onDispatchResult(
                        a1.front(), false, t0m + std::chrono::seconds(33)) ==
                        DispatchOutcome::WillRetry,
                    "first failure should retry");

            // Retry is scheduled at now(33s)+2s = 35s, not before.
            require(scheduler.due(t0m + std::chrono::seconds(34)).empty(),
                    "retry must wait retryInterval");
            auto a2 = scheduler.due(t0m + std::chrono::seconds(35));
            require(a2.size() == 1 && a2.front().attempt == 2, "attempt 2 due");
            require(scheduler.onDispatchResult(
                        a2.front(), false, t0m + std::chrono::seconds(38)) ==
                        DispatchOutcome::WillRetry,
                    "second failure should retry");

            auto a3 = scheduler.due(t0m + std::chrono::seconds(40));
            require(a3.size() == 1 && a3.front().attempt == 3, "attempt 3 due");
            require(scheduler.onDispatchResult(
                        a3.front(), false, t0m + std::chrono::seconds(43)) ==
                        DispatchOutcome::GaveUp,
                    "third failure should give up (maxRetries=2)");

            // The 30s capture is exhausted; the 60s still fires exactly once.
            require(scheduler.due(t0m + std::chrono::seconds(50)).empty(),
                    "no more 30s attempts after giving up");
            auto due60 = scheduler.due(t0m + std::chrono::seconds(60));
            require(due60.size() == 1 &&
                        due60.front().reason == CaptureReason::HallOccupied60s,
                    "60s capture must still fire after 30s gave up");
        }

        // 4) Cancellation: VACANT before the captures fire drops them.
        {
            parking::CaptureScheduler scheduler(fastConfig(), ev01Resolver());
            scheduler.onTransition(makeStarted("S4", "EV01", "HALL01", t0, t0m));
            scheduler.onTransition(makeCompleted("S4", "EV01"));
            require(scheduler.trackedSessions() == 0,
                    "completed session should be cancelled");
            require(scheduler.due(t0m + std::chrono::seconds(60)).empty(),
                    "no capture should fire for a session that ended early");
        }

        // 5) Unmapped slot: schedules nothing, flags it, session unaffected.
        {
            parking::CaptureScheduler scheduler(fastConfig(), ev01Resolver());
            const auto report = scheduler.onTransition(
                makeStarted("S5", "EV02", "HALL02", t0, t0m));
            require(report.scheduled == 0 && report.unmappedSlot,
                    "unmapped slot should schedule nothing and flag it");
            require(scheduler.trackedSessions() == 0, "no session tracked");
            require(!scheduler.nextDeadline().has_value(),
                    "no deadline for an unmapped slot");
        }

        std::cout << "[PASS] capture scheduler (EVDA-135)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] capture scheduler: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

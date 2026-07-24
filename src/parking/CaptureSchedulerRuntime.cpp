#include "parking/CaptureSchedulerRuntime.hpp"

#include "util/Logger.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace parking {

namespace {

std::string describe(const CaptureRequest& request) {
    return std::string("slot=") + request.slotId +
           " session=" + request.sessionId +
           " reason=" + toReasonString(request.reason) +
           " attempt=" + std::to_string(request.attempt);
}

}  // namespace

CaptureSchedulerRuntime::CaptureSchedulerRuntime(CaptureScheduler& scheduler,
                                                 CapturePublisher publisher)
    : scheduler_(scheduler), publisher_(std::move(publisher)) {
}

CaptureSchedulerRuntime::~CaptureSchedulerRuntime() {
    stop();
}

void CaptureSchedulerRuntime::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    stopping_ = false;
    worker_ = std::thread(&CaptureSchedulerRuntime::run, this);
}

void CaptureSchedulerRuntime::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void CaptureSchedulerRuntime::onTransition(
    const ParkingTransitionResult& transition) {
    const CaptureScheduler::ScheduleReport report =
        scheduler_.onTransition(transition);

    if (report.unmappedSlot) {
        util::logLine("CAPTURE_SCHED",
                      "no camera/ROI mapping for slot=" + transition.slotId +
                          "; capture skipped session=" + transition.sessionId);
    } else if (report.scheduled > 0) {
        util::logLine("CAPTURE_SCHED",
                      "scheduled " + std::to_string(report.scheduled) +
                          " capture(s) slot=" + transition.slotId +
                          " session=" + transition.sessionId);
    }

    // Wake the timer thread so it recomputes its wait for the new (or
    // cancelled) deadline. Take the lock before notifying: the timer thread
    // updates its wait while holding mutex_, so acquiring it here guarantees
    // the notify lands after run() is actually waiting (no lost wakeup even
    // though the schedule change above happened under the scheduler's lock).
    std::lock_guard<std::mutex> lock(mutex_);
    condition_.notify_all();
}

void CaptureSchedulerRuntime::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }

            const auto next = scheduler_.nextDeadline();
            if (!next.has_value()) {
                condition_.wait(lock);
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (*next > now) {
                    condition_.wait_until(lock, *next);
                }
            }

            if (stopping_) {
                return;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<CaptureRequest> ready = scheduler_.due(now);

        for (const auto& request : ready) {
            bool accepted = false;
            try {
                accepted = publisher_ ? publisher_(request) : false;
            } catch (const std::exception& error) {
                // Isolation: a publish fault must never escape this thread.
                util::logLine("CAPTURE_SCHED",
                              std::string("publish threw: ") + error.what() +
                                  " " + describe(request));
                accepted = false;
            } catch (...) {
                util::logLine("CAPTURE_SCHED",
                              "publish threw unknown error " + describe(request));
                accepted = false;
            }

            const DispatchOutcome outcome = scheduler_.onDispatchResult(
                request, accepted, std::chrono::steady_clock::now());

            switch (outcome) {
                case DispatchOutcome::Done:
                    util::logLine("CAPTURE_SCHED",
                                  "capture ok " + describe(request));
                    break;
                case DispatchOutcome::WillRetry:
                    util::logLine("CAPTURE_SCHED",
                                  "capture no-response, will retry " +
                                      describe(request));
                    break;
                case DispatchOutcome::GaveUp:
                    util::logLine("CAPTURE_SCHED",
                                  "capture gave up after retries " +
                                      describe(request));
                    break;
                case DispatchOutcome::Unknown:
                    break;  // session ended meanwhile; nothing to log
            }
        }
    }
}

}  // namespace parking

#include "parking/CaptureSchedulerRuntime.hpp"

#include "util/Logger.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace parking {
namespace {

std::string describe(const CaptureRequest& request) {
    return "slot=" + request.slotId + " session=" + request.sessionId +
           " reason=" + toReasonString(request.reason) +
           " attempt=" + std::to_string(request.attempt);
}

}  // namespace

CaptureSchedulerRuntime::CaptureSchedulerRuntime(
    CaptureScheduler& scheduler, CapturePublisher publisher)
    : scheduler_(scheduler), publisher_(std::move(publisher)) {}

CaptureSchedulerRuntime::~CaptureSchedulerRuntime() { stop(); }

void CaptureSchedulerRuntime::start() {
    std::lock_guard lock(mutex_);
    if (started_) return;
    started_ = true;
    stopping_ = false;
    worker_ = std::thread(&CaptureSchedulerRuntime::run, this);
}

void CaptureSchedulerRuntime::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    started_ = false;
}

void CaptureSchedulerRuntime::onTransition(
    const ParkingTransitionResult& transition) {
    const auto report = scheduler_.onTransition(transition);
    if (report.unmappedSlot) {
        util::logLine("CAPTURE_SCHED",
                      "no camera/ROI mapping for slot=" + transition.slotId +
                          "; request skipped session=" + transition.sessionId);
    } else if (report.scheduled > 0) {
        util::logLine("CAPTURE_SCHED",
                      "scheduled " + std::to_string(report.scheduled) +
                          " request(s) slot=" + transition.slotId +
                          " session=" + transition.sessionId);
    }
    // worker가 nextDeadline()을 확인하고 wait에 진입하는 사이의 lost wakeup을
    // 막기 위해 같은 mutex 경계를 지난 뒤 알린다.
    std::lock_guard lock(mutex_);
    condition_.notify_all();
}

void CaptureSchedulerRuntime::run() {
    while (true) {
        {
            std::unique_lock lock(mutex_);
            if (stopping_) return;
            const auto next = scheduler_.nextDeadline();
            if (next) condition_.wait_until(lock, *next);
            else condition_.wait(lock);
            if (stopping_) return;
        }

        const auto now = std::chrono::steady_clock::now();
        for (const auto& request : scheduler_.due(now)) {
            bool published = false;
            try {
                published = publisher_ && publisher_(request);
            } catch (const std::exception& error) {
                util::logError(std::string("capture request publish threw: ") +
                               error.what() + " " + describe(request));
            } catch (...) {
                util::logError("capture request publish threw: " +
                               describe(request));
            }

            const auto outcome = scheduler_.onDispatchResult(
                request, published, std::chrono::steady_clock::now());
            switch (outcome) {
                case DispatchOutcome::Done:
                    util::logLine("CAPTURE_SCHED",
                                  "capture request published " +
                                      describe(request));
                    break;
                case DispatchOutcome::WillRetry:
                    util::logWarn("capture request publish failed; retry " +
                                  describe(request));
                    break;
                case DispatchOutcome::GaveUp:
                    util::logError("capture request publish gave up " +
                                   describe(request));
                    break;
                case DispatchOutcome::Unknown:
                    break;
            }
        }
    }
}

}  // namespace parking

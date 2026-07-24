#pragma once

#include "parking/CaptureRequest.hpp"
#include "parking/CaptureScheduler.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace parking {

// Sends one capture request. Returns true when the request was accepted
// (published/acknowledged) within request.responseTimeout, false otherwise.
// The implementation lives in the composition root (main.cpp) and is the only
// place that knows about MQTT, so the scheduler stays I/O-free.
using CapturePublisher = std::function<bool(const CaptureRequest&)>;

// Timer thread that turns CaptureScheduler decisions into publisher calls.
//
// Registered as a ParkingSessionWorker::TransitionSink: onTransition() only
// records the schedule and wakes the timer thread, so it never blocks the
// UART/worker thread. The blocking publish runs here, on this class's own
// thread, and every publisher call is exception-isolated — a camera/MQTT fault
// can never stop the session state machine or the 1-hour timer.
class CaptureSchedulerRuntime {
public:
    CaptureSchedulerRuntime(CaptureScheduler& scheduler,
                            CapturePublisher publisher);
    ~CaptureSchedulerRuntime();

    CaptureSchedulerRuntime(const CaptureSchedulerRuntime&) = delete;
    CaptureSchedulerRuntime& operator=(const CaptureSchedulerRuntime&) = delete;

    void start();
    void stop();

    // Worker sink entry point. Fast: schedule + notify only.
    void onTransition(const ParkingTransitionResult& transition);

private:
    void run();

    CaptureScheduler& scheduler_;
    CapturePublisher publisher_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
};

}  // namespace parking

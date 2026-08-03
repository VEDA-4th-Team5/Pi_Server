#pragma once

#include "parking/CaptureRequest.hpp"
#include "parking/CaptureScheduler.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace parking {

// true는 주입된 촬영 실행기(RTSP 저장/DB 또는 MQTT-only)가 작업을 수락했음을 뜻한다.
using CapturePublisher = std::function<bool(const CaptureRequest& request)>;

class CaptureSchedulerRuntime {
public:
    CaptureSchedulerRuntime(CaptureScheduler& scheduler,
                            CapturePublisher publisher);
    ~CaptureSchedulerRuntime();

    CaptureSchedulerRuntime(const CaptureSchedulerRuntime&) = delete;
    CaptureSchedulerRuntime& operator=(const CaptureSchedulerRuntime&) = delete;

    void start();
    void stop();
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

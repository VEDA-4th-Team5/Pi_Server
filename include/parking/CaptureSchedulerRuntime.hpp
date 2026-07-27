#pragma once

#include "parking/CaptureScheduler.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace parking {

// true는 카메라 촬영 완료가 아니라 MQTT Broker에 요청 발행이 접수됐음을 뜻한다.
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

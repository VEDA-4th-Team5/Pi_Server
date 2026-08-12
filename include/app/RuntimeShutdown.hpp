#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace app {

struct RuntimeShutdownHooks {
    std::function<void()> stopHttp;
    std::function<void()> quiesceMqttApplication;
    std::function<void()> stopSensorIngress;
    std::function<void()> drainHall;
    std::function<void()> drainCapture;
    std::function<void()> drainBestShot;
    std::function<void()> drainEvidence;
    std::function<void()> drainOcr;
    std::function<void()> drainTimer;
    std::function<void()> stopRtsp;
    std::function<void()> drainFire;
    std::function<void()> drainReporter;
    std::function<void()> stopMqtt;
    std::function<void()> destroyCallbackTargets;
    std::function<void()> closeDatabase;
};

class RuntimeShutdown {
public:
    explicit RuntimeShutdown(RuntimeShutdownHooks hooks);
    ~RuntimeShutdown();
    RuntimeShutdown(const RuntimeShutdown&) = delete;
    RuntimeShutdown& operator=(const RuntimeShutdown&) = delete;

    [[nodiscard]] bool shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    enum class State { Running, Stopping, Stopped, Failed };

    RuntimeShutdownHooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    State state_{State::Running};
    std::thread::id stopping_thread_{};
};

}  // namespace app

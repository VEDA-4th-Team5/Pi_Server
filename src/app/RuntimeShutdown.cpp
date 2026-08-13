#include "app/RuntimeShutdown.hpp"

#include "util/Logger.hpp"

#include <array>
#include <exception>
#include <utility>

namespace app {
namespace {

bool runHook(const char* name, const std::function<void()>& hook) noexcept {
    if (!hook) return true;
    try {
        hook();
        return true;
    } catch (const std::exception& error) {
        util::logError(std::string("runtime shutdown hook failed: ") + name +
                       ": " + error.what());
    } catch (...) {
        util::logError(std::string("runtime shutdown hook failed: ") + name);
    }
    return false;
}

}  // namespace

RuntimeShutdown::RuntimeShutdown(RuntimeShutdownHooks hooks)
    : hooks_(std::move(hooks)) {}

RuntimeShutdown::~RuntimeShutdown() {
    if (!shutdown()) {
        util::logError(
            "runtime shutdown failed during destruction; terminating with "
            "callback targets retained");
        std::terminate();
    }
}

bool RuntimeShutdown::shutdown() noexcept {
    {
        std::unique_lock lock(mutex_);
        if (state_ == State::Stopped) return true;
        if (state_ == State::Failed) return false;
        if (state_ == State::Stopping) {
            if (stopping_thread_ == std::this_thread::get_id()) {
                util::logError("recursive runtime shutdown rejected");
                return false;
            }
            condition_.wait(lock, [this] {
                return state_ == State::Stopped || state_ == State::Failed;
            });
            return state_ == State::Stopped;
        }
        state_ = State::Stopping;
        stopping_thread_ = std::this_thread::get_id();
    }

    const std::array<std::pair<const char*, const std::function<void()>*>, 15>
        orderedHooks{{
            {"http", &hooks_.stopHttp},
            {"mqtt-application", &hooks_.quiesceMqttApplication},
            {"sensor-ingress", &hooks_.stopSensorIngress},
            {"hall", &hooks_.drainHall},
            {"capture", &hooks_.drainCapture},
            {"bestshot", &hooks_.drainBestShot},
            {"evidence", &hooks_.drainEvidence},
            {"ocr", &hooks_.drainOcr},
            {"timer", &hooks_.drainTimer},
            {"rtsp", &hooks_.stopRtsp},
            {"fire", &hooks_.drainFire},
            {"reporter", &hooks_.drainReporter},
            {"mqtt", &hooks_.stopMqtt},
            {"callback-targets", &hooks_.destroyCallbackTargets},
            {"database", &hooks_.closeDatabase}}};

    for (const auto& [name, hook] : orderedHooks) {
        if (runHook(name, *hook)) continue;
        {
            std::lock_guard lock(mutex_);
            state_ = State::Failed;
            stopping_thread_ = {};
        }
        condition_.notify_all();
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        state_ = State::Stopped;
        stopping_thread_ = {};
    }
    condition_.notify_all();
    return true;
}

bool RuntimeShutdown::stopped() const noexcept {
    std::lock_guard lock(mutex_);
    return state_ == State::Stopped;
}

bool RuntimeShutdown::failed() const noexcept {
    std::lock_guard lock(mutex_);
    return state_ == State::Failed;
}

}  // namespace app

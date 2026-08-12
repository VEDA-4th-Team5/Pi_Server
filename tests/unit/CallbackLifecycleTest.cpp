#include "app/RuntimeShutdown.hpp"
#include "mqtt/MqttEndpoint.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ManualEvent {
public:
    void signal() {
        {
            std::lock_guard lock(mutex_);
            signaled_ = true;
        }
        condition_.notify_all();
    }

    void wait(const std::string& failure) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 2s, [this] { return signaled_; })) {
            throw std::runtime_error(failure);
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{false};
};

class FakeMqttTransport final : public mqtt::IMqttTransport {
public:
    bool startResult{true};
    bool stopResult{true};
    bool emitMessageDuringStart{false};
    bool blockStart{false};
    int startCount{};
    int stopCount{};
    int publishCount{};

    bool start(const mqtt::MqttConnectionOptions&,
               const std::vector<mqtt::MqttSubscription>&,
               MessageCallback messageCallback,
               ObserverCallbacks observerCallbacks) override {
        ++startCount;
        messageCallback_ = std::move(messageCallback);
        observerCallbacks_ = std::move(observerCallbacks);
        if (blockStart) {
            startEntered.signal();
            releaseStart.wait("fake transport start was not released");
        }
        running_ = startResult;
        if (emitMessageDuringStart && messageCallback_) {
            messageCallback_("parking/v1/fire/ack/CH1",
                             R"({"command":"ALARM_ACK","alarm_id":"A1"})");
        }
        return startResult;
    }

    mqtt::MqttPublishResult publish(const std::string&,
                                    const std::string&,
                                    int,
                                    bool) override {
        ++publishCount;
        return {running_, running_ ? 7 : -1};
    }

    bool stopAndJoin() noexcept override {
        if (!stopResult) return false;
        if (!stopped_) {
            stopped_ = true;
            running_ = false;
            ++stopCount;
        }
        return true;
    }

    void emitMessage(const std::string& topic, const std::string& payload) {
        if (running_ && messageCallback_) messageCallback_(topic, payload);
    }

    void emitConnected() {
        if (running_ && observerCallbacks_.onConnected)
            observerCallbacks_.onConnected();
    }

    void emitObserversRaw() {
        if (observerCallbacks_.onConnected)
            observerCallbacks_.onConnected();
        if (observerCallbacks_.onDisconnected)
            observerCallbacks_.onDisconnected(1);
        if (observerCallbacks_.onPublished)
            observerCallbacks_.onPublished(7);
    }

    ManualEvent startEntered;
    ManualEvent releaseStart;

private:
    bool running_{false};
    bool stopped_{false};
    MessageCallback messageCallback_;
    ObserverCallbacks observerCallbacks_;
};

void waitForState(mqtt::MqttEndpoint& endpoint,
                  const mqtt::MqttEndpointState expected,
                  const std::string& failure) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (endpoint.state() != expected) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(failure);
        std::this_thread::yield();
    }
}

void testImmediateStartIngressUsesBoundTarget() {
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->emitMessageDuringStart = true;
    mqtt::MqttEndpoint endpoint(std::move(transport));
    std::atomic<int> targetCalls{};

    require(endpoint.bindApplicationHandler(
                [&](const std::string&, const std::string&) { ++targetCalls; }),
            "application target must bind while endpoint is stopped");
    require(endpoint.start({"test-client", "127.0.0.1", 1883, 60},
                           {{"parking/v1/fire/ack/+", 1}}),
            "fake endpoint start failed");
    require(targetCalls.load() == 1,
            "message delivered inside transport start missed the bound target");
    require(endpoint.stop(), "endpoint stop failed");
}

void testQuiesceWaitsAndRejectsLateIngress() {
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    mqtt::MqttEndpoint endpoint(std::move(transport));
    ManualEvent handlerEntered;
    ManualEvent releaseHandler;
    std::atomic<int> applicationCalls{};
    std::atomic<int> observerCalls{};
    std::atomic<bool> quiesceReturned{false};
    std::atomic<bool> quiesceSucceeded{false};

    require(endpoint.bindApplicationHandler(
                [&](const std::string&, const std::string&) {
                    ++applicationCalls;
                    handlerEntered.signal();
                    releaseHandler.wait("held application callback was not released");
                }),
            "application target bind failed");
    require(endpoint.bindTransportObservers({
                [&] { ++observerCalls; }, {}, {}}),
            "transport observer bind failed");
    require(endpoint.start({"test-client", "127.0.0.1", 1883, 60}, {}),
            "fake endpoint start failed");

    std::thread callback([&] { fake->emitMessage("topic", "payload"); });
    handlerEntered.wait("application callback did not enter");

    endpoint.closeIngress();
    require(endpoint.state() == mqtt::MqttEndpointState::Quiescing,
            "closeIngress did not publish Quiescing after closing admission");
    fake->emitMessage("topic", "late-after-close");
    require(applicationCalls.load() == 1,
            "application ingress entered after closeIngress");

    std::thread quiescer([&] {
        quiesceSucceeded.store(endpoint.quiesceIngress());
        quiesceReturned.store(true, std::memory_order_release);
    });
    waitForState(endpoint, mqtt::MqttEndpointState::Quiescing,
                 "endpoint did not enter Quiescing");
    require(!quiesceReturned.load(std::memory_order_acquire),
            "quiesce returned while an application lease was active");

    fake->emitMessage("topic", "late-during-quiesce");
    require(applicationCalls.load() == 1,
            "application ingress entered after Quiescing was published");
    fake->emitConnected();
    require(observerCalls.load() == 1,
            "transport observer was closed with application admission");

    releaseHandler.signal();
    callback.join();
    quiescer.join();
    require(quiesceReturned.load(std::memory_order_acquire),
            "quiesce did not return after the active lease completed");
    require(quiesceSucceeded.load(), "application callback drain failed");

    fake->emitMessage("topic", "late");
    require(applicationCalls.load() == 1,
            "late application ingress ran after quiescence");
    fake->emitConnected();
    require(observerCalls.load() == 2,
            "transport observer was incorrectly closed by application quiescence");
    require(endpoint.stop(), "endpoint stop failed after quiescence");
}

void testStartFailureAndStopAreExactlyOnce() {
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->startResult = false;
    mqtt::MqttEndpoint endpoint(std::move(transport));
    require(endpoint.bindApplicationHandler(
                [](const std::string&, const std::string&) {}),
            "application target bind failed");
    require(!endpoint.start({"test-client", "127.0.0.1", 1883, 60}, {}),
            "transport failure must fail endpoint start");
    require(endpoint.state() == mqtt::MqttEndpointState::Stopped,
            "failed start did not leave the endpoint stopped");
    require(fake->startCount == 1 && fake->stopCount == 1,
            "failed start did not clean the transport exactly once");
    require(endpoint.stop(), "idempotent endpoint stop failed");
    require(endpoint.stop(), "second idempotent endpoint stop failed");
    require(fake->stopCount == 1, "idempotent stop called transport twice");
}

void testConcurrentStartAndStopCannotOrphanTransport() {
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->blockStart = true;
    mqtt::MqttEndpoint endpoint(std::move(transport));
    require(endpoint.bindApplicationHandler(
                [](const std::string&, const std::string&) {}),
            "application target bind failed");
    std::atomic<bool> startResult{false};
    std::atomic<bool> stopResult{false};
    std::atomic<bool> stopReturned{false};
    ManualEvent stopAttempted;

    std::thread starter([&] {
        startResult.store(endpoint.start(
            {"test-client", "127.0.0.1", 1883, 60}, {}));
    });
    fake->startEntered.wait("fake transport start did not enter");
    std::thread stopper([&] {
        stopAttempted.signal();
        stopResult.store(endpoint.stop());
        stopReturned.store(true, std::memory_order_release);
    });
    stopAttempted.wait("concurrent stop did not start");
    require(!stopReturned.load(std::memory_order_acquire),
            "stop passed an in-progress transport start");

    fake->releaseStart.signal();
    starter.join();
    stopper.join();
    require(startResult.load() && stopResult.load(),
            "serialized start/stop did not both complete");
    require(endpoint.state() == mqtt::MqttEndpointState::Stopped &&
                fake->stopCount == 1,
            "concurrent start/stop left an orphan transport loop");
}

void testTransportJoinFailurePreservesObserverMappings() {
    auto transport = std::make_unique<FakeMqttTransport>();
    auto* fake = transport.get();
    fake->stopResult = false;
    mqtt::MqttEndpoint endpoint(std::move(transport));
    std::atomic<int> applicationCalls{};
    std::atomic<int> observerCalls{};
    require(endpoint.bindApplicationHandler(
                [&](const std::string&, const std::string&) {
                    ++applicationCalls;
                }),
            "application target bind failed");
    require(endpoint.bindTransportObservers(
                {[&] { ++observerCalls; },
                 [&](int) { ++observerCalls; },
                 [&](int) { ++observerCalls; }}),
            "transport observer bind failed");
    require(endpoint.start({"test-client", "127.0.0.1", 1883, 60}, {}),
            "fake endpoint start failed");

    require(!endpoint.stop(),
            "transport join failure was reported as successful stop");
    require(endpoint.state() == mqtt::MqttEndpointState::Quiescing,
            "failed transport join cleared endpoint lifecycle state");
    fake->emitMessage("topic", "late");
    fake->emitConnected();
    require(applicationCalls.load() == 0,
            "application callback entered after failed stop quiescence");
    require(observerCalls.load() == 1,
            "observer mapping was destroyed before transport join succeeded");

    fake->stopResult = true;
    require(endpoint.stop(), "transport join retry did not complete");
    fake->emitObserversRaw();
    require(observerCalls.load() == 1,
            "transport observer entered after successful endpoint stop");
}

void testRuntimeShutdownOrderAndIdempotency() {
    std::vector<std::string> trace;
    auto record = [&](std::string name) {
        return [&, name = std::move(name)] { trace.push_back(name); };
    };
    app::RuntimeShutdown shutdown({
        record("http"),
        record("mqtt-app"),
        record("sensor"),
        record("hall"),
        record("capture"),
        record("bestshot"),
        record("evidence"),
        record("ocr"),
        record("timer"),
        record("rtsp"),
        record("fire"),
        record("reporter"),
        record("mqtt"),
        record("targets"),
        record("database")});

    require(shutdown.shutdown(), "ordered runtime shutdown failed");
    require(shutdown.shutdown(), "idempotent runtime shutdown failed");
    const std::vector<std::string> expected{
        "http", "mqtt-app", "sensor", "hall", "capture", "bestshot",
        "evidence", "ocr", "timer", "rtsp", "fire", "reporter",
        "mqtt", "targets", "database"};
    require(trace == expected,
            "runtime shutdown order or exactly-once guarantee is wrong");
}

void testRuntimeShutdownFailurePreservesDependencies() {
    std::vector<std::string> trace;
    auto shutdown = std::make_unique<app::RuntimeShutdown>(
        app::RuntimeShutdownHooks{
        [&] {
            trace.push_back("http");
            throw std::runtime_error("join failed");
        },
        [&] { trace.push_back("mqtt-app"); },
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        [&] { trace.push_back("targets"); },
        [&] { trace.push_back("database"); }});

    require(!shutdown->shutdown(),
            "barrier exception was reported as successful shutdown");
    require(shutdown->failed(), "failed shutdown state was not retained");
    require(trace == std::vector<std::string>{"http"},
            "shutdown continued into target or database destruction");

    // A failed production guard deliberately cannot be destroyed safely: its
    // callback targets must remain allocated until fail-stop. Release this
    // test-only owner to model that retention without invoking terminate().
    (void)shutdown.release();
}

void testFailedDestructorTerminatesInsteadOfUnwindingTargets() {
    const pid_t child = fork();
    require(child >= 0, "fork failed for destructor fail-stop test");
    if (child == 0) {
        std::set_terminate([] { std::_Exit(77); });
        {
            app::RuntimeShutdown shutdown({
                [] { throw std::runtime_error("HTTP join failed"); },
                {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}});
        }
        std::_Exit(0);
    }

    int status{};
    require(waitpid(child, &status, 0) == child,
            "failed to wait for destructor fail-stop child");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 77,
            "failed shutdown destructor unwound into target destruction");
}

}  // namespace

int main() {
    try {
        testImmediateStartIngressUsesBoundTarget();
        testQuiesceWaitsAndRejectsLateIngress();
        testStartFailureAndStopAreExactlyOnce();
        testConcurrentStartAndStopCannotOrphanTransport();
        testTransportJoinFailurePreservesObserverMappings();
        testRuntimeShutdownOrderAndIdempotency();
        testRuntimeShutdownFailurePreservesDependencies();
        testFailedDestructorTerminatesInsteadOfUnwindingTargets();
        std::cout << "Callback lifecycle tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

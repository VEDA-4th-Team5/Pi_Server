#include "mqtt/MqttEndpoint.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
        if (!condition_.wait_for(lock, 2s, [this] { return signaled_; }))
            throw std::runtime_error(failure);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{false};
};

class FakeTrackedTransport final : public mqtt::IMqttTransport {
public:
    bool start(const mqtt::MqttConnectionOptions&,
               const std::vector<mqtt::MqttSubscription>&,
               MessageCallback messageCallback,
               ObserverCallbacks observerCallbacks) override {
        messageCallback_ = std::move(messageCallback);
        observers_ = std::move(observerCallbacks);
        running_ = true;
        emit({mqtt::MqttTransportFactKind::Connected, epoch_, -1, 0, {}, {}});
        emit({mqtt::MqttTransportFactKind::SubscriptionsReady,
              epoch_, -1, 0, {}, {}});
        return true;
    }

    mqtt::MqttPublishResult publish(const std::string&,
                                    const std::string&,
                                    int,
                                    bool) override {
        return {running_, running_ ? reusedMid_ : -1};
    }

    mqtt::MqttTrackedPublishResult publishTracked(
        const std::string&,
        const std::string&,
        bool,
        mqtt::MqttPublishCorrelation correlation,
        const mqtt::MqttConnectionEpoch expectedEpoch) override {
        if (!running_ || expectedEpoch != epoch_) return {};
        if (blockNextTrackedPublish_.exchange(
                false, std::memory_order_acq_rel)) {
            trackedPublishEntered_.signal();
            trackedPublishRelease_.wait(
                "blocked tracked publish was not released");
        }
        const mqtt::MqttTrackedPublishToken token{
            epoch_, reusedMid_, std::move(correlation)};
        if (ackBeforeReturn_) {
            emit({mqtt::MqttTransportFactKind::PublishAcknowledged,
                  epoch_, reusedMid_, 0, {}, {}});
        }
        return {true, token};
    }

    bool stopAndJoin() noexcept override {
        if (!running_) return true;
        running_ = false;
        ++stopCount;
        return true;
    }

    bool abortActiveEpoch(
        const mqtt::MqttConnectionEpoch expectedEpoch) noexcept override {
        abortedEpoch = expectedEpoch;
        ++abortCount;
        return running_ && expectedEpoch == epoch_;
    }

    void blockNextTrackedPublish() {
        blockNextTrackedPublish_.store(true, std::memory_order_release);
    }

    void waitForTrackedPublishEntry() {
        trackedPublishEntered_.wait("tracked publish did not enter transport");
    }

    void releaseTrackedPublish() { trackedPublishRelease_.signal(); }

    void reconnectFreshEpoch() {
        const auto oldEpoch = epoch_;
        emit({mqtt::MqttTransportFactKind::Disconnected,
              oldEpoch, -1, 1, {}, {}});
        ++epoch_;
        emit({mqtt::MqttTransportFactKind::Connected,
              epoch_, -1, 0, {}, {}});
        emit({mqtt::MqttTransportFactKind::SubscriptionAcknowledged,
              epoch_, 3, 0, "parking/v1/fire/ack/+", {1}});
        emit({mqtt::MqttTransportFactKind::SubscriptionsReady,
              epoch_, -1, 0, {}, {}});
    }

    void emitAck(const mqtt::MqttConnectionEpoch epoch, const int mid) {
        emit({mqtt::MqttTransportFactKind::PublishAcknowledged,
              epoch, mid, 0, {}, {}});
    }

    void emitRaw(const mqtt::MqttTransportFact& fact) { emit(fact); }

    bool ackBeforeReturn_{true};
    int stopCount{};
    int abortCount{};
    mqtt::MqttConnectionEpoch abortedEpoch{};

private:
    void emit(const mqtt::MqttTransportFact& fact) {
        if (observers_.onFact) observers_.onFact(fact);
    }

    MessageCallback messageCallback_;
    ObserverCallbacks observers_;
    mqtt::MqttConnectionEpoch epoch_{1};
    int reusedMid_{7};
    bool running_{false};
    std::atomic<bool> blockNextTrackedPublish_{false};
    ManualEvent trackedPublishEntered_;
    ManualEvent trackedPublishRelease_;
};

using AttemptKey = std::pair<mqtt::MqttConnectionEpoch, int>;

class DeliveryActorProbe {
public:
    void enqueue(const mqtt::MqttTransportFact& fact) {
        std::lock_guard lock(mutex_);
        facts_.push_back(fact);
    }

    mqtt::MqttTrackedPublishToken publishAndRegister(
        mqtt::MqttEndpoint& endpoint,
        mqtt::MqttPublishCorrelation correlation,
        const mqtt::MqttConnectionEpoch expectedEpoch) {
        const auto result = endpoint.publishTracked(
            "parking/v2/fire/state/CH1", "{}", true,
            std::move(correlation), expectedEpoch);
        require(result.accepted, "tracked publish was rejected");

        // This registration occurs in the same actor turn as publishTracked.
        // An immediate callback may already be queued but cannot be consumed by
        // this single owner until this method returns.
        inFlight_.emplace(
            AttemptKey{result.token.epoch, result.token.messageId},
            result.token.correlation);
        return result.token;
    }

    void drain(const mqtt::MqttConnectionEpoch currentEpoch) {
        std::deque<mqtt::MqttTransportFact> facts;
        {
            std::lock_guard lock(mutex_);
            facts.swap(facts_);
        }
        for (const auto& fact : facts) {
            if (fact.kind !=
                mqtt::MqttTransportFactKind::PublishAcknowledged) {
                continue;
            }
            if (fact.epoch != currentEpoch) continue;
            const auto found = inFlight_.find(
                AttemptKey{fact.epoch, fact.messageId});
            if (found == inFlight_.end()) continue;
            acknowledged_.push_back(found->second);
            inFlight_.erase(found);
        }
    }

    [[nodiscard]] std::size_t queuedFactCount() const {
        std::lock_guard lock(mutex_);
        return facts_.size();
    }

    [[nodiscard]] std::size_t acknowledgedCount() const {
        return acknowledged_.size();
    }

    [[nodiscard]] const mqtt::MqttPublishCorrelation& lastAcknowledged() const {
        return acknowledged_.back();
    }

private:
    mutable std::mutex mutex_;
    std::deque<mqtt::MqttTransportFact> facts_;
    std::map<AttemptKey, mqtt::MqttPublishCorrelation> inFlight_;
    std::vector<mqtt::MqttPublishCorrelation> acknowledged_;
};

std::unique_ptr<mqtt::MqttEndpoint> makeStartedEndpoint(
    std::unique_ptr<FakeTrackedTransport> transport,
    FakeTrackedTransport** fake,
    mqtt::IMqttTransport::ObserverCallbacks observers) {
    *fake = transport.get();
    auto endpoint = std::make_unique<mqtt::MqttEndpoint>(
        std::move(transport));
    require(endpoint->bindApplicationHandler(
                [](const std::string&, const std::string&) {}),
            "application handler bind failed");
    require(endpoint->bindTransportObservers(std::move(observers)),
            "transport observer bind failed");
    require(endpoint->start(
                {"tracked-test", "127.0.0.1", 1883, 60},
                {{"parking/v1/fire/ack/+", 1}}),
            "endpoint start failed");
    return endpoint;
}

void testImmediateAckIsQueuedUntilTokenRegistration() {
    DeliveryActorProbe actor;
    FakeTrackedTransport* fake{};
    mqtt::IMqttTransport::ObserverCallbacks observers;
    observers.onFact = [&](const mqtt::MqttTransportFact& fact) {
        actor.enqueue(fact);
    };
    auto endpoint = makeStartedEndpoint(
        std::make_unique<FakeTrackedTransport>(), &fake,
        std::move(observers));
    (void)fake;

    // Remove connect/readiness facts. The fake then emits PUBACK synchronously
    // from inside publishTracked(), before that call returns its token.
    actor.drain(1);
    const auto token = actor.publishAndRegister(
        *endpoint, {"retained:CH1", 11, "attempt-11"}, 1);
    require(token.epoch == 1 && token.messageId == 7,
            "tracked token lost epoch or MID");
    require(actor.queuedFactCount() == 1,
            "immediate PUBACK was applied inline instead of queued");

    actor.drain(1);
    require(actor.acknowledgedCount() == 1,
            "queued immediate PUBACK did not resolve registered token");
    const auto& correlation = actor.lastAcknowledged();
    require(correlation.deliveryKey == "retained:CH1" &&
                correlation.revision == 11 &&
                correlation.attemptId == "attempt-11",
            "PUBACK resolved the wrong durable correlation");
    require(endpoint->stop(), "endpoint stop failed");
}

void testEpochMakesReusedMidUnambiguous() {
    DeliveryActorProbe actor;
    FakeTrackedTransport* fake{};
    auto transport = std::make_unique<FakeTrackedTransport>();
    transport->ackBeforeReturn_ = false;
    mqtt::IMqttTransport::ObserverCallbacks observers;
    observers.onFact = [&](const mqtt::MqttTransportFact& fact) {
        actor.enqueue(fact);
    };
    auto endpoint = makeStartedEndpoint(
        std::move(transport), &fake, std::move(observers));
    actor.drain(1);

    const auto oldToken = actor.publishAndRegister(
        *endpoint, {"event:old", 20, "attempt-old"}, 1);
    require(oldToken.epoch == 1 && oldToken.messageId == 7,
            "first token did not use expected epoch/MID");

    fake->reconnectFreshEpoch();
    actor.drain(2);
    require(!endpoint->publishTracked(
                 "parking/v2/fire/event/CH1", "{}", false,
                 {"event:stale-turn", 21, "attempt-stale-turn"}, 1)
                 .accepted,
            "epoch-1 actor turn published through epoch 2");
    const auto newToken = actor.publishAndRegister(
        *endpoint, {"event:new", 22, "attempt-new"}, 2);
    require(newToken.epoch == 2 && newToken.messageId == 7,
            "fake reconnect did not reuse MID under a fresh epoch");

    fake->emitAck(1, 7);
    actor.drain(2);
    require(actor.acknowledgedCount() == 0,
            "late old-epoch PUBACK acknowledged the reused MID");

    fake->emitAck(2, 7);
    actor.drain(2);
    require(actor.acknowledgedCount() == 1 &&
                actor.lastAcknowledged().attemptId == "attempt-new",
            "current-epoch PUBACK did not resolve the new attempt");
    require(endpoint->stop(), "endpoint stop failed");
}

void testObserverQuiesceWaitsAndRejectsLateFacts() {
    FakeTrackedTransport* fake{};
    ManualEvent entered;
    ManualEvent release;
    ManualEvent quiesceAttempted;
    std::atomic<int> observerCalls{};
    std::atomic<bool> quiesceReturned{false};
    mqtt::IMqttTransport::ObserverCallbacks observers;
    observers.onFact = [&](const mqtt::MqttTransportFact& fact) {
        if (fact.kind != mqtt::MqttTransportFactKind::PublishAcknowledged)
            return;
        ++observerCalls;
        entered.signal();
        release.wait("held observer was not released");
    };
    auto endpoint = makeStartedEndpoint(
        std::make_unique<FakeTrackedTransport>(), &fake,
        std::move(observers));
    fake->ackBeforeReturn_ = false;

    std::thread callback([&] { fake->emitAck(1, 7); });
    entered.wait("transport observer did not enter");
    std::thread quiescer([&] {
        quiesceAttempted.signal();
        require(endpoint->quiesceTransportObservers(),
                "transport observer quiesce failed");
        quiesceReturned.store(true, std::memory_order_release);
    });
    quiesceAttempted.wait("observer quiesce did not start");
    const auto closeDeadline = std::chrono::steady_clock::now() + 2s;
    while (endpoint->publishTracked(
               "parking/v2/fire/state/CH1", "{}", true,
               {"probe:CH1", 29, "probe-before-close"}, 1)
               .accepted) {
        if (std::chrono::steady_clock::now() >= closeDeadline)
            throw std::runtime_error(
                "transport observer admission did not close");
        std::this_thread::yield();
    }
    require(!quiesceReturned.load(std::memory_order_acquire),
            "observer quiesce returned while a lease was active");

    release.signal();
    callback.join();
    quiescer.join();
    require(quiesceReturned.load(std::memory_order_acquire),
            "observer quiesce did not finish after lease release");

    fake->emitAck(1, 7);
    require(observerCalls.load() == 1,
            "late transport fact entered after observer quiescence");
    require(!endpoint->publishTracked(
                 "parking/v2/fire/state/CH1", "{}", true,
                 {"retained:CH1", 30, "attempt-after-quiesce"}, 1)
                 .accepted,
            "tracked publish remained open without an ACK observer");
    require(endpoint->stop(), "endpoint stop after observer quiesce failed");
}

void testDirectStopClosesAndDrainsTrackedEgressFirst() {
    FakeTrackedTransport* fake{};
    auto endpoint = makeStartedEndpoint(
        std::make_unique<FakeTrackedTransport>(), &fake, {});
    fake->ackBeforeReturn_ = false;
    fake->blockNextTrackedPublish();
    std::atomic<bool> publishReturned{false};
    std::atomic<bool> publishAccepted{false};
    std::thread publisher([&] {
        const auto result = endpoint->publishTracked(
            "parking/v2/fire/state/CH1", "{}", true,
            {"fire-state:CH1", 31, "held-at-stop"}, 1);
        publishAccepted.store(result.accepted, std::memory_order_release);
        publishReturned.store(true, std::memory_order_release);
    });
    fake->waitForTrackedPublishEntry();

    std::atomic<bool> stopReturned{false};
    std::thread stopper([&] {
        require(endpoint->stop(), "endpoint direct stop failed");
        stopReturned.store(true, std::memory_order_release);
    });
    const auto closeDeadline = std::chrono::steady_clock::now() + 2s;
    while (endpoint->publishTracked(
               "parking/v2/fire/state/CH1", "{}", true,
               {"probe:CH1", 32, "probe-stop-close"}, 1)
               .accepted) {
        if (std::chrono::steady_clock::now() >= closeDeadline)
            throw std::runtime_error(
                "direct stop did not close tracked egress admission");
        std::this_thread::yield();
    }
    require(!stopReturned.load(std::memory_order_acquire) &&
                !publishReturned.load(std::memory_order_acquire) &&
                fake->stopCount == 0,
            "transport stopped before the active tracked publish drained");

    fake->releaseTrackedPublish();
    publisher.join();
    stopper.join();
    require(publishAccepted.load(std::memory_order_acquire) &&
                stopReturned.load(std::memory_order_acquire) &&
                fake->stopCount == 1,
            "tracked publish drain did not complete before exactly-once stop");
}

void testAbortForwardsOnlyTheExpectedActiveEpoch() {
    FakeTrackedTransport* fake{};
    auto endpoint = makeStartedEndpoint(
        std::make_unique<FakeTrackedTransport>(), &fake, {});
    require(!endpoint->abortActiveEpoch(9),
            "stale epoch abort must be rejected by the transport");
    require(endpoint->abortActiveEpoch(1) && fake->abortCount == 2 &&
                fake->abortedEpoch == 1,
            "current epoch abort was not forwarded exactly");
    require(endpoint->stop(), "endpoint stop after abort test failed");
}

}  // namespace

int main() {
    try {
        testImmediateAckIsQueuedUntilTokenRegistration();
        testEpochMakesReusedMidUnambiguous();
        testObserverQuiesceWaitsAndRejectsLateFacts();
        testDirectStopClosesAndDrainsTrackedEgressFirst();
        testAbortForwardsOnlyTheExpectedActiveEpoch();
        std::cout << "MQTT tracked transport tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

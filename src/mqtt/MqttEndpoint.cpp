#include "mqtt/MqttEndpoint.hpp"

#include <utility>

namespace mqtt {

MqttEndpoint::MqttEndpoint(std::unique_ptr<IMqttTransport> transport)
    : transport_(std::move(transport)) {}

MqttEndpoint::~MqttEndpoint() {
    if (!stop()) std::terminate();
}

bool MqttEndpoint::bindApplicationHandler(ApplicationHandler handler) {
    std::lock_guard lock(stateMutex_);
    if (state_ != MqttEndpointState::Constructed || !handler) return false;
    applicationHandler_ = std::move(handler);
    return true;
}

bool MqttEndpoint::bindTransportObservers(TransportObservers observers) {
    std::lock_guard lock(stateMutex_);
    if (state_ != MqttEndpointState::Constructed) return false;
    transportObservers_ = std::move(observers);
    return true;
}

bool MqttEndpoint::start(
    const MqttConnectionOptions& options,
    const std::vector<MqttSubscription>& subscriptions) {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    {
        std::lock_guard lock(stateMutex_);
        if (state_ != MqttEndpointState::Constructed || !transport_ ||
            !applicationHandler_) {
            return false;
        }
        state_ = MqttEndpointState::Running;
    }

    bool started{};
    try {
        started = transport_->start(
            options, subscriptions,
            [this](const std::string& topic, const std::string& payload) {
                dispatchApplication(topic, payload);
            },
            {[this] { dispatchConnected(); },
             [this](const int reason) { dispatchDisconnected(reason); },
             [this](const int messageId) { dispatchPublished(messageId); },
             [this](const MqttTransportFact& fact) {
                 dispatchTransportFact(fact);
             }});
    } catch (...) {
        started = false;
    }
    if (!started) {
        stopLocked(std::chrono::seconds(30));
        return false;
    }
    return true;
}

bool MqttEndpoint::quiesceIngress(
    const std::chrono::milliseconds timeout) noexcept {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    return quiesceIngressLocked(timeout);
}

bool MqttEndpoint::quiesceTransportObservers(
    const std::chrono::milliseconds timeout) noexcept {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    return quiesceTransportObserversLocked(timeout);
}

void MqttEndpoint::closeIngress() noexcept {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    closeIngressLocked();
}

void MqttEndpoint::closeIngressLocked() noexcept {
    applicationGate_.close();
    std::lock_guard lock(stateMutex_);
    if (state_ != MqttEndpointState::Stopped)
        state_ = MqttEndpointState::Quiescing;
}

bool MqttEndpoint::quiesceIngressLocked(
    const std::chrono::milliseconds timeout) noexcept {
    closeIngressLocked();
    return applicationGate_.waitForDrainedFor(timeout);
}

bool MqttEndpoint::quiesceTransportObserversLocked(
    const std::chrono::milliseconds timeout) noexcept {
    trackedEgressGate_.close();
    if (!trackedEgressGate_.waitForDrainedFor(timeout)) return false;
    {
        std::lock_guard lock(stateMutex_);
        transportObserversQuiesced_ = true;
    }
    transportObserverGate_.close();
    return transportObserverGate_.waitForDrainedFor(timeout);
}

bool MqttEndpoint::stop(const std::chrono::milliseconds timeout) noexcept {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    return stopLocked(timeout);
}

bool MqttEndpoint::stopLocked(
    const std::chrono::milliseconds timeout) noexcept {
    {
        std::lock_guard lock(stateMutex_);
        if (state_ == MqttEndpointState::Stopped) return true;
    }

    if (!quiesceIngressLocked(timeout)) return false;
    trackedEgressGate_.close();
    if (!trackedEgressGate_.waitForDrainedFor(timeout)) return false;
    if (!transportStopped_ && transport_) {
        if (!transport_->stopAndJoin()) return false;
        transportStopped_ = true;
    }
    if (!quiesceTransportObserversLocked(timeout)) return false;

    std::lock_guard lock(stateMutex_);
    applicationHandler_ = {};
    transportObservers_ = {};
    state_ = MqttEndpointState::Stopped;
    return true;
}

MqttPublishResult MqttEndpoint::publish(const std::string& topic,
                                        const std::string& payload,
                                        const int qos,
                                        const bool retain) {
    {
        std::lock_guard lock(stateMutex_);
        if (state_ != MqttEndpointState::Running &&
            state_ != MqttEndpointState::Quiescing) {
            return {};
        }
    }
    return transport_->publish(topic, payload, qos, retain);
}

MqttPublishResult MqttEndpoint::publishInEpoch(
    const std::string& topic,
    const std::string& payload,
    const int qos,
    const bool retain,
    const MqttConnectionEpoch expectedEpoch) {
    {
        std::lock_guard lock(stateMutex_);
        if ((state_ != MqttEndpointState::Running &&
             state_ != MqttEndpointState::Quiescing) ||
            expectedEpoch == 0) {
            return {};
        }
    }
    return transport_->publishInEpoch(
        topic, payload, qos, retain, expectedEpoch);
}

MqttTrackedPublishResult MqttEndpoint::publishTracked(
    const std::string& topic,
    const std::string& payload,
    const bool retain,
    MqttPublishCorrelation correlation,
    const MqttConnectionEpoch expectedEpoch) {
    auto lease = trackedEgressGate_.tryAcquire();
    if (!lease) return {};
    {
        std::lock_guard lock(stateMutex_);
        if ((state_ != MqttEndpointState::Running &&
             state_ != MqttEndpointState::Quiescing) ||
            transportObserversQuiesced_) {
            return {};
        }
    }
    return transport_->publishTracked(
        topic, payload, retain, std::move(correlation), expectedEpoch);
}

bool MqttEndpoint::abortActiveEpoch(
    const MqttConnectionEpoch expectedEpoch) noexcept {
    std::lock_guard operationLock(lifecycleOperationMutex_);
    {
        std::lock_guard lock(stateMutex_);
        if ((state_ != MqttEndpointState::Running &&
             state_ != MqttEndpointState::Quiescing) ||
            transportStopped_ || !transport_) {
            return false;
        }
    }
    return transport_->abortActiveEpoch(expectedEpoch);
}

MqttEndpointState MqttEndpoint::state() const noexcept {
    std::lock_guard lock(stateMutex_);
    return state_;
}

void MqttEndpoint::dispatchApplication(const std::string& topic,
                                       const std::string& payload) {
    auto lease = applicationGate_.tryAcquire();
    if (!lease) return;
    ApplicationHandler handler;
    {
        std::lock_guard lock(stateMutex_);
        handler = applicationHandler_;
    }
    if (handler) handler(topic, payload);
}

void MqttEndpoint::dispatchConnected() {
    auto lease = transportObserverGate_.tryAcquire();
    if (!lease) return;
    std::function<void()> observer;
    {
        std::lock_guard lock(stateMutex_);
        observer = transportObservers_.onConnected;
    }
    if (observer) observer();
}

void MqttEndpoint::dispatchDisconnected(const int reason) {
    auto lease = transportObserverGate_.tryAcquire();
    if (!lease) return;
    std::function<void(int)> observer;
    {
        std::lock_guard lock(stateMutex_);
        observer = transportObservers_.onDisconnected;
    }
    if (observer) observer(reason);
}

void MqttEndpoint::dispatchPublished(const int messageId) {
    auto lease = transportObserverGate_.tryAcquire();
    if (!lease) return;
    std::function<void(int)> observer;
    {
        std::lock_guard lock(stateMutex_);
        observer = transportObservers_.onPublished;
    }
    if (observer) observer(messageId);
}

void MqttEndpoint::dispatchTransportFact(const MqttTransportFact& fact) {
    auto lease = transportObserverGate_.tryAcquire();
    if (!lease) return;
    std::function<void(const MqttTransportFact&)> observer;
    {
        std::lock_guard lock(stateMutex_);
        observer = transportObservers_.onFact;
    }
    if (observer) observer(fact);
}

}  // namespace mqtt

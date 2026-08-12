#pragma once

#include "app/CallbackLeaseGate.hpp"
#include "mqtt/MqttTransport.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mqtt {

enum class MqttEndpointState { Constructed, Running, Quiescing, Stopped };

class MqttEndpoint {
public:
    using ApplicationHandler = IMqttTransport::MessageCallback;
    using TransportObservers = IMqttTransport::ObserverCallbacks;

    explicit MqttEndpoint(std::unique_ptr<IMqttTransport> transport);
    ~MqttEndpoint();
    MqttEndpoint(const MqttEndpoint&) = delete;
    MqttEndpoint& operator=(const MqttEndpoint&) = delete;

    bool bindApplicationHandler(ApplicationHandler handler);
    bool bindTransportObservers(TransportObservers observers);
    bool start(const MqttConnectionOptions& options,
               const std::vector<MqttSubscription>& subscriptions);
    void closeIngress() noexcept;
    [[nodiscard]] bool quiesceIngress(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;
    [[nodiscard]] bool quiesceTransportObservers(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;
    [[nodiscard]] bool stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;

    MqttPublishResult publish(const std::string& topic,
                              const std::string& payload,
                              int qos,
                              bool retain);
    MqttPublishResult publishInEpoch(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        MqttConnectionEpoch expectedEpoch);
    MqttTrackedPublishResult publishTracked(
        const std::string& topic,
        const std::string& payload,
        bool retain,
        MqttPublishCorrelation correlation,
        MqttConnectionEpoch expectedEpoch);
    bool abortActiveEpoch(MqttConnectionEpoch expectedEpoch) noexcept;
    [[nodiscard]] MqttEndpointState state() const noexcept;

private:
    void closeIngressLocked() noexcept;
    bool quiesceIngressLocked(std::chrono::milliseconds timeout) noexcept;
    bool quiesceTransportObserversLocked(
        std::chrono::milliseconds timeout) noexcept;
    bool stopLocked(std::chrono::milliseconds timeout) noexcept;
    void dispatchTransportFact(const MqttTransportFact& fact);
    void dispatchApplication(const std::string& topic,
                             const std::string& payload);
    void dispatchConnected();
    void dispatchDisconnected(int reason);
    void dispatchPublished(int messageId);

    std::unique_ptr<IMqttTransport> transport_;
    app::CallbackLeaseGate applicationGate_;
    app::CallbackLeaseGate trackedEgressGate_;
    app::CallbackLeaseGate transportObserverGate_;
    mutable std::mutex stateMutex_;
    std::mutex lifecycleOperationMutex_;
    MqttEndpointState state_{MqttEndpointState::Constructed};
    ApplicationHandler applicationHandler_;
    TransportObservers transportObservers_;
    bool transportStopped_{false};
    bool transportObserversQuiesced_{false};
};

}  // namespace mqtt

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mqtt {

struct MqttConnectionOptions {
    std::string clientId;
    std::string host;
    int port{1883};
    int keepAliveSeconds{60};
};

struct MqttSubscription {
    std::string topic;
    int qos{};
};

struct MqttPublishResult {
    bool accepted{};
    int messageId{-1};
};

using MqttConnectionEpoch = std::uint64_t;

/**
 * Durable delivery identity supplied by the outbox owner. MQTT MID is only a
 * connection-local transport identifier and must never replace this identity.
 */
struct MqttPublishCorrelation {
    std::string deliveryKey;
    std::uint64_t revision{};
    std::string attemptId;
};

struct MqttTrackedPublishToken {
    MqttConnectionEpoch epoch{};
    int messageId{-1};
    MqttPublishCorrelation correlation;

    [[nodiscard]] explicit operator bool() const noexcept {
        return epoch != 0 && messageId >= 0;
    }
};

struct MqttTrackedPublishResult {
    bool accepted{};
    MqttTrackedPublishToken token;
};

enum class MqttTransportFactKind {
    Connected,
    Disconnected,
    SubscriptionAcknowledged,
    SubscriptionsReady,
    PublishAcknowledged,
    EpochFaulted
};

/**
 * A callback-safe transport observation. Observer callbacks enqueue this fact;
 * they must not perform database work or apply durable delivery state inline.
 */
struct MqttTransportFact {
    MqttTransportFactKind kind{MqttTransportFactKind::EpochFaulted};
    MqttConnectionEpoch epoch{};
    int messageId{-1};
    int reason{};
    std::string subscriptionTopic;
    std::vector<int> grantedQos;
};

class IMqttTransport {
public:
    using MessageCallback =
        std::function<void(const std::string&, const std::string&)>;
    struct ObserverCallbacks {
        ObserverCallbacks() = default;
        ObserverCallbacks(
            std::function<void()> connected,
            std::function<void(int)> disconnected,
            std::function<void(int)> published,
            std::function<void(const MqttTransportFact&)> fact = {})
            : onConnected(std::move(connected)),
              onDisconnected(std::move(disconnected)),
              onPublished(std::move(published)),
              onFact(std::move(fact)) {}

        std::function<void()> onConnected;
        std::function<void(int)> onDisconnected;
        std::function<void(int)> onPublished;
        std::function<void(const MqttTransportFact&)> onFact;
    };

    virtual ~IMqttTransport() = default;
    virtual bool start(const MqttConnectionOptions& options,
                       const std::vector<MqttSubscription>& subscriptions,
                       MessageCallback messageCallback,
                       ObserverCallbacks observerCallbacks) = 0;
    virtual MqttPublishResult publish(const std::string& topic,
                                      const std::string& payload,
                                      int qos,
                                      bool retain) = 0;

    /**
     * Submit a regular QoS1 message only if the exact connection epoch is
     * still active and all configured subscriptions are ready.  The check and
     * mosquitto_publish() submission must share the transport's epoch lock so
     * a permit obtained before reconnect cannot leak into a fresh epoch.
     */
    virtual MqttPublishResult publishInEpoch(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        MqttConnectionEpoch expectedEpoch) {
        (void)topic;
        (void)payload;
        (void)qos;
        (void)retain;
        (void)expectedEpoch;
        return {};
    }

    /**
     * Submit one QoS1 message and return its immutable epoch/MID correlation.
     *
     * The default rejection keeps existing test transports source-compatible;
     * transports used by a durable outbox must override this method. A caller
     * must register the returned token in the same single-owner actor turn
     * before consuming queued PublishAcknowledged facts.
     */
    virtual MqttTrackedPublishResult publishTracked(
        const std::string& topic,
        const std::string& payload,
        bool retain,
        MqttPublishCorrelation correlation,
        MqttConnectionEpoch expectedEpoch) {
        (void)topic;
        (void)payload;
        (void)retain;
        (void)correlation;
        (void)expectedEpoch;
        return {};
    }
    /**
     * Retire exactly the currently active connection epoch. Durable delivery
     * owners use this after a fact overflow, MID collision, or PUBACK timeout;
     * the transport must create a fresh client handle before publishing again.
     */
    virtual bool abortActiveEpoch(MqttConnectionEpoch expectedEpoch) noexcept {
        (void)expectedEpoch;
        return false;
    }
    /** @return network loop와 모든 transport callback이 join됐을 때만 true. */
    virtual bool stopAndJoin() noexcept = 0;
};

std::unique_ptr<IMqttTransport> makeMosquittoTransport();

}  // namespace mqtt

#pragma once

#include "event/FirePersistence.hpp"
#include "mqtt/MqttTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace database {
class EventDatabase;
}

namespace event {

class FireDeliveryCoordinator {
public:
    struct RegularEgressGrant {
        mqtt::MqttConnectionEpoch epoch{};
        std::uint64_t generation{};
    };

    struct Config {
        std::vector<std::string> channelIds;
        std::size_t factQueueCapacity{256};
        std::chrono::milliseconds localFailureRetryDelay{
            std::chrono::milliseconds(100)};
        std::chrono::milliseconds pubAckTimeout{
            std::chrono::seconds(5)};
        std::chrono::milliseconds regularEgressDrainTimeout{
            std::chrono::seconds(1)};
    };

    using TrackedPublisher = std::function<mqtt::MqttTrackedPublishResult(
        const FireOutboxRecord&, mqtt::MqttPublishCorrelation,
        mqtt::MqttConnectionEpoch)>;
    using EpochFaultHandler =
        std::function<void(mqtt::MqttConnectionEpoch)>;

    FireDeliveryCoordinator(database::EventDatabase& database,
                            Config config,
                            TrackedPublisher publisher,
                            EpochFaultHandler fault_handler = {});
    ~FireDeliveryCoordinator();

    FireDeliveryCoordinator(const FireDeliveryCoordinator&) = delete;
    FireDeliveryCoordinator& operator=(const FireDeliveryCoordinator&) = delete;

    bool start();
    /** Callback-safe: queues only immutable transport facts, never touches DB. */
    bool enqueueTransportFact(const mqtt::MqttTransportFact& fact) noexcept;
    /** Called after a domain COMMIT; closes readiness before waking the actor. */
    void notifyOutboxChanged(const std::string& channel_id) noexcept;
    /** Closes readiness before a domain transaction can publish a new revision. */
    bool beginDomainMutation(const std::string& channel_id) noexcept;
    /** Re-enables actor reconciliation only after the transaction outcome. */
    void completeDomainMutation(const std::string& channel_id) noexcept;

    [[nodiscard]] bool isReady(const std::string& channel_id) const noexcept;
    [[nodiscard]] bool isRevisionSynchronized(
        const std::string& channel_id,
        std::uint64_t fire_revision) const noexcept;
    [[nodiscard]] bool allChannelsReady() const noexcept;
    [[nodiscard]] std::optional<RegularEgressGrant>
    beginRegularEgress() noexcept;
    [[nodiscard]] bool isRegularEgressGrantCurrent(
        const RegularEgressGrant& grant) const noexcept;
    void completeRegularEgress() noexcept;
    bool waitUntilReady(std::chrono::milliseconds timeout);

    /** Best-effort delivery deadline. False leaves all unacknowledged rows durable. */
    bool drainDeliveries(std::chrono::milliseconds timeout);
    /** Mandatory actor join; exact in-flight rows are returned to PENDING first. */
    bool stop();

private:
    struct AttemptKey {
        mqtt::MqttConnectionEpoch epoch{};
        int messageId{-1};

        auto operator<=>(const AttemptKey&) const = default;
    };

    struct Attempt {
        FireOutboxRecord delivery;
        std::string attemptId;
        std::chrono::steady_clock::time_point submittedAt;
    };

    void run() noexcept;
    void recoverActorFailure(const std::string& reason) noexcept;
    void processFact(const mqtt::MqttTransportFact& fact);
    void disconnectEpoch(mqtt::MqttConnectionEpoch epoch,
                         const std::string& reason);
    void pump();
    bool beginLifecycleRelease(std::uint64_t observed_generation);
    void endLifecycleRelease() noexcept;
    void expirePubAckDeadline();
    [[nodiscard]] std::chrono::steady_clock::time_point nextWakeAt() const;
    [[nodiscard]] bool retryDeferred(
        const std::string& channel_id,
        std::chrono::steady_clock::time_point now) const;
    void scheduleRetry(const std::string& channel_id);
    void clearRetry(const std::string& channel_id) noexcept;
    bool publish(const FireOutboxRecord& delivery);
    bool hasRetainedInFlight(const std::string& channel_id) const;
    bool hasLifecycleInFlight(const std::string& channel_id) const;
    void recomputeReadiness(
        const std::vector<FireOutboxRecord>& retained_rows,
        std::uint64_t observed_generation);
    void updateIdleState();
    void requeueAllInflight(const std::string& reason);
    void invalidateRegularEgressLocked() noexcept;
    void failCloseAllReadinessLocked() noexcept;

    database::EventDatabase& database_;
    Config config_;
    TrackedPublisher publisher_;
    EpochFaultHandler fault_handler_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable state_condition_;
    std::condition_variable regular_egress_condition_;
    std::deque<mqtt::MqttTransportFact> facts_;
    std::thread worker_;
    std::map<std::string, bool> ready_by_channel_;
    std::map<std::string, std::uint64_t> ready_revision_by_channel_;
    std::map<std::string, std::size_t> mutation_depth_by_channel_;
    bool running_{};
    bool accepting_facts_{};
    bool stop_requested_{};
    bool outbox_changed_{};
    bool fact_overflow_{};
    std::optional<mqtt::MqttConnectionEpoch> fact_overflow_epoch_;
    bool lifecycle_release_in_progress_{};
    std::size_t active_regular_egress_{};
    bool delivery_idle_{};
    std::uint64_t outbox_generation_{};
    std::uint64_t readiness_generation_{};
    mqtt::MqttConnectionEpoch regular_egress_epoch_{};
    std::uint64_t regular_egress_generation_{};

    mqtt::MqttConnectionEpoch active_epoch_{};
    bool subscriptions_ready_{};
    std::map<std::string, std::uint64_t> epoch_synced_revision_;
    std::map<AttemptKey, Attempt> in_flight_;
    std::uint64_t next_attempt_id_{1};
    std::map<std::string, std::chrono::steady_clock::time_point>
        retry_after_by_channel_;
};

}  // namespace event

#include "event/FireDeliveryCoordinator.hpp"

#include "database/EventDatabase.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace event {

FireDeliveryCoordinator::FireDeliveryCoordinator(
    database::EventDatabase& database,
    Config config,
    TrackedPublisher publisher,
    EpochFaultHandler fault_handler)
    : database_(database),
      config_(std::move(config)),
      publisher_(std::move(publisher)),
      fault_handler_(std::move(fault_handler)) {
    for (const auto& channel : config_.channelIds) {
        ready_by_channel_.emplace(channel, false);
        ready_revision_by_channel_.emplace(channel, 0);
        mutation_depth_by_channel_.emplace(channel, 0);
    }
}

FireDeliveryCoordinator::~FireDeliveryCoordinator() {
    if (!stop()) std::terminate();
}

bool FireDeliveryCoordinator::start() {
    std::lock_guard lock(mutex_);
    if (running_ || !publisher_ || config_.channelIds.empty() ||
        config_.factQueueCapacity == 0 ||
        config_.pubAckTimeout <= std::chrono::milliseconds::zero() ||
        config_.regularEgressDrainTimeout <=
            std::chrono::milliseconds::zero()) {
        return false;
    }
    std::set<std::string> configured;
    for (const auto& channel : config_.channelIds) {
        if (channel.empty() || !configured.insert(channel).second) return false;
    }
    const auto retained = database_.listFireRetainedDeliveries();
    if (retained.size() != configured.size()) return false;
    for (const auto& row : retained) {
        if (!configured.contains(row.channelId)) return false;
    }
    if (!database_.resetFireInFlightDeliveries()) return false;

    stop_requested_ = false;
    accepting_facts_ = true;
    running_ = true;
    failCloseAllReadinessLocked();
    try {
        worker_ = std::thread(&FireDeliveryCoordinator::run, this);
    } catch (...) {
        running_ = false;
        accepting_facts_ = false;
        return false;
    }
    return true;
}

bool FireDeliveryCoordinator::enqueueTransportFact(
    const mqtt::MqttTransportFact& fact) noexcept {
    std::lock_guard lock(mutex_);
    if (!running_ || !accepting_facts_) return false;
    const bool closes_readiness =
        fact.kind == mqtt::MqttTransportFactKind::Connected ||
        fact.kind == mqtt::MqttTransportFactKind::SubscriptionsReady ||
        fact.kind == mqtt::MqttTransportFactKind::Disconnected ||
        fact.kind == mqtt::MqttTransportFactKind::EpochFaulted;
    if (closes_readiness) failCloseAllReadinessLocked();
    if (facts_.size() >= config_.factQueueCapacity) {
        fact_overflow_ = true;
        if (fact.epoch != 0) fact_overflow_epoch_ = fact.epoch;
        failCloseAllReadinessLocked();
        condition_.notify_one();
        return false;
    }
    facts_.push_back(fact);
    condition_.notify_one();
    return true;
}

void FireDeliveryCoordinator::notifyOutboxChanged(
    const std::string& channel_id) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = ready_by_channel_.find(channel_id);
    if (found != ready_by_channel_.end()) {
        found->second = false;
        ready_revision_by_channel_[channel_id] = 0;
    }
    invalidateRegularEgressLocked();
    outbox_changed_ = true;
    condition_.notify_one();
    state_condition_.notify_all();
}

bool FireDeliveryCoordinator::beginDomainMutation(
    const std::string& channel_id) noexcept {
    std::unique_lock lock(mutex_);
    if (!running_ || stop_requested_) return false;
    const auto found = mutation_depth_by_channel_.find(channel_id);
    if (found == mutation_depth_by_channel_.end() ||
        lifecycle_release_in_progress_) {
        return false;
    }
    ++found->second;
    ready_by_channel_[channel_id] = false;
    ready_revision_by_channel_[channel_id] = 0;
    invalidateRegularEgressLocked();
    outbox_changed_ = true;
    condition_.notify_one();
    state_condition_.notify_all();
    const bool drained = regular_egress_condition_.wait_for(
        lock, config_.regularEgressDrainTimeout, [this] {
            return active_regular_egress_ == 0 || stop_requested_;
        });
    if (!drained || stop_requested_) {
        --found->second;
        invalidateRegularEgressLocked();
        outbox_changed_ = true;
        condition_.notify_one();
        state_condition_.notify_all();
        return false;
    }
    return true;
}

void FireDeliveryCoordinator::completeDomainMutation(
    const std::string& channel_id) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = mutation_depth_by_channel_.find(channel_id);
    if (found == mutation_depth_by_channel_.end() || found->second == 0)
        return;
    --found->second;
    invalidateRegularEgressLocked();
    outbox_changed_ = true;
    condition_.notify_one();
    state_condition_.notify_all();
}

bool FireDeliveryCoordinator::isReady(
    const std::string& channel_id) const noexcept {
    std::lock_guard lock(mutex_);
    const auto found = ready_by_channel_.find(channel_id);
    return found != ready_by_channel_.end() && found->second;
}

bool FireDeliveryCoordinator::isRevisionSynchronized(
    const std::string& channel_id,
    const std::uint64_t fire_revision) const noexcept {
    std::lock_guard lock(mutex_);
    const auto ready = ready_by_channel_.find(channel_id);
    const auto revision = ready_revision_by_channel_.find(channel_id);
    return ready != ready_by_channel_.end() && ready->second &&
           revision != ready_revision_by_channel_.end() &&
           revision->second == fire_revision;
}

bool FireDeliveryCoordinator::allChannelsReady() const noexcept {
    std::lock_guard lock(mutex_);
    return !ready_by_channel_.empty() &&
        std::all_of(ready_by_channel_.begin(), ready_by_channel_.end(),
                    [](const auto& item) { return item.second; });
}

std::optional<FireDeliveryCoordinator::RegularEgressGrant>
FireDeliveryCoordinator::beginRegularEgress() noexcept {
    std::lock_guard lock(mutex_);
    if (!running_ || stop_requested_ || ready_by_channel_.empty() ||
        !delivery_idle_ || regular_egress_epoch_ == 0 ||
        regular_egress_generation_ != readiness_generation_) {
        return std::nullopt;
    }
    const bool all_ready = std::all_of(
        ready_by_channel_.begin(), ready_by_channel_.end(),
        [](const auto& item) { return item.second; });
    const bool no_mutation = std::all_of(
        mutation_depth_by_channel_.begin(), mutation_depth_by_channel_.end(),
        [](const auto& item) { return item.second == 0; });
    if (!all_ready || !no_mutation) return std::nullopt;
    ++active_regular_egress_;
    return RegularEgressGrant{
        regular_egress_epoch_, regular_egress_generation_};
}

bool FireDeliveryCoordinator::isRegularEgressGrantCurrent(
    const RegularEgressGrant& grant) const noexcept {
    std::lock_guard lock(mutex_);
    if (!running_ || stop_requested_ || !delivery_idle_ ||
        grant.epoch == 0 || grant.epoch != regular_egress_epoch_ ||
        grant.generation != regular_egress_generation_ ||
        grant.generation != readiness_generation_) {
        return false;
    }
    return std::all_of(
               ready_by_channel_.begin(), ready_by_channel_.end(),
               [](const auto& item) { return item.second; }) &&
        std::all_of(
               mutation_depth_by_channel_.begin(),
               mutation_depth_by_channel_.end(),
               [](const auto& item) { return item.second == 0; });
}

void FireDeliveryCoordinator::completeRegularEgress() noexcept {
    std::lock_guard lock(mutex_);
    if (active_regular_egress_ == 0) return;
    --active_regular_egress_;
    regular_egress_condition_.notify_all();
}

bool FireDeliveryCoordinator::waitUntilReady(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return state_condition_.wait_for(lock, timeout, [this] {
        return !ready_by_channel_.empty() &&
            std::all_of(ready_by_channel_.begin(), ready_by_channel_.end(),
                        [](const auto& item) { return item.second; });
    });
}

bool FireDeliveryCoordinator::drainDeliveries(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return state_condition_.wait_for(lock, timeout, [this] {
        return delivery_idle_;
    });
}

bool FireDeliveryCoordinator::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return true;
        accepting_facts_ = false;
        stop_requested_ = true;
        failCloseAllReadinessLocked();
        condition_.notify_all();
        regular_egress_condition_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    running_ = false;
    return true;
}

void FireDeliveryCoordinator::run() noexcept {
    for (;;) {
        std::deque<mqtt::MqttTransportFact> facts;
        bool overflow{};
        std::optional<mqtt::MqttConnectionEpoch> overflow_epoch;
        {
            std::unique_lock lock(mutex_);
            const auto wake_at = nextWakeAt();
            if (wake_at == std::chrono::steady_clock::time_point{}) {
                condition_.wait(lock, [this] {
                    return stop_requested_ || !facts_.empty() ||
                           outbox_changed_ || fact_overflow_;
                });
            } else {
                condition_.wait_until(lock, wake_at, [this] {
                    return stop_requested_ || !facts_.empty() ||
                           outbox_changed_ || fact_overflow_;
                });
            }
            if (stop_requested_) break;
            facts.swap(facts_);
            outbox_changed_ = false;
            overflow = std::exchange(fact_overflow_, false);
            overflow_epoch = std::exchange(
                fact_overflow_epoch_, std::nullopt);
        }

        try {
            for (const auto& fact : facts) processFact(fact);
            if (overflow) {
                const auto faulted_epoch =
                    overflow_epoch.value_or(active_epoch_);
                if (faulted_epoch != 0) {
                    if (faulted_epoch == active_epoch_) {
                        disconnectEpoch(
                            faulted_epoch, "transport fact queue overflow");
                    }
                    if (fault_handler_) fault_handler_(faulted_epoch);
                }
            }
            expirePubAckDeadline();
            pump();
            updateIdleState();
        } catch (const std::exception& error) {
            recoverActorFailure(
                "Fire delivery actor exception: " +
                std::string(error.what()));
        } catch (...) {
            recoverActorFailure(
                "Fire delivery actor failed with an unknown exception");
        }
    }
    requeueAllInflight("delivery coordinator stopped before PUBACK");
}

void FireDeliveryCoordinator::recoverActorFailure(
    const std::string& reason) noexcept {
    util::logError(reason);
    const auto faulted_epoch = active_epoch_;
    try {
        if (faulted_epoch != 0) {
            disconnectEpoch(faulted_epoch, reason);
        } else {
            requeueAllInflight(reason);
        }
    } catch (...) {
        in_flight_.clear();
        active_epoch_ = 0;
        subscriptions_ready_ = false;
        epoch_synced_revision_.clear();
    }

    {
        std::lock_guard lock(mutex_);
        failCloseAllReadinessLocked();
    }
    for (const auto& channel : config_.channelIds) scheduleRetry(channel);
    if (faulted_epoch != 0 && fault_handler_) {
        try {
            fault_handler_(faulted_epoch);
        } catch (...) {
            util::logError("Fire epoch fault handler threw an exception");
        }
    }
}

void FireDeliveryCoordinator::processFact(
    const mqtt::MqttTransportFact& fact) {
    switch (fact.kind) {
    case mqtt::MqttTransportFactKind::Connected:
        if (fact.epoch < active_epoch_) return;
        if (active_epoch_ != 0 && fact.epoch != active_epoch_) {
            disconnectEpoch(active_epoch_, "fresh MQTT epoch connected");
        }
        active_epoch_ = fact.epoch;
        subscriptions_ready_ = false;
        epoch_synced_revision_.clear();
        {
            std::lock_guard lock(mutex_);
            failCloseAllReadinessLocked();
        }
        break;
    case mqtt::MqttTransportFactKind::SubscriptionsReady:
        if (fact.epoch == active_epoch_ && active_epoch_ != 0) {
            subscriptions_ready_ = true;
            retry_after_by_channel_.clear();
        }
        break;
    case mqtt::MqttTransportFactKind::Disconnected:
    case mqtt::MqttTransportFactKind::EpochFaulted:
        if (fact.epoch == active_epoch_) {
            disconnectEpoch(fact.epoch,
                            fact.kind ==
                                    mqtt::MqttTransportFactKind::Disconnected
                                ? "MQTT disconnected before PUBACK"
                                : "MQTT epoch faulted before PUBACK");
        }
        break;
    case mqtt::MqttTransportFactKind::PublishAcknowledged: {
        if (fact.epoch != active_epoch_) return;
        const AttemptKey key{fact.epoch, fact.messageId};
        const auto found = in_flight_.find(key);
        if (found == in_flight_.end()) return;
        const Attempt attempt = found->second;
        in_flight_.erase(found);
        const bool acknowledged = database_.acknowledgeFireDelivery(
            attempt.delivery.deliveryKey,
            attempt.delivery.fireRevision);
        if (acknowledged &&
            attempt.delivery.sinkKind ==
                FireDeliverySinkKind::RetainedState) {
            const auto current = database_.getFireDelivery(
                attempt.delivery.deliveryKey);
            if (current &&
                current->fireRevision == attempt.delivery.fireRevision) {
                epoch_synced_revision_[attempt.delivery.channelId] =
                    attempt.delivery.fireRevision;
            }
        }
        clearRetry(attempt.delivery.channelId);
        break;
    }
    case mqtt::MqttTransportFactKind::SubscriptionAcknowledged:
        break;
    }
}

void FireDeliveryCoordinator::disconnectEpoch(
    const mqtt::MqttConnectionEpoch epoch,
    const std::string& reason) {
    if (epoch == 0) return;
    for (auto iterator = in_flight_.begin(); iterator != in_flight_.end();) {
        if (iterator->first.epoch != epoch) {
            ++iterator;
            continue;
        }
        database_.markFireDeliveryPending(
            iterator->second.delivery.deliveryKey,
            iterator->second.delivery.fireRevision, reason);
        iterator = in_flight_.erase(iterator);
    }
    if (active_epoch_ == epoch) {
        active_epoch_ = 0;
        subscriptions_ready_ = false;
        epoch_synced_revision_.clear();
        retry_after_by_channel_.clear();
        std::lock_guard lock(mutex_);
        failCloseAllReadinessLocked();
    }
}

void FireDeliveryCoordinator::pump() {
    if (active_epoch_ == 0 || !subscriptions_ready_) return;
    const auto now = std::chrono::steady_clock::now();

    std::uint64_t observed_generation{};
    {
        std::lock_guard lock(mutex_);
        observed_generation = outbox_generation_;
    }
    const auto retained = database_.listFireRetainedDeliveries();
    bool retained_pending{};
    for (const auto& row : retained) {
        const auto synced = epoch_synced_revision_.find(row.channelId);
        if (synced != epoch_synced_revision_.end() &&
            synced->second == row.fireRevision) {
            continue;
        }
        retained_pending = true;
        if (retryDeferred(row.channelId, now) ||
            hasRetainedInFlight(row.channelId)) {
            continue;
        }
        (void)publish(row);
    }
    recomputeReadiness(retained, observed_generation);
    if (retained_pending || !allChannelsReady()) return;

    if (!beginLifecycleRelease(observed_generation)) return;
    try {
        const auto lifecycle = database_.listPendingFireLifecycleDeliveries();
        std::set<std::string> submitted_channels;
        for (const auto& row : lifecycle) {
            if (!submitted_channels.insert(row.channelId).second) continue;
            if (retryDeferred(row.channelId, now) ||
                hasLifecycleInFlight(row.channelId)) {
                continue;
            }
            (void)publish(row);
        }
        endLifecycleRelease();
    } catch (...) {
        endLifecycleRelease();
        throw;
    }
}

bool FireDeliveryCoordinator::beginLifecycleRelease(
    const std::uint64_t observed_generation) {
    std::lock_guard lock(mutex_);
    if (lifecycle_release_in_progress_ ||
        observed_generation != outbox_generation_ || active_epoch_ == 0 ||
        !subscriptions_ready_) {
        return false;
    }
    const bool all_ready = !ready_by_channel_.empty() &&
        std::all_of(ready_by_channel_.begin(), ready_by_channel_.end(),
                    [](const auto& item) { return item.second; });
    const bool no_mutation = std::all_of(
        mutation_depth_by_channel_.begin(), mutation_depth_by_channel_.end(),
        [](const auto& item) { return item.second == 0; });
    if (!all_ready || !no_mutation) return false;
    lifecycle_release_in_progress_ = true;
    return true;
}

void FireDeliveryCoordinator::endLifecycleRelease() noexcept {
    std::lock_guard lock(mutex_);
    lifecycle_release_in_progress_ = false;
    condition_.notify_all();
    state_condition_.notify_all();
}

bool FireDeliveryCoordinator::publish(const FireOutboxRecord& delivery) {
    if (!database_.markFireDeliveryInFlight(
            delivery.deliveryKey, delivery.fireRevision)) {
        scheduleRetry(delivery.channelId);
        return false;
    }
    const std::string attempt_id =
        "fire-attempt:" + std::to_string(next_attempt_id_++);
    const auto expected_epoch = active_epoch_;
    const auto result = publisher_(
        delivery,
        {delivery.deliveryKey, delivery.fireRevision, attempt_id},
        expected_epoch);
    if (!result.accepted || result.token.epoch != expected_epoch ||
        active_epoch_ != expected_epoch ||
        result.token.messageId < 0 ||
        result.token.correlation.deliveryKey != delivery.deliveryKey ||
        result.token.correlation.revision != delivery.fireRevision ||
        result.token.correlation.attemptId != attempt_id) {
        database_.markFireDeliveryPending(
            delivery.deliveryKey, delivery.fireRevision,
            "MQTT tracked publish was not accepted in the active epoch");
        scheduleRetry(delivery.channelId);
        return false;
    }
    const AttemptKey key{result.token.epoch, result.token.messageId};
    if (in_flight_.contains(key)) {
        database_.markFireDeliveryPending(
            delivery.deliveryKey, delivery.fireRevision,
            "MQTT reused an in-flight MID in the same epoch");
        const auto faulted_epoch = active_epoch_;
        disconnectEpoch(active_epoch_, "same-epoch MQTT MID collision");
        if (fault_handler_) fault_handler_(faulted_epoch);
        return false;
    }
    in_flight_.emplace(
        key, Attempt{delivery, attempt_id, std::chrono::steady_clock::now()});
    clearRetry(delivery.channelId);
    return true;
}

void FireDeliveryCoordinator::expirePubAckDeadline() {
    if (active_epoch_ == 0 || in_flight_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto expired = std::find_if(
        in_flight_.begin(), in_flight_.end(), [&](const auto& item) {
            return item.second.submittedAt + config_.pubAckTimeout <= now;
        });
    if (expired == in_flight_.end()) return;
    const auto faulted_epoch = active_epoch_;
    disconnectEpoch(faulted_epoch, "MQTT PUBACK deadline expired");
    if (fault_handler_) fault_handler_(faulted_epoch);
}

std::chrono::steady_clock::time_point
FireDeliveryCoordinator::nextWakeAt() const {
    auto wake_at = std::chrono::steady_clock::time_point{};
    for (const auto& [channel, retry_after] : retry_after_by_channel_) {
        (void)channel;
        if (wake_at == std::chrono::steady_clock::time_point{} ||
            retry_after < wake_at) {
            wake_at = retry_after;
        }
    }
    for (const auto& [key, attempt] : in_flight_) {
        (void)key;
        const auto deadline = attempt.submittedAt + config_.pubAckTimeout;
        if (wake_at == std::chrono::steady_clock::time_point{} ||
            deadline < wake_at) {
            wake_at = deadline;
        }
    }
    return wake_at;
}

bool FireDeliveryCoordinator::retryDeferred(
    const std::string& channel_id,
    const std::chrono::steady_clock::time_point now) const {
    const auto found = retry_after_by_channel_.find(channel_id);
    return found != retry_after_by_channel_.end() && found->second > now;
}

void FireDeliveryCoordinator::scheduleRetry(
    const std::string& channel_id) {
    retry_after_by_channel_[channel_id] =
        std::chrono::steady_clock::now() + config_.localFailureRetryDelay;
}

void FireDeliveryCoordinator::clearRetry(
    const std::string& channel_id) noexcept {
    retry_after_by_channel_.erase(channel_id);
}

bool FireDeliveryCoordinator::hasRetainedInFlight(
    const std::string& channel_id) const {
    return std::any_of(in_flight_.begin(), in_flight_.end(),
                       [&](const auto& entry) {
        return entry.second.delivery.channelId == channel_id &&
               entry.second.delivery.sinkKind ==
                   FireDeliverySinkKind::RetainedState;
    });
}

bool FireDeliveryCoordinator::hasLifecycleInFlight(
    const std::string& channel_id) const {
    return std::any_of(in_flight_.begin(), in_flight_.end(),
                       [&](const auto& entry) {
        return entry.second.delivery.channelId == channel_id &&
               entry.second.delivery.sinkKind ==
                   FireDeliverySinkKind::LifecycleEvent;
    });
}

void FireDeliveryCoordinator::recomputeReadiness(
    const std::vector<FireOutboxRecord>& retained_rows,
    const std::uint64_t observed_generation) {
    std::lock_guard lock(mutex_);
    bool all_ready = !ready_by_channel_.empty();
    for (auto& [channel, ready] : ready_by_channel_) {
        const auto row = std::find_if(
            retained_rows.begin(), retained_rows.end(),
            [&](const auto& candidate) {
                return candidate.channelId == channel;
            });
        const auto synced = epoch_synced_revision_.find(channel);
        const auto mutation = mutation_depth_by_channel_.find(channel);
        ready = observed_generation == outbox_generation_ &&
                mutation != mutation_depth_by_channel_.end() &&
                mutation->second == 0 && active_epoch_ != 0 &&
                subscriptions_ready_ &&
                row != retained_rows.end() &&
                synced != epoch_synced_revision_.end() &&
                synced->second == row->fireRevision;
        ready_revision_by_channel_[channel] =
            ready ? row->fireRevision : 0;
        all_ready = all_ready && ready;
    }
    regular_egress_epoch_ = all_ready ? active_epoch_ : 0;
    regular_egress_generation_ = readiness_generation_;
    state_condition_.notify_all();
}

void FireDeliveryCoordinator::updateIdleState() {
    const bool pending_lifecycle =
        !database_.listPendingFireLifecycleDeliveries().empty();
    std::lock_guard lock(mutex_);
    const bool ready = !ready_by_channel_.empty() &&
        std::all_of(ready_by_channel_.begin(), ready_by_channel_.end(),
                    [](const auto& item) { return item.second; });
    delivery_idle_ = ready && !pending_lifecycle && in_flight_.empty();
    state_condition_.notify_all();
}

void FireDeliveryCoordinator::requeueAllInflight(const std::string& reason) {
    for (const auto& [key, attempt] : in_flight_) {
        (void)key;
        database_.markFireDeliveryPending(
            attempt.delivery.deliveryKey,
            attempt.delivery.fireRevision, reason);
    }
    in_flight_.clear();
}

void FireDeliveryCoordinator::invalidateRegularEgressLocked() noexcept {
    ++outbox_generation_;
    ++readiness_generation_;
    if (readiness_generation_ == 0) ++readiness_generation_;
    regular_egress_epoch_ = 0;
    regular_egress_generation_ = 0;
    delivery_idle_ = false;
    state_condition_.notify_all();
}

void FireDeliveryCoordinator::failCloseAllReadinessLocked() noexcept {
    for (auto& [channel, ready] : ready_by_channel_) {
        ready = false;
        ready_revision_by_channel_[channel] = 0;
    }
    invalidateRegularEgressLocked();
}

}  // namespace event

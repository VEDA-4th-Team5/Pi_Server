#include "database/EventDatabase.hpp"
#include "event/FireAlarmService.hpp"
#include "event/FireDeliveryCoordinator.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path databasePath(const std::string& name) {
    return std::filesystem::temp_directory_path() /
        ("pi-fire-delivery-" + name + '-' +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()) + ".db");
}

void executeExternalSql(const std::filesystem::path& path,
                        const char* sql) {
    sqlite3* connection{};
    require(sqlite3_open(path.string().c_str(), &connection) == SQLITE_OK,
            "external SQLite open failed");
    char* raw_error{};
    const int result =
        sqlite3_exec(connection, sql, nullptr, nullptr, &raw_error);
    const std::string error = raw_error ? raw_error : "unknown SQLite error";
    sqlite3_free(raw_error);
    sqlite3_close(connection);
    require(result == SQLITE_OK, "external SQLite statement failed: " + error);
}

template <typename Predicate>
void requireEventually(Predicate predicate,
                       const std::chrono::milliseconds timeout,
                       const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(predicate(), message);
}

class ResultCollector {
public:
    void add(const event::FireCommandResult& result) {
        std::lock_guard lock(mutex_);
        results_[result.ticket] = result;
        condition_.notify_all();
    }

    event::FireCommandResult wait(const std::uint64_t ticket) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 3s, [&] {
                return results_.contains(ticket);
            })) {
            throw std::runtime_error("Fire command result timeout");
        }
        return results_.at(ticket);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::map<std::uint64_t, event::FireCommandResult> results_;
};

event::FireSignal fireSignal(const bool detected,
                             const std::uint64_t sequence) {
    event::FireSignal signal;
    signal.sensorId = "F1";
    signal.detected = detected;
    signal.occurredAt = std::chrono::system_clock::now();
    signal.sourceSequence = sequence;
    signal.sourceTransport = "uart";
    signal.rawPayload = std::string("FIRE:F1:") +
        (detected ? "DETECTED:" : "CLEARED:") +
        std::to_string(sequence);
    return signal;
}

class FakeTrackedPublisher {
public:
    struct Submission {
        event::FireOutboxRecord delivery;
        mqtt::MqttPublishCorrelation correlation;
        mqtt::MqttTrackedPublishToken token;
        bool accepted{};
    };

    mqtt::MqttTrackedPublishResult publish(
        const event::FireOutboxRecord& delivery,
        mqtt::MqttPublishCorrelation correlation,
        const mqtt::MqttConnectionEpoch expectedEpoch) {
        mqtt::MqttTrackedPublishToken token;
        bool accepted{};
        bool inline_ack{};
        event::FireDeliveryCoordinator* coordinator{};
        {
            std::unique_lock lock(mutex_);
            if (blockNextPublish_) {
                blockNextPublish_ = false;
                publishBlocked_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this] { return releasePublish_; });
                publishBlocked_ = false;
                releasePublish_ = false;
            }
            token = {epoch_, reusedMid_, correlation};
            accepted = accept_ && expectedEpoch == epoch_ &&
                delivery.channelId != rejectedChannel_;
            inline_ack = inlineAck_ && accepted;
            coordinator = coordinator_;
            submissions_.push_back(
                {delivery, correlation, token, accepted});
            condition_.notify_all();
        }
        if (inline_ack && coordinator) {
            coordinator->enqueueTransportFact(
                {mqtt::MqttTransportFactKind::PublishAcknowledged,
                 token.epoch, token.messageId, 0, {}, {}});
        }
        return accepted ? mqtt::MqttTrackedPublishResult{true, token}
                        : mqtt::MqttTrackedPublishResult{};
    }

    void bind(event::FireDeliveryCoordinator* coordinator) {
        std::lock_guard lock(mutex_);
        coordinator_ = coordinator;
    }

    void setEpoch(const mqtt::MqttConnectionEpoch epoch) {
        std::lock_guard lock(mutex_);
        epoch_ = epoch;
    }

    void setAccept(const bool accept) {
        std::lock_guard lock(mutex_);
        accept_ = accept;
    }

    void setRejectedChannel(std::string channel) {
        std::lock_guard lock(mutex_);
        rejectedChannel_ = std::move(channel);
    }

    void setInlineAck(const bool enabled) {
        std::lock_guard lock(mutex_);
        inlineAck_ = enabled;
    }

    void blockNextPublish() {
        std::lock_guard lock(mutex_);
        blockNextPublish_ = true;
    }

    void waitUntilPublishBlocked() {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 3s, [this] {
                return publishBlocked_;
            })) {
            throw std::runtime_error("tracked publisher did not block");
        }
    }

    void releaseBlockedPublish() {
        {
            std::lock_guard lock(mutex_);
            releasePublish_ = true;
        }
        condition_.notify_all();
    }

    Submission waitFor(const std::size_t index) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 3s, [&] {
                return submissions_.size() > index;
            })) {
            throw std::runtime_error("tracked publish timeout at index " +
                                     std::to_string(index));
        }
        return submissions_.at(index);
    }

    bool hasSubmission(const std::size_t index,
                       const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            return submissions_.size() > index;
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Submission> submissions_;
    event::FireDeliveryCoordinator* coordinator_{};
    mqtt::MqttConnectionEpoch epoch_{1};
    int reusedMid_{7};
    bool accept_{true};
    bool inlineAck_{};
    bool blockNextPublish_{};
    bool publishBlocked_{};
    bool releasePublish_{};
    std::string rejectedChannel_;
};

void connectEpoch(event::FireDeliveryCoordinator& coordinator,
                  FakeTrackedPublisher& publisher,
                  const mqtt::MqttConnectionEpoch epoch) {
    publisher.setEpoch(epoch);
    require(coordinator.enqueueTransportFact(
                {mqtt::MqttTransportFactKind::Connected,
                 epoch, -1, 0, {}, {}}),
            "Connected fact must be accepted");
    require(coordinator.enqueueTransportFact(
                {mqtt::MqttTransportFactKind::SubscriptionsReady,
                 epoch, -1, 0, {}, {}}),
            "SubscriptionsReady fact must be accepted");
}

void acknowledge(event::FireDeliveryCoordinator& coordinator,
                 const FakeTrackedPublisher::Submission& submission) {
    require(coordinator.enqueueTransportFact(
                {mqtt::MqttTransportFactKind::PublishAcknowledged,
                 submission.token.epoch, submission.token.messageId,
                 0, {}, {}}),
            "PUBACK fact must be accepted");
}

void testRetainedBarrierRevisionReplacementAndEpochReuse() {
    const auto path = databasePath("barrier");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireDeliveryCoordinator* coordinator_ptr{};
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
                if (coordinator_ptr &&
                    (result.status ==
                         event::FireCommandStatus::DurablyCommitted ||
                     result.status == event::FireCommandStatus::Idempotent)) {
                    coordinator_ptr->notifyOutboxChanged(result.channelId);
                }
            },
            [&](const std::string& channel, const std::uint64_t revision) {
                return coordinator_ptr &&
                       coordinator_ptr->isRevisionSynchronized(
                           channel, revision);
            },
            [&](const std::string& channel) {
                return !coordinator_ptr ||
                       coordinator_ptr->beginDomainMutation(channel);
            },
            [&](const std::string& channel) {
                if (coordinator_ptr)
                    coordinator_ptr->completeDomainMutation(channel);
            });
        require(service.initialize() && service.start(),
                "Fire domain service must start");
        const auto open = service.submitSignal(fireSignal(true, 1));
        require(collector.wait(open.ticket).fireRevision == 2,
                "OPEN revision must be committed before transport starts");

        FakeTrackedPublisher publisher;
        event::FireDeliveryCoordinator::Config delivery_config;
        delivery_config.channelIds = {"ch01"};
        delivery_config.localFailureRetryDelay = 50ms;
        event::FireDeliveryCoordinator coordinator(
            database, delivery_config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        coordinator_ptr = &coordinator;
        publisher.bind(&coordinator);
        require(coordinator.start(), "Fire delivery coordinator must start");
        connectEpoch(coordinator, publisher, 1);

        const auto retained_open = publisher.waitFor(0);
        require(retained_open.delivery.sinkKind ==
                    event::FireDeliverySinkKind::RetainedState &&
                    retained_open.delivery.fireRevision == 2,
                "latest retained OPEN must be first after SUBACK barrier");
        require(!publisher.hasSubmission(1, 100ms),
                "lifecycle history must wait for exact retained PUBACK");
        require(!coordinator.isReady("ch01"),
                "Fire ACK readiness must remain closed before retained PUBACK");

        acknowledge(coordinator, retained_open);
        const auto lifecycle_open = publisher.waitFor(1);
        require(lifecycle_open.delivery.sinkKind ==
                    event::FireDeliverySinkKind::LifecycleEvent &&
                    lifecycle_open.delivery.fireRevision == 2,
                "OPEN history must follow retained synchronization");
        require(coordinator.waitUntilReady(2s),
                "current retained revision PUBACK must open readiness");
        require(!coordinator.beginRegularEgress(),
                "regular MQTT must remain closed until lifecycle PUBACK");
        acknowledge(coordinator, lifecycle_open);
        require(coordinator.drainDeliveries(2s),
                "acknowledged retained and history must become idle");
        const auto first_regular_grant = coordinator.beginRegularEgress();
        require(first_regular_grant.has_value() &&
                    first_regular_grant->epoch == 1 &&
                    coordinator.isRegularEgressGrantCurrent(
                        *first_regular_grant),
                "regular MQTT must open after both Fire sinks drain");
        coordinator.completeRegularEgress();

        const auto clear = service.submitSignal(fireSignal(false, 2));
        require(collector.wait(clear.ticket).fireRevision == 3,
                "CLEAR revision must commit");
        const auto retained_clear = publisher.waitFor(2);
        require(retained_clear.delivery.fireRevision == 3,
                "CLEAR must reopen retained synchronization");

        const auto reopen = service.submitSignal(fireSignal(true, 3));
        require(collector.wait(reopen.ticket).fireRevision == 4,
                "new OPEN must replace durable retained CLEAR");
        acknowledge(coordinator, retained_clear);
        const auto retained_reopen_epoch1 = publisher.waitFor(3);
        require(retained_reopen_epoch1.delivery.fireRevision == 4,
                "old revision PUBACK must trigger latest retained revision");
        const auto durable_after_old_ack =
            database.listFireRetainedDeliveries();
        require(durable_after_old_ack.size() == 1 &&
                    durable_after_old_ack[0].fireRevision == 4 &&
                    durable_after_old_ack[0].deliveryState !=
                        event::FireDeliveryState::Acknowledged,
                "revision 3 PUBACK must not acknowledge revision 4");

        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Disconnected,
                     1, -1, 1, {}, {}}),
                "disconnect fact must be accepted");
        connectEpoch(coordinator, publisher, 2);
        const auto retained_reopen_epoch2 = publisher.waitFor(4);
        require(retained_reopen_epoch2.token.epoch == 2 &&
                    retained_reopen_epoch2.token.messageId == 7 &&
                    retained_reopen_epoch2.delivery.fireRevision == 4,
                "fresh epoch must resync latest retained with reusable MID");

        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::PublishAcknowledged,
                     1, 7, 0, {}, {}}),
                "late old-epoch PUBACK fact must be queueable");
        require(!coordinator.waitUntilReady(100ms),
                "old epoch PUBACK must not open current readiness");
        acknowledge(coordinator, retained_reopen_epoch2);
        require(coordinator.waitUntilReady(2s),
                "epoch 2 exact retained PUBACK must open readiness");

        const auto lifecycle_clear = publisher.waitFor(5);
        require(lifecycle_clear.delivery.fireRevision == 3 &&
                    lifecycle_clear.delivery.sinkKind ==
                        event::FireDeliverySinkKind::LifecycleEvent,
                "history replay must preserve revision order after reconnect");
        acknowledge(coordinator, lifecycle_clear);
        const auto lifecycle_reopen = publisher.waitFor(6);
        require(lifecycle_reopen.delivery.fireRevision == 4,
                "newer lifecycle history must follow CLEAR");
        acknowledge(coordinator, lifecycle_reopen);
        require(coordinator.drainDeliveries(2s),
                "all exact PUBACKs must drain the outbox");
        require(coordinator.stop(), "delivery coordinator must join");
        require(!coordinator.isReady("ch01") &&
                    !coordinator.beginRegularEgress(),
                "stopped coordinator exposed stale MQTT readiness");
        coordinator_ptr = nullptr;
        require(service.stop(), "Fire domain service must join");
    }
    std::filesystem::remove(path);
}

void testImmediateAckAndLocalFailureRemainDurable() {
    const auto path = databasePath("failure");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize() && service.start(),
                "Fire failure fixture must start");

        FakeTrackedPublisher publisher;
        publisher.setInlineAck(true);
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.localFailureRetryDelay = 1s;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        publisher.bind(&coordinator);
        require(coordinator.start(), "coordinator must start");
        connectEpoch(coordinator, publisher, 1);
        const auto bootstrap = publisher.waitFor(0);
        require(bootstrap.delivery.fireRevision == 1,
                "bootstrap retained state must publish");
        require(coordinator.waitUntilReady(2s),
                "inline PUBACK must be applied after token registration");

        require(coordinator.stop(), "inline ACK coordinator must stop");

        FakeTrackedPublisher rejecting_publisher;
        rejecting_publisher.setAccept(false);
        event::FireDeliveryCoordinator rejecting(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return rejecting_publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        rejecting_publisher.bind(&rejecting);
        require(rejecting.start(), "rejecting coordinator must start");
        connectEpoch(rejecting, rejecting_publisher, 2);
        const auto rejected = rejecting_publisher.waitFor(0);
        require(!rejected.accepted,
                "fixture must reject local tracked publish");
        require(!rejecting.drainDeliveries(100ms),
                "delivery deadline must expire while broker publish rejects");
        const auto retained = database.listFireRetainedDeliveries();
        require(retained.size() == 1 &&
                    retained[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "local publish failure must leave retained work durable");
        require(rejecting.stop(), "rejecting coordinator must join");
        require(service.stop(), "Fire service must join");
    }
    std::filesystem::remove(path);
}

void testMissingPubAckFaultsEpochAndRequeuesDurably() {
    const auto path = databasePath("puback-timeout");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize(), "PUBACK timeout fixture must bootstrap");

        FakeTrackedPublisher publisher;
        std::mutex fault_mutex;
        std::condition_variable fault_condition;
        mqtt::MqttConnectionEpoch faulted_epoch{};
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.pubAckTimeout = 50ms;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            },
            [&](const mqtt::MqttConnectionEpoch epoch) {
                {
                    std::lock_guard lock(fault_mutex);
                    faulted_epoch = epoch;
                }
                fault_condition.notify_all();
            });
        publisher.bind(&coordinator);
        require(coordinator.start(), "PUBACK timeout coordinator must start");
        connectEpoch(coordinator, publisher, 1);
        const auto retained = publisher.waitFor(0);
        require(retained.delivery.sinkKind ==
                    event::FireDeliverySinkKind::RetainedState,
                "timeout fixture must first submit retained state");

        {
            std::unique_lock lock(fault_mutex);
            require(fault_condition.wait_for(lock, 2s, [&] {
                        return faulted_epoch == 1;
                    }),
                    "withheld PUBACK did not fault the active epoch");
        }
        const auto durable = database.listFireRetainedDeliveries();
        require(durable.size() == 1 &&
                    durable[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "PUBACK timeout must return exact delivery to PENDING");
        require(!coordinator.isReady("ch01"),
                "timed-out epoch must never leave ACK readiness open");
        require(coordinator.stop(), "timeout coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testLifecycleReleaseExcludesConcurrentDomainCommit() {
    const auto path = databasePath("lifecycle-gate");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize() && service.start(),
                "lifecycle gate fixture must start");
        const auto open = service.submitSignal(fireSignal(true, 1));
        require(collector.wait(open.ticket).fireRevision == 2,
                "lifecycle gate OPEN must commit before delivery starts");
        require(service.stop(), "lifecycle gate domain worker must stop");

        std::mutex publish_mutex;
        std::condition_variable publish_condition;
        std::vector<FakeTrackedPublisher::Submission> submissions;
        bool lifecycle_entered{};
        bool release_lifecycle{};
        int next_mid{1};
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.pubAckTimeout = 2s;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                std::unique_lock lock(publish_mutex);
                const mqtt::MqttTrackedPublishToken token{
                    expected_epoch, next_mid++, correlation};
                submissions.push_back(
                    {delivery, correlation, token, true});
                publish_condition.notify_all();
                if (delivery.sinkKind ==
                    event::FireDeliverySinkKind::LifecycleEvent) {
                    lifecycle_entered = true;
                    publish_condition.notify_all();
                    publish_condition.wait(lock, [&] {
                        return release_lifecycle;
                    });
                }
                return mqtt::MqttTrackedPublishResult{true, token};
            },
            [](mqtt::MqttConnectionEpoch) {});
        require(coordinator.start(), "lifecycle gate coordinator must start");
        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Connected,
                     1, -1, 0, {}, {}}) &&
                    coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::SubscriptionsReady,
                     1, -1, 0, {}, {}}),
                "lifecycle gate epoch facts must enqueue");

        FakeTrackedPublisher::Submission retained;
        {
            std::unique_lock lock(publish_mutex);
            require(publish_condition.wait_for(lock, 2s, [&] {
                        return !submissions.empty();
                    }),
                    "retained OPEN was not submitted");
            retained = submissions.front();
        }
        acknowledge(coordinator, retained);

        FakeTrackedPublisher::Submission lifecycle;
        {
            std::unique_lock lock(publish_mutex);
            require(publish_condition.wait_for(lock, 2s, [&] {
                        return lifecycle_entered && submissions.size() >= 2;
                    }),
                    "lifecycle publish did not enter the release gate");
            lifecycle = submissions[1];
        }
        require(!coordinator.beginDomainMutation("ch01"),
                "domain commit entered while lifecycle release used an old "
                "retained snapshot");
        {
            std::lock_guard lock(publish_mutex);
            release_lifecycle = true;
        }
        publish_condition.notify_all();
        acknowledge(coordinator, lifecycle);
        require(coordinator.drainDeliveries(2s),
                "lifecycle gate fixture did not drain");
        require(coordinator.stop(), "lifecycle gate coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testRetainedLocalFailureDoesNotBlockAnotherChannel() {
    const auto path = databasePath("retained-channel-isolation");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"},
             {"F2", "ch02", "parking/fire/ch02"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize(),
                "two-channel Fire topology must initialize");

        FakeTrackedPublisher publisher;
        publisher.setRejectedChannel("ch01");
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01", "ch02"};
        config.localFailureRetryDelay = 5s;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        publisher.bind(&coordinator);
        require(coordinator.start(),
                "two-channel delivery coordinator must start");
        connectEpoch(coordinator, publisher, 1);

        const auto rejected = publisher.waitFor(0);
        const auto accepted = publisher.waitFor(1);
        require(rejected.delivery.channelId == "ch01" &&
                    !rejected.accepted,
                "fixture must reject the first retained channel");
        require(accepted.delivery.channelId == "ch02" &&
                    accepted.accepted,
                "one channel failure must not block another retained state");
        acknowledge(coordinator, accepted);
        requireEventually(
            [&] { return coordinator.isRevisionSynchronized("ch02", 1); },
            2s,
            "healthy channel retained PUBACK was not applied independently");
        require(!coordinator.isReady("ch01") &&
                    !coordinator.allChannelsReady(),
                "global Fire readiness must remain closed for failed channel");
        require(coordinator.stop(),
                "two-channel delivery coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testFactOverflowFaultsTheDroppedConnectionEpoch() {
    const auto path = databasePath("fact-overflow-epoch");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize(), "overflow topology must initialize");

        std::mutex publish_mutex;
        std::condition_variable publish_condition;
        bool publish_entered{};
        bool release_publish{};
        std::mutex fault_mutex;
        std::condition_variable fault_condition;
        mqtt::MqttConnectionEpoch faulted_epoch{};
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.factQueueCapacity = 2;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                (void)delivery;
                std::unique_lock lock(publish_mutex);
                publish_entered = true;
                publish_condition.notify_all();
                publish_condition.wait(lock, [&] { return release_publish; });
                return mqtt::MqttTrackedPublishResult{
                    true, {expected_epoch, 7, std::move(correlation)}};
            },
            [&](const mqtt::MqttConnectionEpoch epoch) {
                {
                    std::lock_guard lock(fault_mutex);
                    faulted_epoch = epoch;
                }
                fault_condition.notify_all();
            });
        require(coordinator.start(), "overflow coordinator must start");
        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Connected,
                     1, -1, 0, {}, {}}) &&
                    coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::SubscriptionsReady,
                     1, -1, 0, {}, {}}),
                "overflow fixture connection facts must enqueue");
        {
            std::unique_lock lock(publish_mutex);
            require(publish_condition.wait_for(lock, 2s, [&] {
                        return publish_entered;
                    }),
                    "overflow fixture did not enter retained publish");
        }

        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Disconnected,
                     1, -1, 1, {}, {}}),
                "disconnect must fill the blocked actor queue");
        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::SubscriptionAcknowledged,
                     1, 91, 0, "ignored", {1}}),
                "second fact must fill the blocked actor queue");
        require(!coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Connected,
                     2, -1, 0, {}, {}}),
                "new epoch fact must exercise bounded overflow");
        {
            std::lock_guard lock(publish_mutex);
            release_publish = true;
        }
        publish_condition.notify_all();
        {
            std::unique_lock lock(fault_mutex);
            require(fault_condition.wait_for(lock, 2s, [&] {
                        return faulted_epoch == 2;
                    }),
                    "overflow did not retire the exact dropped epoch");
        }
        require(!coordinator.isReady("ch01"),
                "fact overflow must leave Fire readiness fail-closed");
        require(coordinator.stop(), "overflow coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testActorDatabaseExceptionFailsClosedAndRecovers() {
    const auto path = databasePath("actor-db-recovery");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize(), "DB recovery topology must initialize");

        FakeTrackedPublisher publisher;
        std::mutex fault_mutex;
        std::condition_variable fault_condition;
        mqtt::MqttConnectionEpoch faulted_epoch{};
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.localFailureRetryDelay = 25ms;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            },
            [&](const mqtt::MqttConnectionEpoch epoch) {
                {
                    std::lock_guard lock(fault_mutex);
                    faulted_epoch = epoch;
                }
                fault_condition.notify_all();
            });
        publisher.bind(&coordinator);
        require(coordinator.start(), "DB recovery coordinator must start");
        connectEpoch(coordinator, publisher, 1);
        const auto first = publisher.waitFor(0);
        acknowledge(coordinator, first);
        require(coordinator.waitUntilReady(2s),
                "DB recovery fixture must first become ready");
        require(coordinator.drainDeliveries(2s),
                "DB recovery fixture must first become idle");

        executeExternalSql(
            path,
            "ALTER TABLE FIRE_MQTT_OUTBOX "
            "RENAME TO FIRE_MQTT_OUTBOX_HIDDEN;");
        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::SubscriptionAcknowledged,
                     1, 92, 0, "ignored", {1}}),
                "DB exception wake fact must enqueue");
        {
            std::unique_lock lock(fault_mutex);
            require(fault_condition.wait_for(lock, 2s, [&] {
                        return faulted_epoch == 1;
                    }),
                    "DB list exception did not fault the active epoch");
        }
        require(!coordinator.isReady("ch01") &&
                    !coordinator.allChannelsReady(),
                "actor exception must clear every published readiness bit");

        executeExternalSql(
            path,
            "ALTER TABLE FIRE_MQTT_OUTBOX_HIDDEN "
            "RENAME TO FIRE_MQTT_OUTBOX;");
        connectEpoch(coordinator, publisher, 2);
        const auto replay = publisher.waitFor(1);
        require(replay.token.epoch == 2 &&
                    replay.delivery.fireRevision == 1,
                "recovered actor must resynchronize on a fresh epoch");
        acknowledge(coordinator, replay);
        require(coordinator.waitUntilReady(2s),
                "actor did not recover after the DB became available");
        require(coordinator.stop(), "DB recovery coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testRegularEgressPermitClosesBeforeDomainMutation() {
    const auto path = databasePath("regular-egress-permit");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize(), "egress permit topology must initialize");

        FakeTrackedPublisher publisher;
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        config.regularEgressDrainTimeout = 2s;
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        publisher.bind(&coordinator);
        require(coordinator.start(), "egress permit coordinator must start");
        connectEpoch(coordinator, publisher, 1);
        const auto retained = publisher.waitFor(0);
        acknowledge(coordinator, retained);
        require(coordinator.waitUntilReady(2s),
                "egress permit fixture must become ready");
        require(coordinator.drainDeliveries(2s),
                "egress permit fixture must become Fire-idle");
        const auto held_grant = coordinator.beginRegularEgress();
        require(held_grant.has_value() && held_grant->epoch == 1 &&
                    coordinator.isRegularEgressGrantCurrent(*held_grant),
                "ready coordinator must issue a regular egress permit");

        std::mutex start_mutex;
        std::condition_variable start_condition;
        bool mutation_entered{};
        std::atomic<bool> mutation_returned{false};
        bool mutation_admitted{};
        std::jthread mutation([&] {
            {
                std::lock_guard lock(start_mutex);
                mutation_entered = true;
            }
            start_condition.notify_all();
            mutation_admitted = coordinator.beginDomainMutation("ch01");
            mutation_returned.store(true, std::memory_order_release);
        });
        {
            std::unique_lock lock(start_mutex);
            require(start_condition.wait_for(lock, 2s, [&] {
                        return mutation_entered;
                    }),
                    "domain mutation thread did not enter");
        }
        requireEventually(
            [&] { return !coordinator.isReady("ch01"); }, 2s,
            "domain mutation did not close regular egress readiness");
        require(!mutation_returned.load(std::memory_order_acquire),
                "domain mutation passed an active regular publish permit");
        require(!coordinator.beginRegularEgress(),
                "new regular publish entered after mutation admission closed");

        coordinator.completeRegularEgress();
        mutation.join();
        require(mutation_admitted,
                "domain mutation did not resume after permit release");
        coordinator.completeDomainMutation("ch01");
        require(coordinator.stop(), "egress permit coordinator must stop");
    }
    std::filesystem::remove(path);
}

void testQueuedEpochBoundaryImmediatelyFailsClosed() {
    const auto path = databasePath("epoch-boundary-fail-close");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireAlarmService service(
            database, service_config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&](const event::FireCommandResult& result) {
                collector.add(result);
            });
        require(service.initialize() && service.start(),
                "epoch boundary topology must initialize");
        const auto open = service.submitSignal(fireSignal(true, 1));
        require(collector.wait(open.ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "epoch boundary OPEN must commit before delivery starts");

        FakeTrackedPublisher publisher;
        event::FireDeliveryCoordinator::Config config;
        config.channelIds = {"ch01"};
        event::FireDeliveryCoordinator coordinator(
            database, config,
            [&](const event::FireOutboxRecord& delivery,
                mqtt::MqttPublishCorrelation correlation,
                const mqtt::MqttConnectionEpoch expected_epoch) {
                return publisher.publish(
                    delivery, std::move(correlation), expected_epoch);
            });
        publisher.bind(&coordinator);
        require(coordinator.start(),
                "epoch boundary coordinator must start");
        connectEpoch(coordinator, publisher, 1);
        const auto retained = publisher.waitFor(0);

        publisher.blockNextPublish();
        acknowledge(coordinator, retained);
        publisher.waitUntilPublishBlocked();
        require(coordinator.isRevisionSynchronized(
                    "ch01", retained.delivery.fireRevision),
                "fixture retained revision did not become ready");

        require(coordinator.enqueueTransportFact(
                    {mqtt::MqttTransportFactKind::Connected,
                     2, -1, 0, {}, {}}),
                "fresh epoch fact must enqueue while actor is blocked");
        require(!coordinator.isRevisionSynchronized(
                    "ch01", retained.delivery.fireRevision) &&
                    !coordinator.beginRegularEgress(),
                "queued fresh epoch left stale Fire readiness published");

        publisher.setEpoch(2);
        publisher.releaseBlockedPublish();
        const auto stale_lifecycle = publisher.waitFor(1);
        require(!stale_lifecycle.accepted &&
                    stale_lifecycle.token.epoch == 2,
                "epoch-1 actor turn published lifecycle data on epoch 2");
        require(coordinator.stop(),
                "epoch boundary coordinator must stop");
        require(service.stop(), "epoch boundary service must stop");
    }
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    try {
        testRetainedBarrierRevisionReplacementAndEpochReuse();
        testImmediateAckAndLocalFailureRemainDurable();
        testMissingPubAckFaultsEpochAndRequeuesDurably();
        testLifecycleReleaseExcludesConcurrentDomainCommit();
        testRetainedLocalFailureDoesNotBlockAnotherChannel();
        testFactOverflowFaultsTheDroppedConnectionEpoch();
        testActorDatabaseExceptionFailsClosedAndRecovers();
        testRegularEgressPermitClosesBeforeDomainMutation();
        testQueuedEpochBoundaryImmediatelyFailsClosed();
    } catch (const std::exception& error) {
        std::cerr << "FireDeliveryIntegrationTest failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "FireDeliveryIntegrationTest passed\n";
    return 0;
}

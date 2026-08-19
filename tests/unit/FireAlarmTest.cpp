#include "database/EventDatabase.hpp"
#include "event/FireAlarmService.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <sqlite3.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path uniqueDatabasePath(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return std::filesystem::temp_directory_path() /
           ("pi-fire-" + name + '-' + std::to_string(stamp) + ".db");
}

void executeExternalSql(const std::filesystem::path& path, const char* sql) {
    sqlite3* connection{};
    const int open_result = sqlite3_open(path.string().c_str(), &connection);
    if (open_result != SQLITE_OK) {
        const std::string message = connection
            ? sqlite3_errmsg(connection)
            : "SQLite handle allocation failed";
        if (connection) sqlite3_close(connection);
        throw std::runtime_error("external SQLite open failed: " + message);
    }
    char* raw_error{};
    const int result =
        sqlite3_exec(connection, sql, nullptr, nullptr, &raw_error);
    const std::string message = raw_error ? raw_error : "unknown SQLite error";
    sqlite3_free(raw_error);
    sqlite3_close(connection);
    if (result != SQLITE_OK) {
        throw std::runtime_error("external SQLite statement failed: " + message);
    }
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
            throw std::runtime_error("timed out waiting for Fire DB commit");
        }
        return results_.at(ticket);
    }

    bool has(const std::uint64_t ticket,
             const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            return results_.contains(ticket);
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::map<std::uint64_t, event::FireCommandResult> results_;
};

event::FireAlarmService makeService(database::EventDatabase& database,
                                    ResultCollector& collector,
                                    std::vector<event::FireChannelBinding> bindings =
                                        {{"F1", "ch01",
                                          "parking/fire/ch01"}}) {
    event::FireAlarmService::Config config;
    config.cameraId = "cam01";
    config.lifecycleTopicPrefix = "parking/v1/events";
    config.commandQueueCapacity = 8;
    return event::FireAlarmService(
        database, std::move(config), std::move(bindings),
        [&collector](const event::FireCommandResult& result) {
            collector.add(result);
        });
}

event::FireSignal signal(const bool detected, const std::uint64_t sequence) {
    event::FireSignal value;
    value.sensorId = "F1";
    value.detected = detected;
    value.occurredAt = std::chrono::system_clock::now();
    value.sourceSequence = sequence;
    value.sourceTransport = "uart";
    value.rawPayload = std::string("FIRE:F1:") +
        (detected ? "DETECTED:" : "CLEARED:") +
        std::to_string(sequence);
    return value;
}

event::FireChannelBootstrap bootstrap(
    const std::string& sensor_id,
    const std::string& channel_id,
    const std::string& retained_topic,
    const std::uint64_t revision) {
    const std::string timestamp = "2026-08-11T00:00:00Z";
    event::FireChannelBootstrap value;
    value.initialState.channelId = channel_id;
    value.initialState.sensorId = sensor_id;
    value.initialState.retainedTopic = retained_topic;
    value.initialState.desiredLifecycle =
        event::FireAlarmLifecycle::Resolved;
    value.initialState.lastEventId =
        "fire-event:" + channel_id + ':' + std::to_string(revision);
    value.initialState.fireRevision = revision;
    value.initialState.protocolMode = event::FireProtocolMode::Unseen;
    value.initialState.lastSignalJson =
        "{\"sensor_id\":\"" + sensor_id + "\",\"detected\":false}";
    value.initialState.updatedAt = timestamp;

    value.retainedDelivery.deliveryKey = "fire-state:" + channel_id;
    value.retainedDelivery.sinkKind =
        event::FireDeliverySinkKind::RetainedState;
    value.retainedDelivery.logicalKey = retained_topic;
    value.retainedDelivery.sensorId = sensor_id;
    value.retainedDelivery.channelId = channel_id;
    value.retainedDelivery.eventId = value.initialState.lastEventId;
    value.retainedDelivery.alarmId =
        "fire-alarm:" + channel_id + ":bootstrap";
    value.retainedDelivery.fireRevision = revision;
    value.retainedDelivery.topic = retained_topic;
    value.retainedDelivery.payloadJson =
        "{\"fire_revision\":" + std::to_string(revision) + '}';
    value.retainedDelivery.qos = 1;
    value.retainedDelivery.retain = true;
    value.retainedDelivery.nextAttemptAt = timestamp;
    value.retainedDelivery.deliveryState =
        event::FireDeliveryState::Pending;
    value.retainedDelivery.createdAt = timestamp;
    value.retainedDelivery.updatedAt = timestamp;
    return value;
}

void testParserAndStrictMapping() {
    sensor::SensorProtocolParser parser;
    std::string error;
    const auto parsed = parser.parseFire(
        "FIRE:F1:DETECTED:42", std::chrono::system_clock::now(), &error);
    require(parsed.has_value() && parsed->sensorId == "F1" &&
                parsed->sequence == 42,
            "legacy Fire frame must still parse at the UART adapter");
    const auto versioned = parser.parseFire(
        "FIRE2:F1:DETECTED:boot-b:1",
        std::chrono::system_clock::now(), &error);
    require(versioned && versioned->bootId == "boot-b" &&
                versioned->sequence == 1 &&
                versioned->protocolVersion ==
                    sensor::SensorProtocolVersion::BootEpochV2,
            "versioned Fire frame must expose explicit protocol and boot ID");
    require(!parser.parseFire(
                "FIRE2:F1:DETECTED::1",
                std::chrono::system_clock::now(), &error),
            "versioned Fire frame without a boot ID was accepted");

    const auto valid = event::parseFireChannelBindingsStrict(
        "F1=ch01,F2=ch02", "parking/fire");
    require(valid.valid() && valid.bindings.size() == 2,
            "strict mapping must accept a one-to-one topology");
    require(valid.bindings[0].retainedTopic == "parking/fire/ch01",
            "retained topic must derive from the authoritative channel");
    require(!event::parseFireChannelBindingsStrict(
                 "", "parking/fire").valid(),
            "enabled Fire with no mapping must fail closed");
    require(!event::parseFireChannelBindingsStrict(
                 "F1=ch01,F1=ch02", "parking/fire").valid(),
            "duplicate sensor mapping must fail closed");
    require(!event::parseFireChannelBindingsStrict(
                 "F1=ch01,F2=ch01", "parking/fire").valid(),
            "duplicate channel mapping must fail closed");
    require(!event::parseFireChannelBindingsStrict(
                 "F1", "parking/fire").valid(),
            "malformed mapping must not be silently ignored");
}

void testTopologyBootstrapIsOneTransaction() {
    const auto success_path = uniqueDatabasePath("topology-success");
    {
        database::EventDatabase database(success_path);
        database.migrateRuntimeSchema();
        database.migrateRuntimeSchema();
        require(database.runtimeSchemaReady() &&
                    !database.occupancySchemaReady(),
                "Fire-only repeated migration must not advertise the "
                "parking occupancy schema as ready");
        ResultCollector collector;
        auto service = makeService(
            database, collector,
            {{"F1", "ch01", "parking/fire/ch01"},
             {"F2", "ch02", "parking/fire/ch02"}});
        require(service.initialize(),
                "all configured Fire channels must bootstrap together");
        require(database.listFireAlarmStates().size() == 2 &&
                    database.listFireRetainedDeliveries().size() == 2,
                "multi-channel bootstrap must commit every state/outbox pair");
    }
    std::filesystem::remove(success_path);

    const auto incomplete_path = uniqueDatabasePath("incomplete-parking");
    executeExternalSql(incomplete_path,
                       "CREATE TABLE PARKING_SLOT(slot_id TEXT PRIMARY KEY);");
    {
        database::EventDatabase database(incomplete_path);
        bool rejected = false;
        try {
            database.migrateRuntimeSchema();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && !database.runtimeSchemaReady() &&
                    !database.occupancySchemaReady(),
                "partial parking base schema must fail closed");
    }
    std::filesystem::remove(incomplete_path);

    const auto rollback_path = uniqueDatabasePath("topology-rollback");
    {
        database::EventDatabase database(rollback_path);
        database.migrateRuntimeSchema();
        ResultCollector existing_collector;
        {
            auto existing = makeService(
                database, existing_collector,
                {{"F2", "ch02", "parking/fire/ch02"}});
            require(existing.initialize(),
                    "existing durable channel fixture must bootstrap");
        }

        ResultCollector conflicting_collector;
        auto conflicting = makeService(
            database, conflicting_collector,
            {{"F1", "ch01", "parking/fire/ch01"},
             {"F2", "ch02", "parking/fire/renamed-ch02"}});
        require(!conflicting.initialize(),
                "one conflicting channel must reject the whole topology");
        const auto states = database.listFireAlarmStates();
        const auto retained = database.listFireRetainedDeliveries();
        require(!database.getFireAlarmState("ch01") && states.size() == 1 &&
                    states[0].channelId == "ch02" && retained.size() == 1 &&
                    retained[0].channelId == "ch02",
                "topology conflict must roll back earlier channel inserts");
    }
    std::filesystem::remove(rollback_path);
}

void testFireRevisionUsesSqliteIntegerCeiling() {
    const auto path = uniqueDatabasePath("revision-ceiling");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        const auto seeded = database.initializeFireTopology(
            {bootstrap("F1", "ch01", "parking/fire/ch01",
                       event::kMaxFireRevision)});
        require(seeded.committed(),
                "SQLite INT64_MAX must remain a valid Fire revision");

        ResultCollector collector;
        auto service = makeService(database, collector);
        require(service.initialize() && service.start(),
                "service must restore a channel at the revision ceiling");
        const auto detected = service.submitSignal(signal(true, 1));
        const auto result = collector.wait(detected.ticket);
        require(result.status == event::FireCommandStatus::Failed &&
                    result.fireRevision == event::kMaxFireRevision,
                "service must reject a transition before INT64 overflow");
        require(database.listFireLifecycleDeliveries().empty(),
                "revision exhaustion must not create delivery intents");
        require(service.stop(), "revision ceiling service must stop");
    }
    std::filesystem::remove(path);

    const auto invalid_path = uniqueDatabasePath("revision-overflow");
    {
        database::EventDatabase database(invalid_path);
        database.migrateRuntimeSchema();
        const auto rejected = database.initializeFireTopology(
            {bootstrap("F1", "ch01", "parking/fire/ch01",
                       event::kMaxFireRevision + 1)});
        require(rejected.outcome == event::FireStoreMutationOutcome::Invalid &&
                    database.listFireAlarmStates().empty() &&
                    database.listFireRetainedDeliveries().empty(),
                "revision above SQLite INT64_MAX must fail before persistence");
    }
    std::filesystem::remove(invalid_path);
}

void testFireListFailsClosedOnStepError() {
    const auto path = uniqueDatabasePath("list-step-error");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        executeExternalSql(path, R"sql(
            DROP TABLE FIRE_ALARM_STATE;
            CREATE VIEW FIRE_ALARM_STATE AS
            SELECT
                'ch01' AS channel_id,
                'F1' AS sensor_id,
                'parking/fire/ch01' AS retained_topic,
                'RESOLVED' AS desired_lifecycle,
                NULL AS active_alarm_id,
                'fire-event:ch01:1' AS last_event_id,
                1 AS fire_revision,
                'UNSEEN' AS protocol_mode,
                NULL AS active_boot_id,
                NULL AS last_source_sequence,
                '{}' AS last_signal_json,
                '2026-08-11T00:00:00Z' AS updated_at
            UNION ALL
            SELECT
                'ch02', 'F2', 'parking/fire/ch02', 'RESOLVED', NULL,
                'fire-event:ch02:1',
                abs(CAST('-9223372036854775808' AS INTEGER)),
                'UNSEEN', NULL, NULL, '{}', '2026-08-11T00:00:00Z';
        )sql");

        bool rejected = false;
        try {
            (void)database.listFireAlarmStates();
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find(
                           "SQLite Fire state list failed") !=
                       std::string::npos;
        }
        require(rejected,
                "non-SQLITE_DONE list termination must throw fail-closed");
    }
    std::filesystem::remove(path);
}

void testAtomicStateAndIndependentSinks() {
    const auto path = uniqueDatabasePath("atomic");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        auto service = makeService(database, collector);
        require(service.initialize(), "Fire bootstrap must commit");

        auto states = database.listFireAlarmStates();
        auto retained = database.listFireRetainedDeliveries();
        require(states.size() == 1 && states[0].fireRevision == 1 &&
                    states[0].desiredLifecycle ==
                        event::FireAlarmLifecycle::Resolved,
                "bootstrap must create one durable RESOLVED state");
        require(retained.size() == 1 && retained[0].fireRevision == 1 &&
                    retained[0].retain,
                "bootstrap must create one retained delivery intent");
        require(database.listFireLifecycleDeliveries().empty(),
                "bootstrap must not invent lifecycle history");

        require(service.start(), "Fire service worker must start");
        const auto opened = service.submitSignal(signal(true, 1));
        require(opened.status == event::FireCommandStatus::Queued,
                "accepted signal must report Queued, not committed");
        const auto open_result = collector.wait(opened.ticket);
        require(open_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    open_result.fireRevision == 2,
                "OPEN must report durable commit only after SQLite COMMIT");

        states = database.listFireAlarmStates();
        retained = database.listFireRetainedDeliveries();
        auto lifecycle = database.listFireLifecycleDeliveries();
        require(states[0].desiredLifecycle ==
                    event::FireAlarmLifecycle::Open &&
                    states[0].activeAlarmId == "fire-alarm:ch01:2",
                "OPEN alarm identity must derive from durable revision");
        require(retained.size() == 1 && retained[0].fireRevision == 2 &&
                    retained[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "OPEN must replace only the retained intent");
        require(lifecycle.size() == 1 && lifecycle[0].fireRevision == 2 &&
                    lifecycle[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "OPEN must append one immutable lifecycle intent");
        require(retained[0].payloadJson.find("\"fire_revision\":2") !=
                    std::string::npos &&
                    retained[0].payloadJson.find(
                        "\"delivery_id\":\"fire-state:ch01\"") !=
                        std::string::npos,
                "v2 payload must expose revision and sink delivery identity");

        require(database.markFireDeliveryInFlight(
                    retained[0].deliveryKey, 2),
                "retained delivery must be claimable");
        require(database.acknowledgeFireDelivery(
                    retained[0].deliveryKey, 2),
                "exact retained PUBACK must be persisted");
        lifecycle = database.listFireLifecycleDeliveries();
        require(lifecycle[0].deliveryState ==
                    event::FireDeliveryState::Pending,
                "retained PUBACK must not acknowledge the event sink");
        require(database.markFireDeliveryInFlight(
                    lifecycle[0].deliveryKey, 2) &&
                    database.acknowledgeFireDelivery(
                        lifecycle[0].deliveryKey, 2),
                "event sink must acknowledge independently");

        const auto duplicate = service.submitSignal(signal(true, 1));
        require(collector.wait(duplicate.ticket).status ==
                    event::FireCommandStatus::Idempotent,
                "same physical frame must be an idempotent delivery wake");
        require(database.listFireLifecycleDeliveries().size() == 1,
                "duplicate physical frame must not create a lifecycle row");

        const auto repeated = service.submitSignal(signal(true, 2));
        const auto repeated_result = collector.wait(repeated.ticket);
        require(repeated_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    repeated_result.fireRevision == 2,
                "newer same-state sequence must commit cursor only");
        require(database.listFireLifecycleDeliveries().size() == 1,
                "same-state cursor advance must not create history");

        const auto alarm_id =
            database.getFireAlarmState("ch01")->activeAlarmId;
        const auto ack = service.submitAcknowledge("ch01", alarm_id);
        require(collector.wait(ack.ticket).fireRevision == 3,
                "ACK must atomically create revision 3");
        retained = database.listFireRetainedDeliveries();
        require(retained[0].fireRevision == 3 &&
                    retained[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "ACK must reopen the retained sink independently");
        require(!database.acknowledgeFireDelivery(
                    retained[0].deliveryKey, 2),
                "revision 2 PUBACK must not clear retained revision 3");

        const auto clear = service.submitSignal(signal(false, 3));
        require(collector.wait(clear.ticket).fireRevision == 4,
                "CLEAR must atomically create revision 4");
        states = database.listFireAlarmStates();
        retained = database.listFireRetainedDeliveries();
        lifecycle = database.listFireLifecycleDeliveries();
        require(states[0].desiredLifecycle ==
                    event::FireAlarmLifecycle::Resolved &&
                    states[0].activeAlarmId.empty(),
                "CLEAR must resolve the durable state");
        require(retained.size() == 1 && retained[0].fireRevision == 4,
                "OPEN/ACK/CLEAR must still have one latest retained row");
        require(lifecycle.size() == 3 &&
                    lifecycle[0].eventId == "fire-event:ch01:2" &&
                    lifecycle[1].eventId == "fire-event:ch01:3" &&
                    lifecycle[2].eventId == "fire-event:ch01:4",
                "OPEN/ACK/CLEAR lifecycle rows must never overwrite each other");
        require(lifecycle[2].alarmId == alarm_id,
                "CLEAR must retain the OPEN alarm identity");
        require(service.stop(), "Fire service must drain and join");
    }
    std::filesystem::remove(path);
}

void testDatabaseFailureDoesNotConsumeSignal() {
    const auto path = uniqueDatabasePath("db-failure");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        auto service = makeService(database, collector);
        require(service.initialize() && service.start(),
                "Fire fixture must start");

        const auto legacy = service.submitSignal(signal(false, 900));
        require(collector.wait(legacy.ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "legacy Fire high cursor did not commit");

        database.close();
        auto first_v2 = signal(true, 1);
        first_v2.sourceProtocolVersion =
            sensor::SensorProtocolVersion::BootEpochV2;
        first_v2.sourceBootId = "boot-b";
        const auto first = service.submitSignal(std::move(first_v2));
        require(!service.drainCommitted(150ms),
                "accepted signal must remain queued while SQLite is down");

        require(database.open(path.string()), "fixture DB must reopen");
        database.migrateRuntimeSchema();
        const auto retry_result = collector.wait(first.ticket);
        require(retry_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    retry_result.fireRevision == 2,
                "the original accepted signal must commit after DB recovery");
        require(database.listFireLifecycleDeliveries().size() == 1,
                "internal retry must not duplicate Fire lifecycle history");
        const auto state = database.getFireAlarmState("ch01");
        require(state && state->protocolMode ==
                    event::FireProtocolMode::Versioned &&
                    state->activeBootId == "boot-b" &&
                    state->lastSourceSequence == 1,
                "failed transaction did not retry legacy-high to v2-low");
        require(service.stop(), "Fire service must stop");
    }
    std::filesystem::remove(path);
}

void testVersionedEpochRejectsDowngradeAndRetiredBoot() {
    const auto path = uniqueDatabasePath("versioned-epoch");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        auto service = makeService(database, collector);
        require(service.initialize() && service.start(),
                "versioned ingress fixture must start");
        require(collector.wait(
                    service.submitSignal(signal(false, 900)).ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "legacy Fire cursor did not commit");
        auto versioned = signal(true, 1);
        versioned.sourceProtocolVersion =
            sensor::SensorProtocolVersion::BootEpochV2;
        versioned.sourceBootId = "boot-b";
        require(collector.wait(
                    service.submitSignal(std::move(versioned)).ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "first v2 low sequence was rejected after legacy high");
        require(collector.wait(
                    service.submitSignal(signal(false, 901)).ticket).status ==
                    event::FireCommandStatus::Rejected,
                "legacy Fire downgrade was accepted");

        auto next_boot = signal(false, 1);
        next_boot.sourceProtocolVersion =
            sensor::SensorProtocolVersion::BootEpochV2;
        next_boot.sourceBootId = "boot-c";
        require(collector.wait(
                    service.submitSignal(std::move(next_boot)).ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "next Fire boot epoch did not commit");
        auto retired = signal(true, 2);
        retired.sourceProtocolVersion =
            sensor::SensorProtocolVersion::BootEpochV2;
        retired.sourceBootId = "boot-b";
        require(collector.wait(
                    service.submitSignal(std::move(retired)).ticket).status ==
                    event::FireCommandStatus::Rejected,
                "retired Fire boot ID was accepted");
        require(service.stop(), "versioned ingress fixture must stop");
    }
    std::filesystem::remove(path);
}

void testAcknowledgeWaitsForRetainedSynchronization() {
    const auto path = uniqueDatabasePath("ack-readiness");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        std::atomic<bool> ready{false};
        event::FireAlarmService::Config config;
        config.cameraId = "cam01";
        config.transientFailureRetryDelay = 25ms;
        event::FireAlarmService service(
            database, config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&collector](const event::FireCommandResult& result) {
                collector.add(result);
            },
            [&ready](const std::string&, const std::uint64_t) {
                return ready.load(std::memory_order_acquire);
            });
        require(service.initialize() && service.start(),
                "ACK readiness fixture must start");
        const auto open = service.submitSignal(signal(true, 1));
        require(collector.wait(open.ticket).fireRevision == 2,
                "ACK readiness fixture OPEN must commit");
        const auto alarm_id =
            database.getFireAlarmState("ch01")->activeAlarmId;
        const auto ack = service.submitAcknowledge("ch01", alarm_id);
        require(!collector.has(ack.ticket, 100ms) &&
                    !service.drainCommitted(25ms),
                "early MQTT ACK must remain admitted, not be rejected");
        ready.store(true, std::memory_order_release);
        const auto applied = collector.wait(ack.ticket);
        require(applied.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    applied.fireRevision == 3,
                "the same admitted ACK must commit when retained sync opens");
        require(service.stop(), "ACK readiness fixture must stop");
    }
    std::filesystem::remove(path);
}

void testEarlyAcknowledgeCannotBlockSensorClear() {
    const auto path = uniqueDatabasePath("ack-vs-clear");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config config;
        config.cameraId = "cam01";
        config.transientFailureRetryDelay = 25ms;
        event::FireAlarmService service(
            database, config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&collector](const event::FireCommandResult& result) {
                collector.add(result);
            },
            [](const std::string&, std::uint64_t) { return false; });
        require(service.initialize() && service.start(),
                "ACK/CLEAR fixture must start");
        const auto open = service.submitSignal(signal(true, 1));
        require(collector.wait(open.ticket).fireRevision == 2,
                "ACK/CLEAR OPEN must commit");
        const auto alarm_id =
            database.getFireAlarmState("ch01")->activeAlarmId;
        const auto early_ack = service.submitAcknowledge("ch01", alarm_id);
        require(!collector.has(early_ack.ticket, 75ms),
                "early ACK must wait for readiness");

        const auto clear = service.submitSignal(signal(false, 2));
        const auto clear_result = collector.wait(clear.ticket);
        require(clear_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    clear_result.fireRevision == 3,
                "physical CLEAR must pass a readiness-waiting ACK");
        const auto obsolete_ack = collector.wait(early_ack.ticket);
        require(obsolete_ack.status == event::FireCommandStatus::Rejected,
                "ACK must become obsolete after authoritative CLEAR");
        const auto state = database.getFireAlarmState("ch01");
        require(state && state->desiredLifecycle ==
                             event::FireAlarmLifecycle::Resolved,
                "waiting ACK must not prevent durable CLEAR");
        require(service.stop(), "ACK/CLEAR fixture must stop");
    }
    std::filesystem::remove(path);
}

void testRepeatedEarlyAcknowledgeIsCoalescedBeforeSensorClear() {
    const auto path = uniqueDatabasePath("ack-storm-vs-clear");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        event::FireAlarmService::Config config;
        config.cameraId = "cam01";
        config.commandQueueCapacity = 4;
        config.transientFailureRetryDelay = 25ms;
        event::FireAlarmService service(
            database, config,
            {{"F1", "ch01", "parking/fire/ch01"}},
            [&collector](const event::FireCommandResult& result) {
                collector.add(result);
            },
            [](const std::string&, std::uint64_t) { return false; });
        require(service.initialize() && service.start(),
                "ACK storm fixture must start");
        const auto open = service.submitSignal(signal(true, 1));
        require(collector.wait(open.ticket).fireRevision == 2,
                "ACK storm OPEN must commit");
        const auto alarm_id =
            database.getFireAlarmState("ch01")->activeAlarmId;

        std::uint64_t coalesced_ticket{};
        for (int index = 0; index < 64; ++index) {
            const auto ack = service.submitAcknowledge("ch01", alarm_id);
            require(ack.status == event::FireCommandStatus::Queued,
                    "duplicate early ACK must remain admitted");
            if (index == 0) coalesced_ticket = ack.ticket;
            require(ack.ticket == coalesced_ticket,
                    "duplicate early ACKs must share one pending command");
        }
        require(!collector.has(coalesced_ticket, 75ms),
                "coalesced ACK must still wait for retained readiness");

        const auto clear = service.submitSignal(signal(false, 2));
        require(clear.status == event::FireCommandStatus::Queued,
                "ACK storm must preserve authoritative signal capacity");
        const auto clear_result = collector.wait(clear.ticket);
        require(clear_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    clear_result.fireRevision == 3,
                "sensor CLEAR must commit behind a coalesced ACK storm");
        require(collector.wait(coalesced_ticket).status ==
                    event::FireCommandStatus::Rejected,
                "coalesced ACK must become obsolete after CLEAR");
        require(service.stop(), "ACK storm fixture must stop");
    }
    std::filesystem::remove(path);
}

void testTransientFailureBlocksOnlyItsChannel() {
    const auto path = uniqueDatabasePath("per-channel-retry");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        std::atomic<bool> allow_ch01{false};
        event::FireAlarmService::Config config;
        config.cameraId = "cam01";
        config.transientFailureRetryDelay = 25ms;
        event::FireAlarmService service(
            database, config,
            {{"F1", "ch01", "parking/fire/ch01"},
             {"F2", "ch02", "parking/fire/ch02"}},
            [&collector](const event::FireCommandResult& result) {
                collector.add(result);
            }, {},
            [&allow_ch01](const std::string& channel) {
                return channel != "ch01" ||
                    allow_ch01.load(std::memory_order_acquire);
            },
            [](const std::string&) {});
        require(service.initialize() && service.start(),
                "per-channel retry fixture must start");
        const auto blocked = service.submitSignal(signal(true, 1));
        require(!collector.has(blocked.ticket, 75ms),
                "ch01 mutation boundary must keep its command admitted");

        auto other_signal = signal(true, 1);
        other_signal.sensorId = "F2";
        other_signal.rawPayload = "FIRE:F2:DETECTED:1";
        const auto other = service.submitSignal(std::move(other_signal));
        const auto other_result = collector.wait(other.ticket);
        require(other_result.status ==
                    event::FireCommandStatus::DurablyCommitted &&
                    other_result.channelId == "ch02",
                "ch01 retry must not stall an independent Fire channel");

        allow_ch01.store(true, std::memory_order_release);
        require(collector.wait(blocked.ticket).status ==
                    event::FireCommandStatus::DurablyCommitted,
                "blocked channel must resume the original command");
        require(service.stop(), "per-channel retry fixture must stop");
    }
    std::filesystem::remove(path);
}

void testRestartRestoresPendingClear() {
    const auto path = uniqueDatabasePath("restart");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        {
            ResultCollector collector;
            auto service = makeService(database, collector);
            require(service.initialize() && service.start(),
                    "first Fire process fixture must start");
            const auto open = service.submitSignal(signal(true, 10));
            require(collector.wait(open.ticket).fireRevision == 2,
                    "OPEN must commit before simulated restart");
            const auto clear = service.submitSignal(signal(false, 11));
            require(collector.wait(clear.ticket).fireRevision == 3,
                    "CLEAR must commit before simulated restart");
            require(service.stop(), "first service must stop");
        }

        const auto before_restart = database.listFireRetainedDeliveries();
        require(before_restart.size() == 1 &&
                    before_restart[0].fireRevision == 3 &&
                    before_restart[0].deliveryState ==
                        event::FireDeliveryState::Pending,
                "pending CLEAR must be durable before restart");

        ResultCollector restored_collector;
        auto restored = makeService(database, restored_collector);
        require(restored.initialize(),
                "recreated service must validate existing Fire state");
        const auto state = database.getFireAlarmState("ch01");
        require(state && state->fireRevision == 3 &&
                    state->desiredLifecycle ==
                        event::FireAlarmLifecycle::Resolved,
                "recreated service must restore desired CLEAR without input");
        require(database.listFireLifecycleDeliveries().size() == 2,
                "restart must preserve stable OPEN and CLEAR event IDs");
    }
    std::filesystem::remove(path);
}

void testManualClearPreservesPhysicalSensorCursor() {
    const auto path = uniqueDatabasePath("manual-clear");
    {
        database::EventDatabase database(path);
        database.migrateRuntimeSchema();
        ResultCollector collector;
        auto service = makeService(database, collector);
        require(service.initialize() && service.start(),
                "manual-clear fixture must start");

        const auto open = service.submitSignal(signal(true, 10));
        require(collector.wait(open.ticket).fireRevision == 2,
                "manual-clear fixture OPEN must commit");
        const auto clear = service.submitManualClear("ch01");
        const auto cleared = collector.wait(clear.ticket);
        require(cleared.status == event::FireCommandStatus::DurablyCommitted &&
                    cleared.fireRevision == 3,
                "manual clear must commit a new revision");
        const auto state = database.getFireAlarmState("ch01");
        require(state &&
                    state->desiredLifecycle ==
                        event::FireAlarmLifecycle::Resolved &&
                    state->lastSourceSequence == 10,
                "manual clear must preserve the physical sensor cursor");

        const auto duplicate = service.submitManualClear("ch01");
        require(collector.wait(duplicate.ticket).status ==
                    event::FireCommandStatus::Idempotent,
                "repeated manual clear must be idempotent");
        const auto reopen = service.submitSignal(signal(true, 11));
        require(collector.wait(reopen.ticket).fireRevision == 4,
                "newer physical DETECTED must reopen after manual clear");
        require(service.stop(), "manual-clear fixture must stop");
    }
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    try {
        testParserAndStrictMapping();
        testTopologyBootstrapIsOneTransaction();
        testFireRevisionUsesSqliteIntegerCeiling();
        testFireListFailsClosedOnStepError();
        testAtomicStateAndIndependentSinks();
        testDatabaseFailureDoesNotConsumeSignal();
        testVersionedEpochRejectsDowngradeAndRetiredBoot();
        testAcknowledgeWaitsForRetainedSynchronization();
        testEarlyAcknowledgeCannotBlockSensorClear();
        testRepeatedEarlyAcknowledgeIsCoalescedBeforeSensorClear();
        testTransientFailureBlocksOnlyItsChannel();
        testRestartRestoresPendingClear();
        testManualClearPreservesPhysicalSensorCursor();
    } catch (const std::exception& error) {
        std::cerr << "FireAlarmTest failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "FireAlarmTest passed\n";
    return 0;
}

#include "database/EventDatabase.hpp"
#include "database/SessionTransitionStore.hpp"
#include "parking/SlotTransitionActor.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate,
               const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        try {
            if (predicate()) return true;
        } catch (const std::exception&) {
            // Async transport admission may not have created the row yet.
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    try {
        return predicate();
    } catch (const std::exception&) {
        return false;
    }
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        static std::atomic<unsigned long long> ordinal{};
        const auto unique = std::to_string(nowEpochMs()) + "-" +
                            std::to_string(++ordinal);
        path = std::filesystem::temp_directory_path() /
               ("slot-transition-actor-" + unique + ".db");
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

class SqliteProbe {
public:
    explicit SqliteProbe(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.string().c_str(), &database_,
                            SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            const std::string message = database_ == nullptr
                ? "SQLite probe open failed"
                : sqlite3_errmsg(database_);
            if (database_ != nullptr) sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(database_, 3000);
    }

    ~SqliteProbe() {
        if (database_ != nullptr) sqlite3_close(database_);
    }

    SqliteProbe(const SqliteProbe&) = delete;
    SqliteProbe& operator=(const SqliteProbe&) = delete;

    void execute(const std::string& sql) {
        char* error{};
        const int result = sqlite3_exec(database_, sql.c_str(), nullptr,
                                        nullptr, &error);
        if (result == SQLITE_OK) return;
        const std::string message = error == nullptr
            ? sqlite3_errmsg(database_)
            : error;
        sqlite3_free(error);
        throw std::runtime_error("SQLite probe exec failed: " + message);
    }

    std::optional<std::int64_t> optionalInt(const std::string& sql) const {
        sqlite3_stmt* statement{};
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement,
                              nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite probe prepare failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
        const int step = sqlite3_step(statement);
        std::optional<std::int64_t> value;
        if (step == SQLITE_ROW &&
            sqlite3_column_type(statement, 0) != SQLITE_NULL) {
            value = sqlite3_column_int64(statement, 0);
        } else if (step != SQLITE_DONE && step != SQLITE_ROW) {
            const std::string message = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite probe read failed: " + message);
        }
        sqlite3_finalize(statement);
        return value;
    }

    std::int64_t integer(const std::string& sql) const {
        const auto value = optionalInt(sql);
        if (!value) throw std::runtime_error("SQLite probe returned no row");
        return *value;
    }

    std::string text(const std::string& sql) const {
        sqlite3_stmt* statement{};
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement,
                              nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite probe prepare failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
        const int step = sqlite3_step(statement);
        if (step != SQLITE_ROW) {
            const std::string message = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite probe returned no text: " +
                                     message);
        }
        const auto* raw = sqlite3_column_text(statement, 0);
        const std::string value = raw == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(raw);
        sqlite3_finalize(statement);
        return value;
    }

private:
    sqlite3* database_{};
};

void initialize(database::EventDatabase& database) {
    const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");
}

parking::SlotTransitionActor::Config actorConfig() {
    parking::SlotTransitionActor::Config config;
    config.transportAdmissionCapacity = 16;
    config.durablePendingCapacity = 100;
    config.retryDelay = 10ms;
    return config;
}

parking::SlotTransitionCommand hallCommand(
    const std::string& command_id,
    const std::uint64_t sequence,
    const std::string& state,
    const std::int64_t occurred_epoch_ms,
    const std::int64_t due_epoch_ms = 0,
    const std::string& protocol = "LEGACY_V1",
    const std::string& boot_id = {}) {
    parking::SlotTransitionCommand command;
    command.commandId = command_id;
    command.kind = parking::SlotCommandKind::HallObservation;
    command.slotId = "P01";
    command.sensorId = "hall-p01";
    command.sourceIdentity = "hall-p01:" + protocol + ':' + boot_id + ':' +
                             std::to_string(sequence);
    command.sourceSequence = sequence;
    command.occurredAt = "2026-08-11T00:00:" +
                         std::to_string(sequence) + "Z";
    command.payloadJson = nlohmann::json{
        {"state", state},
        {"source_protocol", protocol},
        {"transport", "UART"},
        {"occurred_at_epoch_ms", occurred_epoch_ms}}.dump();
    if (!boot_id.empty()) {
        auto payload = nlohmann::json::parse(command.payloadJson);
        payload["source_boot_id"] = boot_id;
        command.payloadJson = payload.dump();
    }
    command.dueAtEpochMs = due_epoch_ms;
    return command;
}

parking::SlotTransitionCommand cameraCommand(
    const std::string& command_id,
    const std::string& action,
    const std::string& object_id,
    const std::int64_t occurred_epoch_ms,
    const std::int64_t due_epoch_ms) {
    parking::SlotTransitionCommand command;
    command.commandId = command_id;
    command.kind = parking::SlotCommandKind::CameraObservation;
    command.slotId = "EV01";
    command.sourceIdentity = "camera:ch01:IVA1:" + object_id + ":" +
                             command_id;
    command.occurredAt = "2026-08-11T01:00:00Z";
    command.payloadJson = nlohmann::json{
        {"action", action},
        {"authoritative_exit", true},
        {"area_key", "IVA1"},
        {"configured_areas", nlohmann::json::array({"IVA1"})},
        {"object_id", object_id},
        {"transport", "MQTT"},
        {"occurred_at_epoch_ms", occurred_epoch_ms},
        {"timestamp_authority", "CAMERA_UTC"},
        {"deadline_due_at_epoch_ms", due_epoch_ms}}.dump();
    command.dueAtEpochMs = nowEpochMs() - 1;
    return command;
}

parking::SlotTransitionCommand hybridHallCommand(
    const std::string& command_id,
    const std::uint64_t sequence,
    const std::string& state,
    const std::int64_t occurred_epoch_ms,
    const std::int64_t due_epoch_ms = 0) {
    auto command = hallCommand(command_id, sequence, state,
                               occurred_epoch_ms, due_epoch_ms);
    command.slotId = "EV01";
    auto payload = nlohmann::json::parse(command.payloadJson);
    payload["occupancy_policy"] = "HYBRID_OR";
    command.payloadJson = payload.dump();
    return command;
}

parking::SlotTransitionCommand hybridCameraCommand(
    const std::string& command_id,
    const std::string& action,
    const std::string& object_id,
    const std::int64_t occurred_epoch_ms,
    const std::int64_t deadline_epoch_ms) {
    auto command = cameraCommand(command_id, action, object_id,
                                 occurred_epoch_ms, deadline_epoch_ms);
    auto payload = nlohmann::json::parse(command.payloadJson);
    payload["occupancy_policy"] = "HYBRID_OR";
    command.payloadJson = payload.dump();
    return command;
}

parking::SlotTransitionActor makeActor(
    database::SessionTransitionStore& store) {
    return parking::SlotTransitionActor(
        store, actorConfig(),
        [](const parking::CommittedOccupancyTransition&) { return true; });
}

void requireSubmitted(const parking::SlotSubmitResult& result,
                      const std::string& context) {
    require(result.accepted(), context + ": " + result.message);
}

void testAsyncAdmissionFailurePreservesSlotHead() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    probe.execute(
        "CREATE TRIGGER fail_inbox_admission BEFORE INSERT ON "
        "OCCUPANCY_COMMAND_INBOX "
        "BEGIN SELECT RAISE(ABORT,'injected durable admission failure'); END;");

    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "async-admission actor did not start");
    const auto occupied = actor.submit(hallCommand(
        "hall-admission-occ", 91, "OCCUPIED", 9000, 0));
    const auto vacant = actor.submit(hallCommand(
        "hall-admission-vac", 92, "VACANT", 9100, 0));
    require(occupied.code == parking::SlotSubmitCode::TransportQueued &&
                vacant.code == parking::SlotSubmitCode::TransportQueued,
            "submit performed SQLite admission on the transport callback");

    std::this_thread::sleep_for(50ms);
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX;") == 0,
            "SQLite admission trigger unexpectedly allowed a command");
    probe.execute("DROP TRIGGER fail_inbox_admission;");
    require(actor.waitUntilIdle(3s),
            "queued slot lane did not retry durable admission");
    require(probe.integer(
                "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-admission-occ';") <
                probe.integer(
                    "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX "
                    "WHERE command_id='hall-admission-vac';"),
            "same-slot VACANT overtook the failed OCCUPIED admission head");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE "
                "entry_command_id='hall-admission-occ';") == "ENDED",
            "retried async OCCUPIED/VACANT lane did not close its session");
    require(probe.text(
                "SELECT status FROM PARKING_SLOT WHERE slot_id='P01';") ==
                "VACANT",
            "retried async OCCUPIED/VACANT lane has the wrong final state");
    require(probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                92,
            "async admission retry committed sequences out of order");
    require(actor.stopAndDrain(2s), "async-admission actor did not stop");
}

void testTransportQueueFullLeavesCameraStateUntouched() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    probe.execute(
        "CREATE TRIGGER block_transport_admission BEFORE INSERT ON "
        "OCCUPANCY_COMMAND_INBOX BEGIN SELECT RAISE(ABORT,"
        "'injected transport admission failure'); END;");

    database::SessionTransitionStore store(database);
    auto config = actorConfig();
    config.transportAdmissionCapacity = 1;
    parking::SlotTransitionActor actor(
        store, config,
        [](const parking::CommittedOccupancyTransition&) { return true; });
    require(actor.start(), "transport-capacity actor did not start");
    requireSubmitted(actor.submit(hallCommand(
        "transport-head", 93, "OCCUPIED", nowEpochMs() - 10)),
        "transport-capacity head was not queued");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX;") == 0;
    }, 100ms), "transport head unexpectedly crossed failed admission");

    const auto camera = cameraCommand(
        "camera-queue-full", "INTRUSION", "queue-object",
        nowEpochMs() - 5, 0);
    const auto rejected = actor.submit(camera);
    require(rejected.code == parking::SlotSubmitCode::QueueFull,
            "full transport queue accepted a camera observation");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='camera-queue-full';") == 0,
            "queue-full camera observation mutated the durable inbox");
    require(probe.integer(
                "SELECT COUNT(*) FROM IVA_SLOT_OBSERVATION_STATE WHERE "
                "slot_id='EV01';") == 0,
            "queue-full camera observation mutated the IVA reducer");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01';") ==
                0,
            "queue-full camera observation created a session");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01';") == 0,
            "queue-full camera observation changed a deadline");

    probe.execute("DROP TRIGGER block_transport_admission;");
    require(actor.waitUntilIdle(3s),
            "transport head did not recover after admission unblocked");
    requireSubmitted(actor.submit(camera),
                     "camera retry was blocked after queue recovery");
    require(actor.waitUntilIdle(3s),
            "retried camera observation did not settle");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                "AND status='ACTIVE';") == 1,
            "camera retry did not create its authoritative session");
    require(actor.stopAndDrain(2s),
            "transport-capacity actor did not stop");
}

void testSourceRedeliveryIgnoresOnlyLocalSchedulingTime() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    database::SessionTransitionStore store(database);

    auto hall = hallCommand("hall-redelivery", 94, "OCCUPIED", 1000, 2000);
    require(store.admit(hall, 100).code ==
                parking::SlotAdmissionCode::Admitted,
            "initial Hall source fact was not admitted");
    auto hall_redelivery = hall;
    hall_redelivery.occurredAt = "2026-08-11T03:00:10Z";
    hall_redelivery.dueAtEpochMs = 9000;
    auto hall_payload = nlohmann::json::parse(hall_redelivery.payloadJson);
    hall_payload["occurred_at_epoch_ms"] = 8000;
    hall_redelivery.payloadJson = hall_payload.dump();
    require(store.admit(hall_redelivery, 100).code ==
                parking::SlotAdmissionCode::Existing,
            "same sequenced Hall fact conflicted on receive time");
    auto hall_conflict = hall_redelivery;
    auto conflict_payload = nlohmann::json::parse(hall_conflict.payloadJson);
    conflict_payload["state"] = "VACANT";
    hall_conflict.payloadJson = conflict_payload.dump();
    require(store.admit(hall_conflict, 100).code ==
                parking::SlotAdmissionCode::Conflict,
            "same Hall identity accepted a different physical state");

    auto camera = cameraCommand(
        "camera-redelivery", "EXIT", "object-redelivery", 3000, 5000);
    require(store.admit(camera, 100).code ==
                parking::SlotAdmissionCode::Admitted,
            "initial camera source fact was not admitted");
    auto camera_redelivery = camera;
    camera_redelivery.dueAtEpochMs += 1000;
    auto camera_payload = nlohmann::json::parse(
        camera_redelivery.payloadJson);
    camera_payload["deadline_due_at_epoch_ms"] = 9000;
    camera_redelivery.payloadJson = camera_payload.dump();
    require(store.admit(camera_redelivery, 100).code ==
                parking::SlotAdmissionCode::Existing,
            "same camera fact conflicted on receive-time deadline");
    auto camera_conflict = camera_redelivery;
    auto camera_conflict_payload = nlohmann::json::parse(
        camera_conflict.payloadJson);
    camera_conflict_payload["object_id"] = "different-object";
    camera_conflict.payloadJson = camera_conflict_payload.dump();
    require(store.admit(camera_conflict, 100).code ==
                parking::SlotAdmissionCode::Conflict,
            "same camera identity accepted a changed object");
}

void testConcurrentDurableCapacityHasOneWinner() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    auto first = hallCommand("capacity-first", 95, "OCCUPIED", 1000, 5000);
    auto second = hallCommand("capacity-second", 96, "OCCUPIED", 1001, 5000);
    second.slotId = "P02";
    second.sensorId = "hall-p02";
    second.sourceIdentity = "hall-p02:96";
    std::barrier start_line(3);
    parking::SlotAdmissionResult first_result;
    parking::SlotAdmissionResult second_result;
    std::thread first_thread([&] {
        start_line.arrive_and_wait();
        first_result = store.admit(first, 1);
    });
    std::thread second_thread([&] {
        start_line.arrive_and_wait();
        second_result = store.admit(second, 1);
    });
    start_line.arrive_and_wait();
    first_thread.join();
    second_thread.join();
    const int admitted =
        (first_result.code == parking::SlotAdmissionCode::Admitted ? 1 : 0) +
        (second_result.code == parking::SlotAdmissionCode::Admitted ? 1 : 0);
    const int full =
        (first_result.code == parking::SlotAdmissionCode::CapacityFull ? 1 : 0) +
        (second_result.code == parking::SlotAdmissionCode::CapacityFull ? 1 : 0);
    require(admitted == 1 && full == 1,
            "concurrent capacity check did not produce one exact winner");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX;") == 1,
            "capacity loser mutated the durable inbox");
    require(probe.integer("SELECT COUNT(*) FROM PARKING_SESSION;") == 0,
            "durable admission race mutated authoritative sessions");
}

void testFullDurableInboxPreservesAndPacesDueDeadline() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto now = nowEpochMs();

    const auto intrusion = cameraCommand(
        "deadline-capacity-entry", "INTRUSION", "deadline-object",
        now - 1000, 0);
    require(store.admit(intrusion, 100).accepted(),
            "deadline-capacity intrusion admission failed");
    require(store.apply(intrusion.commandId).code ==
                parking::CommittedOccupancyCode::SessionStarted,
            "deadline-capacity intrusion did not start a session");
    require(store.completeEffects(intrusion.commandId),
            "deadline-capacity start effect did not complete");

    const auto exit = cameraCommand(
        "deadline-capacity-exit", "EXIT", "deadline-object",
        now - 500, now - 1);
    require(store.admit(exit, 100).accepted(),
            "deadline-capacity exit admission failed");
    require(store.apply(exit.commandId).code ==
                parking::CommittedOccupancyCode::ExitScheduled,
            "deadline-capacity exit did not schedule a deadline");

    const auto filler = hallCommand(
        "deadline-capacity-filler", 97, "OCCUPIED", now,
        now + 60000);
    require(store.admit(filler, 100).accepted(),
            "deadline-capacity filler admission failed");
    require(store.admitDueDeadlines(now, 1) == 0,
            "full durable inbox admitted a due deadline");
    require(probe.text(
                "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01';") == "SCHEDULED",
            "capacity-full deadline was consumed instead of preserved");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "source_kind='EXIT_DEADLINE';") == 0,
            "capacity-full deadline leaked a command row");

    std::atomic<int> deadline_attempts{};
    auto config = actorConfig();
    config.durablePendingCapacity = 1;
    config.retryDelay = 20ms;
    config.checkpoint = [&](parking::SlotActorCheckpoint checkpoint) {
        if (checkpoint ==
            parking::SlotActorCheckpoint::BeforeDeadlineLinearization) {
            deadline_attempts.fetch_add(1, std::memory_order_relaxed);
        }
    };
    parking::SlotTransitionActor actor(
        store, config,
        [](const parking::CommittedOccupancyTransition&) { return true; });
    require(actor.start(), "deadline-capacity actor did not start");
    std::this_thread::sleep_for(100ms);
    require(deadline_attempts.load(std::memory_order_relaxed) <= 10,
            "past-due capacity failure caused a tight SQLite retry loop");
    require(probe.text(
                "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01';") == "SCHEDULED",
            "actor consumed a deadline while durable capacity was full");

    probe.execute(
        "UPDATE OCCUPANCY_COMMAND_INBOX SET status='APPLIED',"
        "result_code='NO_CHANGE' WHERE "
        "command_id='deadline-capacity-filler';");
    require(actor.waitUntilIdle(3s),
            "preserved deadline did not run after capacity recovered");
    require(probe.text(
                "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01';") == "APPLIED",
            "recovered deadline was not applied");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE "
                "entry_command_id='deadline-capacity-entry';") == "ENDED",
            "recovered deadline did not close its exact session");
    require(actor.stopAndDrain(2s),
            "deadline-capacity actor did not stop");
}

void testHallCreateFailureRetriesWithoutConsumingSequence() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "create-retry actor did not start");
    requireSubmitted(actor.submit(hallCommand(
        "hall-legacy-high", 900, "VACANT", 9000)),
        "legacy high sequence admission failed");
    require(actor.waitUntilIdle(3s), "legacy high sequence did not commit");
    require(probe.text(
                "SELECT protocol_mode FROM OCCUPANCY_SENSOR_SEQUENCE_STATE "
                "WHERE sensor_id='hall-p01';") == "LEGACY" &&
            probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                900,
            "legacy high cursor was not committed");

    probe.execute(
        "CREATE TRIGGER fail_hall_create BEFORE INSERT ON PARKING_SESSION "
        "BEGIN SELECT RAISE(ABORT,'injected hall create failure'); END;");
    const auto command = hallCommand(
        "hall-create-retry", 1, "OCCUPIED", 10000, 0,
        "BOOT_EPOCH_V2", "boot-b");
    requireSubmitted(actor.submit(command), "Hall OCCUPIED admission failed");

    require(waitUntil([&] {
        return probe.integer(
            "SELECT attempt_count FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='hall-create-retry';") >= 1;
    }), "Hall create failure was not deferred for automatic retry");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01';") ==
                0,
            "failed Hall create leaked a parking session");
    require(probe.text(
                "SELECT protocol_mode FROM OCCUPANCY_SENSOR_SEQUENCE_STATE "
                "WHERE sensor_id='hall-p01';") == "LEGACY" &&
            probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                900,
            "failed first v2 frame consumed or retired the legacy cursor");

    probe.execute("DROP TRIGGER fail_hall_create;");
    require(actor.waitUntilIdle(3s),
            "Hall create command did not retry after trigger removal");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") == 1,
            "retried Hall create did not commit exactly one active session");
    require(probe.text(
                "SELECT protocol_mode FROM OCCUPANCY_SENSOR_SEQUENCE_STATE "
                "WHERE sensor_id='hall-p01';") == "VERSIONED" &&
            probe.text(
                "SELECT active_boot_id FROM OCCUPANCY_SENSOR_SEQUENCE_STATE "
                "WHERE sensor_id='hall-p01';") == "boot-b" &&
            probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                1,
            "legacy-high to first-v2-low did not commit atomically");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-create-retry';") == "APPLIED",
            "retried Hall create did not reach terminal APPLIED state");

    require(store.admit(hallCommand(
                "hall-v1-downgrade", 901, "OCCUPIED", 10001), 100).code ==
                parking::SlotAdmissionCode::RejectedStale,
            "legacy downgrade was accepted after versioned commit");
    const auto next_boot = hallCommand(
        "hall-next-boot", 1, "VACANT", 10002, 0,
        "BOOT_EPOCH_V2", "boot-c");
    requireSubmitted(actor.submit(next_boot), "next Hall boot was rejected");
    require(actor.waitUntilIdle(3s), "next Hall boot did not commit");
    require(store.admit(hallCommand(
                "hall-retired-boot", 2, "OCCUPIED", 10003, 0,
                "BOOT_EPOCH_V2", "boot-b"), 100).code ==
                parking::SlotAdmissionCode::RejectedStale,
            "retired Hall boot ID was accepted");
    require(actor.stopAndDrain(2s), "create-retry actor did not stop");
}

void testShutdownDoesNotClaimDueRetryWasDrained() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    probe.execute(
        "CREATE TRIGGER fail_shutdown_create BEFORE INSERT ON PARKING_SESSION "
        "BEGIN SELECT RAISE(ABORT,'injected shutdown create failure'); END;");

    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "shutdown-drain actor did not start");
    requireSubmitted(actor.submit(hallCommand(
        "hall-shutdown-retry", 151, "OCCUPIED", nowEpochMs() - 10)),
        "shutdown retry admission failed");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT attempt_count FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='hall-shutdown-retry';") >= 1;
    }), "shutdown retry failure was not durably deferred");

    require(!actor.stopAndDrain(150ms),
            "shutdown falsely reported a due failed command as drained");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-shutdown-retry';") ==
                "PENDING_UNPREPARED",
            "failed shutdown retry unexpectedly became terminal");

    probe.execute("DROP TRIGGER fail_shutdown_create;");
    require(actor.stopAndDrain(3s),
            "shutdown retry did not finish after SQLite recovered");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-shutdown-retry';") == "APPLIED",
            "recovered shutdown retry did not commit before stop");
}

void testHallCloseFailureRetriesExactSession() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "close-retry actor did not start");

    requireSubmitted(actor.submit(hallCommand(
        "hall-close-entry", 201, "OCCUPIED", 20000)),
        "Hall entry admission failed");
    require(actor.waitUntilIdle(3s), "Hall entry did not commit");
    const auto original_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='P01' "
        "AND status='ACTIVE';");

    probe.execute(
        "CREATE TRIGGER fail_hall_close BEFORE UPDATE OF status ON "
        "PARKING_SESSION WHEN NEW.status='ENDED' "
        "BEGIN SELECT RAISE(ABORT,'injected hall close failure'); END;");
    requireSubmitted(actor.submit(hallCommand(
        "hall-close-retry", 202, "VACANT", 21000)),
        "Hall VACANT admission failed");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT attempt_count FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='hall-close-retry';") >= 1;
    }), "Hall close failure was not deferred for automatic retry");
    require(probe.integer(
                "SELECT session_id FROM PARKING_SESSION WHERE slot_id='P01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") ==
                original_session,
            "failed Hall close changed the authoritative active session");
    require(probe.text(
                "SELECT status FROM PARKING_SLOT WHERE slot_id='P01';") ==
                "OCCUPIED",
            "failed Hall close split slot state from its active session");
    require(probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                201,
            "failed Hall close consumed its sequence");

    probe.execute("DROP TRIGGER fail_hall_close;");
    require(actor.waitUntilIdle(3s),
            "Hall close command did not retry after trigger removal");
    require(probe.integer(
                "SELECT session_id FROM PARKING_SESSION WHERE slot_id='P01' "
                "AND status='ENDED';") == original_session,
            "Hall close retry did not end the exact original session");
    require(probe.text(
                "SELECT exit_command_id FROM PARKING_SESSION WHERE "
                "session_id=" + std::to_string(original_session) + ";") ==
                "hall-close-retry",
            "Hall close retry lost its exact command identity");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01';") ==
                1,
            "Hall close retry created or closed an unrelated session");
    require(probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                202,
            "successful Hall close retry did not commit its sequence");
    require(actor.stopAndDrain(2s), "close-retry actor did not stop");
}

void testInvalidVacantTimestampDoesNotCloseState() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "invalid-timestamp actor did not start");

    requireSubmitted(actor.submit(hallCommand(
        "hall-invalid-entry", 301, "OCCUPIED", 30000)),
        "invalid-timestamp setup entry failed");
    require(actor.waitUntilIdle(3s), "invalid-timestamp setup did not settle");
    const auto active_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='P01' "
        "AND status='ACTIVE';");

    requireSubmitted(actor.submit(hallCommand(
        "hall-invalid-vacant", 302, "VACANT", 29999)),
        "invalid VACANT admission failed");
    require(actor.waitUntilIdle(3s), "invalid VACANT did not become terminal");
    require(probe.integer(
                "SELECT session_id FROM PARKING_SESSION WHERE slot_id='P01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") == active_session,
            "invalid VACANT timestamp closed the active session");
    require(probe.text(
                "SELECT status FROM PARKING_SLOT WHERE slot_id='P01';") ==
                "OCCUPIED",
            "invalid VACANT timestamp changed the slot projection");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-invalid-vacant';") == "REJECTED_INVALID",
            "invalid VACANT did not receive a terminal rejection");
    require(probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                301,
            "invalid VACANT consumed its sequence watermark");
    require(actor.stopAndDrain(2s), "invalid-timestamp actor did not stop");
}

void testDurableHallConfirmationSurvivesRestart() {
    TemporaryDatabase temporary;
    const auto due = nowEpochMs() + 250;
    {
        database::EventDatabase database(temporary.path);
        initialize(database);
        SqliteProbe probe(temporary.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "confirmation first actor did not start");
        requireSubmitted(actor.submit(hallCommand(
            "hall-confirm-restart", 401, "OCCUPIED", due - 100, due)),
            "durable confirmation admission failed");
        require(waitUntil([&] {
            return probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-confirm-restart';") == 1;
        }), "Hall confirmation did not cross the durable admission boundary");
        require(probe.text(
                    "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='hall-confirm-restart';") ==
                    "PENDING_UNPREPARED",
                "Hall confirmation was not persisted before restart");
        require(probe.integer(
                    "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01';") ==
                    0,
                "Hall confirmation created a session before its deadline");
        require(!probe.optionalInt(
                    "SELECT CAST(last_sequence AS INTEGER) FROM "
                    "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';"),
                "pending Hall confirmation consumed its sequence");
        require(actor.stopAndDrain(2s),
                "confirmation first actor did not stop cleanly");
    }

    {
        database::EventDatabase database(temporary.path);
        database.migrateRuntimeSchema();
        SqliteProbe probe(temporary.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "confirmation restart actor did not start");
        require(actor.waitUntilIdle(3s),
                "restart did not replay durable Hall confirmation");
        require(probe.integer(
                    "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01' "
                    "AND status='ACTIVE';") == 1,
                "restarted Hall confirmation did not create its session");
        require(probe.integer(
                    "SELECT CAST(last_sequence AS INTEGER) FROM "
                    "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                    401,
                "restarted Hall confirmation did not commit its sequence");
        require(actor.stopAndDrain(2s),
                "confirmation restart actor did not stop cleanly");
    }
}

void testAdmittedExitPrecedesNewIntrusion() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto base = nowEpochMs() - 1000;

    const auto first = cameraCommand(
        "camera-first-intrusion", "INTRUSION", "object-old", base, 0);
    require(store.admit(first, 100).accepted(),
            "first camera intrusion was not admitted");
    const auto first_result = store.apply(first.commandId);
    require(first_result.code == parking::CommittedOccupancyCode::SessionStarted,
            "first camera intrusion did not start a session");
    // Some actor revisions make the authoritative command terminal in the
    // same transaction; older revisions keep a replayable effects phase.
    // Acknowledging is therefore intentionally idempotent/optional here.
    (void)store.completeEffects(first.commandId);
    const auto old_session = first_result.sessionId;

    const auto exit_due = nowEpochMs() - 100;
    const auto exit = cameraCommand(
        "camera-exit-before-new", "EXIT", "object-old",
        base + 100, exit_due);
    require(store.admit(exit, 100).accepted(), "camera exit was not admitted");
    require(store.apply(exit.commandId).code ==
                parking::CommittedOccupancyCode::ExitScheduled,
            "camera exit did not durably schedule its deadline");
    require(store.admitDueDeadlines(nowEpochMs(), 100) == 1,
            "due camera exit was not atomically admitted");
    const auto deadline_id = probe.text(
        "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE slot_id='EV01';");
    const std::string deadline_command = "deadline-command:" + deadline_id;

    probe.execute(
        "CREATE TRIGGER block_deadline_close BEFORE UPDATE OF status ON "
        "PARKING_SESSION WHEN NEW.status='ENDED' "
        "BEGIN SELECT RAISE(ABORT,'deadline barrier'); END;");
    auto actor = makeActor(store);
    require(actor.start(), "deadline-order actor did not start");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT attempt_count FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='" + deadline_command + "';") >= 1;
    }), "admitted deadline did not reach the SQLite close barrier");

    const auto newer = cameraCommand(
        "camera-new-intrusion", "INTRUSION", "object-new",
        nowEpochMs(), 0);
    requireSubmitted(actor.submit(newer),
                     "new intrusion was not admitted behind deadline");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='camera-new-intrusion';") == 1;
    }), "new intrusion did not cross the durable admission boundary");
    require(probe.integer(
                "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='" + deadline_command + "';") <
                probe.integer(
                    "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX "
                    "WHERE command_id='camera-new-intrusion';"),
            "new intrusion was ordered ahead of the already admitted deadline");

    probe.execute("DROP TRIGGER block_deadline_close;");
    require(actor.waitUntilIdle(3s),
            "deadline/new-intrusion ordering did not settle");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE session_id=" +
                std::to_string(old_session) + ";") == "ENDED",
            "admitted old deadline did not close its exact session first");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01';") ==
                2,
            "new intrusion did not create a new session generation");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") == 1,
            "final camera state is not exactly one new ACTIVE session");
    require(probe.text(
                "SELECT entry_command_id FROM PARKING_SESSION WHERE "
                "slot_id='EV01' AND status='ACTIVE';") ==
                "camera-new-intrusion",
            "final active camera session belongs to the wrong observation");
    require(actor.stopAndDrain(2s), "deadline-order actor did not stop");
}

void testIntrusionSupersedesScheduledExitBeforeAdmission() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto base = nowEpochMs();

    const auto first = cameraCommand(
        "camera-supersede-first", "INTRUSION", "object-stable", base, 0);
    require(store.admit(first, 100).accepted(),
            "supersede setup intrusion was not admitted");
    const auto first_result = store.apply(first.commandId);
    require(first_result.code == parking::CommittedOccupancyCode::SessionStarted,
            "supersede setup did not create a session");
    (void)store.completeEffects(first.commandId);

    const auto deadline_due = nowEpochMs() + 250;
    const auto exit = cameraCommand(
        "camera-supersede-exit", "EXIT", "object-stable",
        base + 10, deadline_due);
    require(store.admit(exit, 100).accepted(),
            "future camera exit was not admitted");
    require(store.apply(exit.commandId).code ==
                parking::CommittedOccupancyCode::ExitScheduled,
            "future camera exit was not persisted as SCHEDULED");
    const auto deadline_id = probe.text(
        "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE slot_id='EV01';");

    auto actor = makeActor(store);
    require(actor.start(), "deadline-supersede actor did not start");
    const auto refresh = cameraCommand(
        "camera-superseding-intrusion", "INTRUSION", "object-refresh",
        base + 20, 0);
    requireSubmitted(actor.submit(refresh),
                     "superseding intrusion was not admitted");
    require(actor.waitUntilIdle(3s),
            "superseding intrusion did not become terminal");
    require(probe.text(
                "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "deadline_id='" + deadline_id + "';") == "SUPERSEDED",
            "new intrusion did not supersede the not-yet-admitted deadline");

    std::this_thread::sleep_until(
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds(deadline_due + 100)});
    require(actor.waitUntilIdle(1s),
            "actor did not remain idle after superseded deadline became due");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") == 1,
            "superseded deadline closed the refreshed active session");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='deadline-command:" + deadline_id + "';") == 0,
            "superseded deadline was later admitted as a command");
    require(actor.stopAndDrain(2s), "deadline-supersede actor did not stop");
}

void testCameraDeadlineStatesSurviveDatabaseReopen() {
    // SCHEDULED -> reopen -> exactly one ADMITTED/APPLIED command.
    TemporaryDatabase scheduled_database;
    std::string scheduled_deadline_id;
    std::int64_t scheduled_session{};
    const auto scheduled_due = nowEpochMs() + 150;
    {
        database::EventDatabase database(scheduled_database.path);
        initialize(database);
        database::SessionTransitionStore store(database);
        const auto intrusion = cameraCommand(
            "reopen-scheduled-entry", "INTRUSION", "scheduled-object",
            scheduled_due - 1000, 0);
        require(store.admit(intrusion, 100).accepted(),
                "scheduled-reopen entry admission failed");
        const auto started = store.apply(intrusion.commandId);
        require(started.code ==
                    parking::CommittedOccupancyCode::SessionStarted,
                "scheduled-reopen entry did not commit");
        scheduled_session = started.sessionId;
        (void)store.completeEffects(intrusion.commandId);
        const auto exit = cameraCommand(
            "reopen-scheduled-exit", "EXIT", "scheduled-object",
            scheduled_due - 500, scheduled_due);
        require(store.admit(exit, 100).accepted(),
                "scheduled-reopen exit admission failed");
        require(store.apply(exit.commandId).code ==
                    parking::CommittedOccupancyCode::ExitScheduled,
                "scheduled-reopen deadline was not persisted");
        SqliteProbe probe(scheduled_database.path);
        scheduled_deadline_id = probe.text(
            "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE "
            "slot_id='EV01';");
        require(probe.text(
                    "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                    "deadline_id='" + scheduled_deadline_id + "';") ==
                    "SCHEDULED",
                "deadline was not SCHEDULED at database close boundary");
    }
    std::this_thread::sleep_until(std::chrono::system_clock::time_point{
        std::chrono::milliseconds(scheduled_due + 25)});
    {
        database::EventDatabase database(scheduled_database.path);
        database.migrateRuntimeSchema();
        SqliteProbe probe(scheduled_database.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "scheduled-reopen actor did not start");
        require(actor.waitUntilIdle(3s),
                "scheduled deadline did not settle after database reopen");
        require(probe.text(
                    "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                    "deadline_id='" + scheduled_deadline_id + "';") ==
                    "APPLIED",
                "scheduled deadline was not applied after reopen");
        require(probe.text(
                    "SELECT status FROM PARKING_SESSION WHERE session_id=" +
                    std::to_string(scheduled_session) + ";") == "ENDED",
                "reopened scheduled deadline did not close its exact session");
        require(probe.integer(
                    "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='deadline-command:" + scheduled_deadline_id +
                    "';") == 1,
                "reopened scheduled deadline was not admitted exactly once");
        require(actor.stopAndDrain(2s),
                "scheduled-reopen actor did not stop");
    }
    {
        database::EventDatabase database(scheduled_database.path);
        database.migrateRuntimeSchema();
        SqliteProbe probe(scheduled_database.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "applied-reopen actor did not start");
        require(actor.waitUntilIdle(1s),
                "already APPLIED deadline replay did not remain idle");
        require(probe.integer(
                    "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='deadline-command:" + scheduled_deadline_id +
                    "';") == 1,
                "APPLIED deadline was duplicated on another reopen");
        require(actor.stopAndDrain(2s),
                "applied-reopen actor did not stop");
    }

    // ADMITTED command -> reopen -> exact command replay, never a second row.
    TemporaryDatabase admitted_database;
    std::string admitted_deadline_id;
    {
        database::EventDatabase database(admitted_database.path);
        initialize(database);
        database::SessionTransitionStore store(database);
        const auto base = nowEpochMs() - 1000;
        const auto intrusion = cameraCommand(
            "reopen-admitted-entry", "INTRUSION", "admitted-object", base, 0);
        require(store.admit(intrusion, 100).accepted(),
                "admitted-reopen entry admission failed");
        require(store.apply(intrusion.commandId).code ==
                    parking::CommittedOccupancyCode::SessionStarted,
                "admitted-reopen entry did not commit");
        (void)store.completeEffects(intrusion.commandId);
        const auto exit = cameraCommand(
            "reopen-admitted-exit", "EXIT", "admitted-object", base + 100,
            nowEpochMs() - 1);
        require(store.admit(exit, 100).accepted(),
                "admitted-reopen exit admission failed");
        require(store.apply(exit.commandId).code ==
                    parking::CommittedOccupancyCode::ExitScheduled,
                "admitted-reopen exit was not scheduled");
        require(store.admitDueDeadlines(nowEpochMs(), 100) == 1,
                "deadline was not ADMITTED before close");
        SqliteProbe probe(admitted_database.path);
        admitted_deadline_id = probe.text(
            "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE "
            "slot_id='EV01';");
        require(probe.text(
                    "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                    "deadline_id='" + admitted_deadline_id + "';") ==
                    "ADMITTED",
                "deadline lost ADMITTED state before close");
    }
    {
        database::EventDatabase database(admitted_database.path);
        database.migrateRuntimeSchema();
        SqliteProbe probe(admitted_database.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "admitted-reopen actor did not start");
        require(actor.waitUntilIdle(3s),
                "ADMITTED deadline did not replay after reopen");
        require(probe.text(
                    "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                    "deadline_id='" + admitted_deadline_id + "';") ==
                    "APPLIED",
                "ADMITTED deadline did not become APPLIED after reopen");
        require(probe.integer(
                    "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='deadline-command:" + admitted_deadline_id +
                    "';") == 1,
                "ADMITTED deadline replay created a duplicate command");
        require(actor.stopAndDrain(2s),
                "admitted-reopen actor did not stop");
    }

    // SUPERSEDED -> reopen after due -> zero deadline commands.
    TemporaryDatabase superseded_database;
    std::string superseded_deadline_id;
    const auto superseded_due = nowEpochMs() + 120;
    {
        database::EventDatabase database(superseded_database.path);
        initialize(database);
        database::SessionTransitionStore store(database);
        const auto intrusion = cameraCommand(
            "reopen-superseded-entry", "INTRUSION", "old-object",
            superseded_due - 1000, 0);
        require(store.admit(intrusion, 100).accepted(),
                "superseded-reopen entry admission failed");
        require(store.apply(intrusion.commandId).code ==
                    parking::CommittedOccupancyCode::SessionStarted,
                "superseded-reopen entry did not commit");
        (void)store.completeEffects(intrusion.commandId);
        const auto exit = cameraCommand(
            "reopen-superseded-exit", "EXIT", "old-object",
            superseded_due - 500, superseded_due);
        require(store.admit(exit, 100).accepted(),
                "superseded-reopen exit admission failed");
        require(store.apply(exit.commandId).code ==
                    parking::CommittedOccupancyCode::ExitScheduled,
                "superseded-reopen exit was not scheduled");
        const auto refresh = cameraCommand(
            "reopen-superseding-intrusion", "INTRUSION", "new-object",
            superseded_due - 250, 0);
        require(store.admit(refresh, 100).accepted(),
                "superseding refresh admission failed");
        require(store.apply(refresh.commandId).code ==
                    parking::CommittedOccupancyCode::NoChange,
                "superseding refresh did not preserve the active session");
        SqliteProbe probe(superseded_database.path);
        superseded_deadline_id = probe.text(
            "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE "
            "slot_id='EV01';");
        require(probe.text(
                    "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                    "deadline_id='" + superseded_deadline_id + "';") ==
                    "SUPERSEDED",
                "refresh did not persist SUPERSEDED before close");
    }
    std::this_thread::sleep_until(std::chrono::system_clock::time_point{
        std::chrono::milliseconds(superseded_due + 25)});
    {
        database::EventDatabase database(superseded_database.path);
        database.migrateRuntimeSchema();
        SqliteProbe probe(superseded_database.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "superseded-reopen actor did not start");
        require(actor.waitUntilIdle(1s),
                "superseded deadline caused work after reopen");
        require(probe.integer(
                    "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='deadline-command:" + superseded_deadline_id +
                    "';") == 0,
                "SUPERSEDED deadline was admitted after reopen");
        require(probe.integer(
                    "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                    "AND status='ACTIVE' AND exit_time IS NULL;") == 1,
                "SUPERSEDED deadline changed the active session on reopen");
        require(actor.stopAndDrain(2s),
                "superseded-reopen actor did not stop");
    }
}

void testSameSlotOrdinalOccupiedVacantOccupied() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);

    const auto occupied_one = hallCommand(
        "hall-ordinal-occ-1", 501, "OCCUPIED", 50000, 0);
    const auto vacant = hallCommand(
        "hall-ordinal-vac", 502, "VACANT", 51000, 0);
    const auto occupied_two = hallCommand(
        "hall-ordinal-occ-2", 503, "OCCUPIED", 52000, 0);
    require(store.admit(occupied_one, 100).accepted(),
            "ordinal OCC1 admission failed");
    require(store.admit(vacant, 100).accepted(),
            "ordinal VAC admission failed");
    require(store.admit(occupied_two, 100).accepted(),
            "ordinal OCC2 admission failed");
    require(probe.integer(
                "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-ordinal-occ-1';") <
                probe.integer(
                    "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX "
                    "WHERE command_id='hall-ordinal-vac';") &&
                probe.integer(
                    "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX "
                    "WHERE command_id='hall-ordinal-vac';") <
                probe.integer(
                    "SELECT admission_ordinal FROM OCCUPANCY_COMMAND_INBOX "
                    "WHERE command_id='hall-ordinal-occ-2';"),
            "same-slot durable admission ordinals are not OCC/VAC/OCC");

    auto actor = makeActor(store);
    require(actor.start(), "ordinal actor did not start");
    require(actor.waitUntilIdle(3s), "ordinal actor did not drain commands");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01';") ==
                2,
            "OCC/VAC/OCC did not create two exact session generations");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE entry_command_id="
                "'hall-ordinal-occ-1';") == "ENDED",
            "first OCC generation was not ended by the ordered VAC");
    require(probe.text(
                "SELECT exit_command_id FROM PARKING_SESSION WHERE "
                "entry_command_id='hall-ordinal-occ-1';") ==
                "hall-ordinal-vac",
            "ordered VAC closed a session other than the first OCC generation");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE entry_command_id="
                "'hall-ordinal-occ-2';") == "ACTIVE",
            "second OCC generation is not the final ACTIVE session");
    require(probe.integer(
                "SELECT CAST(last_sequence AS INTEGER) FROM "
                "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id='hall-p01';") ==
                503,
            "OCC/VAC/OCC sequence watermark did not follow durable order");
    require(actor.stopAndDrain(2s), "ordinal actor did not stop");
}

void testCommittedEffectReplaysAfterRestart() {
    TemporaryDatabase temporary;
    const auto occurred = nowEpochMs() - 100;
    {
        database::EventDatabase database(temporary.path);
        initialize(database);
        SqliteProbe probe(temporary.path);
        database::SessionTransitionStore store(database);
        const auto command = hallCommand(
            "hall-effect-crash", 601, "OCCUPIED", occurred, 0);
        require(store.admit(command, 100).accepted(),
                "effect crash command admission failed");
        const auto committed = store.apply(command.commandId);
        require(committed.code ==
                    parking::CommittedOccupancyCode::SessionStarted,
                "effect crash setup did not commit its session");
        require(probe.text(
                    "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='hall-effect-crash';") == "APPLIED",
                "authoritative command was not terminal at crash boundary");
        require(probe.text(
                    "SELECT effect_state FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='hall-effect-crash';") == "PENDING",
                "committed effect was not durable before simulated crash");
    }

    database::EventDatabase database(temporary.path);
    database.migrateRuntimeSchema();
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    int replay_count{};
    parking::SlotTransitionActor actor(
        store, actorConfig(),
        [&](const parking::CommittedOccupancyTransition& effect) {
            ++replay_count;
            require(effect.commandId == "hall-effect-crash",
                    "restart replay used the wrong command identity");
            require(effect.code ==
                        parking::CommittedOccupancyCode::SessionStarted,
                    "restart replay used the wrong committed result");
            require(effect.sessionId > 0,
                    "restart replay lost the exact SQLite session id");
            return true;
        });
    require(actor.start(), "effect replay actor did not start");
    require(replay_count == 1,
            "pending committed effect was not replayed before ingress opened");
    require(probe.text(
                "SELECT effect_state FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-effect-crash';") == "APPLIED",
            "restart replay acknowledgement was not persisted");
    require(actor.stopAndDrain(2s), "effect replay actor did not stop");
}

void testEffectFailureDoesNotBlockAuthoritativeVacant() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    std::atomic<bool> accept_effects{false};
    parking::SlotTransitionActor actor(
        store, actorConfig(),
        [&](const parking::CommittedOccupancyTransition&) {
            return accept_effects.load(std::memory_order_acquire);
        });
    require(actor.start(), "nonblocking-effect actor did not start");
    const auto base = nowEpochMs() - 1000;
    requireSubmitted(actor.submit(hallCommand(
        "hall-effect-occ", 611, "OCCUPIED", base, 0)),
        "effect-failure OCC submit failed");
    require(waitUntil([&] {
        return probe.integer(
            "SELECT effect_attempt_count FROM OCCUPANCY_COMMAND_INBOX WHERE "
            "command_id='hall-effect-occ';") >= 1;
    }), "committed OCC effect did not enter durable retry");

    requireSubmitted(actor.submit(hallCommand(
        "hall-effect-vac", 612, "VACANT", base + 500, 0)),
        "effect-failure VAC submit failed");
    require(waitUntil([&] {
        return probe.text(
            "SELECT status FROM PARKING_SESSION WHERE "
            "entry_command_id='hall-effect-occ';") == "ENDED";
    }), "pending start projection blocked authoritative VACANT");
    require(probe.text(
                "SELECT exit_command_id FROM PARKING_SESSION WHERE "
                "entry_command_id='hall-effect-occ';") == "hall-effect-vac",
            "VACANT did not close the exact committed session");

    accept_effects.store(true, std::memory_order_release);
    require(actor.waitUntilIdle(3s),
            "durable start/end effects did not drain after recovery");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "effect_state='PENDING';") == 0,
            "effect recovery left a pending projection row");
    require(actor.stopAndDrain(2s),
            "nonblocking-effect actor did not stop");
}

void testCameraObjectOverlapAndLegacyTakeover() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto base = nowEpochMs() - 1000;
    const auto future = nowEpochMs() + 1000;

    const auto object_a = cameraCommand(
        "camera-overlap-a-in", "INTRUSION", "object-A", base, 0);
    const auto object_b = cameraCommand(
        "camera-overlap-b-in", "INTRUSION", "object-B", base + 10, 0);
    const auto object_a_exit = cameraCommand(
        "camera-overlap-a-out", "EXIT", "object-A", base + 20, future);
    require(store.admit(object_a, 100).accepted(),
            "object A admission failed");
    require(store.apply(object_a.commandId).code ==
                parking::CommittedOccupancyCode::SessionStarted,
            "object A did not start the camera session");
    require(store.admit(object_b, 100).accepted(),
            "object B admission failed");
    require(store.apply(object_b.commandId).code ==
                parking::CommittedOccupancyCode::NoChange,
            "object B did not join the active area set");
    require(store.admit(object_a_exit, 100).accepted(),
            "object A exit admission failed");
    require(store.apply(object_a_exit.commandId).code ==
                parking::CommittedOccupancyCode::NoChange,
            "object A exit ignored the still-active object B");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01' AND state='SCHEDULED';") == 0,
            "object A exit scheduled departure while object B remained");
    require(probe.text(
                "SELECT area_states_json FROM IVA_SLOT_OBSERVATION_STATE "
                "WHERE slot_id='EV01';") == "{\"IVA1\":[\"object-B\"]}",
            "camera reducer did not retain the exact active object set");

    // Simulate an old database whose area state was a boolean. The first
    // exact observation must take over that anonymous state, otherwise its
    // sentinel can keep the slot occupied forever.
    probe.execute(
        "UPDATE IVA_SLOT_OBSERVATION_STATE SET "
        "area_states_json='{\"IVA1\":true}' WHERE slot_id='EV01';");
    const auto takeover_exit = cameraCommand(
        "camera-legacy-b-out", "EXIT", "object-B", base + 30, future);
    require(store.admit(takeover_exit, 100).accepted(),
            "legacy takeover exit admission failed");
    require(store.apply(takeover_exit.commandId).code ==
                parking::CommittedOccupancyCode::ExitScheduled,
            "legacy sentinel survived the first exact EXIT after upgrade");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "slot_id='EV01' AND state='SCHEDULED';") == 1,
            "exact final object exit did not persist its deadline");
}

void testQueuedIntrusionWinsBeforeDeadlineLinearization() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto base = nowEpochMs() - 1000;
    const auto due = nowEpochMs() + 500;

    const auto first = cameraCommand(
        "camera-race-first", "INTRUSION", "object-race", base, 0);
    require(store.admit(first, 100).accepted(),
            "deadline race setup intrusion admission failed");
    require(store.apply(first.commandId).code ==
                parking::CommittedOccupancyCode::SessionStarted,
            "deadline race setup did not start a session");
    require(store.completeEffects(first.commandId),
            "deadline race setup effect did not acknowledge");
    const auto exit = cameraCommand(
        "camera-race-exit", "EXIT", "object-race", base + 10, due);
    require(store.admit(exit, 100).accepted(),
            "deadline race EXIT admission failed");
    require(store.apply(exit.commandId).code ==
                parking::CommittedOccupancyCode::ExitScheduled,
            "deadline race EXIT did not schedule a deadline");
    const auto deadline_id = probe.text(
        "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE WHERE "
        "slot_id='EV01';");

    std::mutex checkpoint_mutex;
    std::condition_variable checkpoint_condition;
    bool armed{};
    bool entered{};
    bool release{};
    auto config = actorConfig();
    config.checkpoint = [&](const parking::SlotActorCheckpoint checkpoint) {
        if (checkpoint !=
            parking::SlotActorCheckpoint::BeforeDeadlineLinearization) return;
        std::unique_lock lock(checkpoint_mutex);
        if (!armed) return;
        entered = true;
        checkpoint_condition.notify_all();
        checkpoint_condition.wait(lock, [&] { return release; });
    };
    parking::SlotTransitionActor actor(
        store, std::move(config),
        [](const parking::CommittedOccupancyTransition&) { return true; });
    require(actor.start(), "deadline race actor did not start");
    {
        std::lock_guard lock(checkpoint_mutex);
        armed = true;
    }
    {
        std::unique_lock lock(checkpoint_mutex);
        require(checkpoint_condition.wait_for(
                    lock, 2s, [&] { return entered; }),
                "actor did not reach the pre-deadline checkpoint");
    }

    const auto refresh = cameraCommand(
        "camera-race-refresh", "INTRUSION", "object-race",
        base + 20, 0);
    requireSubmitted(actor.submit(refresh),
                     "queued race intrusion was rejected");
    {
        std::lock_guard lock(checkpoint_mutex);
        release = true;
    }
    checkpoint_condition.notify_all();
    require(actor.waitUntilIdle(3s),
            "queued race intrusion did not settle");
    require(probe.text(
                "SELECT state FROM OCCUPANCY_EXIT_DEADLINE WHERE "
                "deadline_id='" + deadline_id + "';") == "SUPERSEDED",
            "deadline overtook an intrusion queued before linearization");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                "AND status='ACTIVE' AND exit_time IS NULL;") == 1,
            "deadline race ended the refreshed active session");
    require(actor.stopAndDrain(2s), "deadline race actor did not stop");
}

void testOverdueHallConfirmationPrecedesVacant() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    const auto due = nowEpochMs() - 100;
    const auto occupied = hallCommand(
        "hall-overdue-confirm", 701, "OCCUPIED", due - 500, due);
    const auto vacant = hallCommand(
        "hall-after-confirm", 702, "VACANT", due + 50, 0);
    require(store.admit(occupied, 100).accepted(),
            "overdue Hall confirmation admission failed");
    require(store.admit(vacant, 100).accepted(),
            "post-confirm VACANT admission failed");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='hall-overdue-confirm';") ==
                "PENDING_UNPREPARED",
            "post-deadline VACANT canceled an overdue OCCUPIED head");

    auto actor = makeActor(store);
    require(actor.start(), "overdue confirmation actor did not start");
    require(actor.waitUntilIdle(3s),
            "overdue OCCUPIED then VACANT did not settle");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='P01';") ==
                1,
            "overdue OCCUPIED was skipped instead of creating its generation");
    require(probe.text(
                "SELECT status FROM PARKING_SESSION WHERE "
                "entry_command_id='hall-overdue-confirm';") == "ENDED",
            "following VACANT did not end the overdue OCCUPIED generation");
    require(probe.text(
                "SELECT exit_command_id FROM PARKING_SESSION WHERE "
                "entry_command_id='hall-overdue-confirm';") ==
                "hall-after-confirm",
            "post-confirm VACANT closed the wrong Hall generation");
    require(actor.stopAndDrain(2s),
            "overdue confirmation actor did not stop");
}

void testHybridOrUsesOneSessionAndSourceAwareExit() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "hybrid OR actor did not start");

    const auto base = nowEpochMs() - 1000;

    // IVA-only: an unrelated Hall VACANT cannot terminate a session that the
    // Hall sensor never confirmed.
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-iva-only-in", "INTRUSION", "iva-only", base, 0)),
        "hybrid IVA-only intrusion failed");
    require(actor.waitUntilIdle(3s), "hybrid IVA-only entry did not settle");
    const auto iva_only_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='EV01' "
        "AND exit_time IS NULL;");
    require(probe.integer(
                "SELECT iva_confirmed*1000+iva_occupied*100+"
                "hall_confirmed*10+hall_occupied FROM PARKING_SESSION WHERE "
                "session_id=" + std::to_string(iva_only_session) + ";") == 1100,
            "IVA-only session source state is incorrect");
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-iva-only-hall-vacant", 1, "VACANT", base + 100)),
        "hybrid IVA-only Hall VACANT failed");
    require(actor.waitUntilIdle(3s),
            "hybrid IVA-only Hall VACANT did not settle");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE session_id=" +
                std::to_string(iva_only_session) +
                " AND exit_time IS NULL;") == 1,
            "Hall VACANT closed an IVA-only session");
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-iva-only-out", "EXIT", "iva-only", base + 200,
        nowEpochMs() + 40)), "hybrid IVA-only exit failed");
    require(waitUntil([&] {
                return probe.integer(
                           "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                           "session_id=" + std::to_string(iva_only_session) +
                           " AND exit_time IS NOT NULL;") == 1;
            }), "IVA-only session did not close on confirmed IVA Exit");

    // Hall-only: the durable Hall confirmation creates the session and Hall
    // VACANT owns its exit without waiting for a camera event.
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-hall-only-in", 2, "OCCUPIED", base + 300)),
        "hybrid Hall-only entry failed");
    require(actor.waitUntilIdle(3s), "hybrid Hall-only entry did not settle");
    const auto hall_only_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='EV01' "
        "AND exit_time IS NULL;");
    require(hall_only_session != iva_only_session,
            "Hall-only entry reused an ended session");
    require(probe.integer(
                "SELECT iva_confirmed*1000+iva_occupied*100+"
                "hall_confirmed*10+hall_occupied FROM PARKING_SESSION WHERE "
                "session_id=" + std::to_string(hall_only_session) + ";") == 11,
            "Hall-only session source state is incorrect");
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-hall-only-out", 3, "VACANT", base + 400)),
        "hybrid Hall-only exit failed");
    require(waitUntil([&] {
                return probe.integer(
                           "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                           "session_id=" + std::to_string(hall_only_session) +
                           " AND exit_time IS NOT NULL;") == 1;
            }), "Hall-only session did not close on Hall VACANT");

    // Both sensors: the second arrival enriches the same session. Hall VACANT
    // alone is recorded but must not close while IVA remains occupied.
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-both-camera-in", "INTRUSION", "both-a", base + 500, 0)),
        "hybrid both-sensor camera entry failed");
    require(actor.waitUntilIdle(3s),
            "hybrid both-sensor camera entry did not settle");
    const auto both_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='EV01' "
        "AND exit_time IS NULL;");
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-both-hall-in", 4, "OCCUPIED", base + 600)),
        "hybrid both-sensor Hall confirmation failed");
    require(actor.waitUntilIdle(3s),
            "hybrid both-sensor Hall confirmation did not settle");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE slot_id='EV01' "
                "AND exit_time IS NULL;") == 1 &&
                probe.integer(
                    "SELECT hall_confirmed+iva_confirmed+hall_occupied+"
                    "iva_occupied FROM PARKING_SESSION WHERE session_id=" +
                    std::to_string(both_session) + ";") == 4,
            "second sensor created a duplicate session or failed to confirm");
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-both-hall-out", 5, "VACANT", base + 700)),
        "hybrid both-sensor Hall VACANT failed");
    require(actor.waitUntilIdle(3s),
            "hybrid both-sensor Hall VACANT did not settle");
    require(probe.integer(
                "SELECT hall_occupied*10+iva_occupied FROM PARKING_SESSION "
                "WHERE session_id=" + std::to_string(both_session) + ";") == 1,
            "Hall VACANT did not preserve the active IVA-owned session");
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-both-camera-out", "EXIT", "both-a", base + 800,
        nowEpochMs() + 40)), "hybrid both-sensor camera exit failed");
    require(waitUntil([&] {
                return probe.integer(
                           "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                           "session_id=" + std::to_string(both_session) +
                           " AND exit_time IS NOT NULL;") == 1;
            }), "both-sensor session did not close after both sources vacated");

    // Reverse order: IVA Exit becomes vacant but Hall keeps the same session
    // alive until its own VACANT observation arrives.
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-reverse-hall-in", 6, "OCCUPIED", base + 900)),
        "hybrid reverse Hall entry failed");
    require(actor.waitUntilIdle(3s), "hybrid reverse Hall entry did not settle");
    const auto reverse_session = probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE slot_id='EV01' "
        "AND exit_time IS NULL;");
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-reverse-camera-in", "INTRUSION", "both-b", base + 1000, 0)),
        "hybrid reverse IVA confirmation failed");
    require(actor.waitUntilIdle(3s),
            "hybrid reverse IVA confirmation did not settle");
    requireSubmitted(actor.submit(hybridCameraCommand(
        "hybrid-reverse-camera-out", "EXIT", "both-b", base + 1100,
        nowEpochMs() + 40)), "hybrid reverse IVA exit failed");
    require(waitUntil([&] {
                return probe.integer(
                           "SELECT iva_occupied FROM PARKING_SESSION WHERE "
                           "session_id=" + std::to_string(reverse_session) +
                           ";") == 0;
            }), "hybrid reverse IVA exit did not record source vacancy");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE session_id=" +
                std::to_string(reverse_session) +
                " AND exit_time IS NULL;") == 1,
            "IVA Exit closed a session still held by Hall");
    requireSubmitted(actor.submit(hybridHallCommand(
        "hybrid-reverse-hall-out", 7, "VACANT", base + 1200)),
        "hybrid reverse Hall exit failed");
    require(waitUntil([&] {
                return probe.integer(
                           "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                           "session_id=" + std::to_string(reverse_session) +
                           " AND exit_time IS NOT NULL;") == 1;
            }), "reverse-order session did not close after Hall VACANT");

    require(actor.stopAndDrain(2s), "hybrid OR actor did not stop");
}

void testHybridSourceStateSurvivesDatabaseReopen() {
    TemporaryDatabase temporary;
    std::int64_t session_id{};
    const auto base = nowEpochMs() - 1000;

    {
        database::EventDatabase database(temporary.path);
        initialize(database);
        SqliteProbe probe(temporary.path);
        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "hybrid reopen first actor did not start");

        requireSubmitted(actor.submit(hybridCameraCommand(
            "hybrid-reopen-camera-in", "INTRUSION", "reopen-object",
            base, 0)), "hybrid reopen camera entry failed");
        requireSubmitted(actor.submit(hybridHallCommand(
            "hybrid-reopen-hall-in", 81, "OCCUPIED", base + 100)),
            "hybrid reopen Hall confirmation failed");
        require(actor.waitUntilIdle(3s),
                "hybrid reopen source confirmations did not settle");
        session_id = probe.integer(
            "SELECT session_id FROM PARKING_SESSION WHERE slot_id='EV01' "
            "AND exit_time IS NULL;");

        requireSubmitted(actor.submit(hybridHallCommand(
            "hybrid-reopen-hall-out", 82, "VACANT", base + 200)),
            "hybrid reopen Hall VACANT failed");
        require(actor.waitUntilIdle(3s),
                "hybrid reopen Hall VACANT did not settle");
        require(probe.integer(
                    "SELECT hall_confirmed*1000+hall_occupied*100+"
                    "iva_confirmed*10+iva_occupied FROM PARKING_SESSION "
                    "WHERE session_id=" + std::to_string(session_id) + ";") ==
                    1011,
                "pre-reopen hybrid source state is incorrect");
        require(actor.stopAndDrain(2s),
                "hybrid reopen first actor did not stop");
    }

    {
        database::EventDatabase database(temporary.path);
        database.migrateRuntimeSchema();
        database.migrateRuntimeSchema();
        SqliteProbe probe(temporary.path);
        require(probe.integer(
                    "SELECT hall_confirmed*1000+hall_occupied*100+"
                    "iva_confirmed*10+iva_occupied FROM PARKING_SESSION "
                    "WHERE session_id=" + std::to_string(session_id) + ";") ==
                    1011,
                "runtime migration overwrote persisted hybrid source state");

        database::SessionTransitionStore store(database);
        auto actor = makeActor(store);
        require(actor.start(), "hybrid reopen second actor did not start");
        requireSubmitted(actor.submit(hybridCameraCommand(
            "hybrid-reopen-camera-out", "EXIT", "reopen-object",
            base + 300, nowEpochMs() + 40)),
            "hybrid reopen camera exit failed");
        require(waitUntil([&] {
                    return probe.integer(
                               "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                               "session_id=" + std::to_string(session_id) +
                               " AND exit_time IS NOT NULL;") == 1;
                }), "reopened hybrid session did not close after IVA Exit");
        require(actor.stopAndDrain(2s),
                "hybrid reopen second actor did not stop");
    }
}

void testInitializeMigratesOldDatabaseAndIsIdempotent() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    SqliteProbe probe(temporary.path);
    probe.execute(
        "PRAGMA foreign_keys=OFF;"
        "CREATE TABLE VEHICLE(vehicle_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "plate_number TEXT UNIQUE NOT NULL,is_ev INTEGER NOT NULL DEFAULT 0,"
        "registered_at TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE PARKING_SLOT(slot_id TEXT PRIMARY KEY,slot_type TEXT "
        "NOT NULL,status TEXT NOT NULL,sensor_type TEXT,updated_at TEXT);"
        "CREATE TABLE PARKING_SESSION(session_id INTEGER PRIMARY KEY "
        "AUTOINCREMENT,vehicle_id INTEGER,slot_id TEXT NOT NULL,plate_number "
        "TEXT,entry_time TEXT NOT NULL,exit_time TEXT,duration_sec INTEGER "
        "DEFAULT 0,status TEXT NOT NULL);"
        "CREATE TABLE IMAGE_LOG(image_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "session_id INTEGER,original_image_path TEXT,enhanced_image_path TEXT,"
        "enhancement_type TEXT,ocr_result TEXT,captured_at TEXT);"
        "CREATE TABLE EVENT_LOG(event_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "session_id INTEGER,slot_id TEXT,event_type TEXT NOT NULL,message TEXT,"
        "created_at TEXT,handled INTEGER DEFAULT 0);"
        "INSERT INTO PARKING_SLOT(slot_id,slot_type,status,sensor_type,"
        "updated_at) VALUES('P01','NORMAL','OCCUPIED','HALL',"
        "CURRENT_TIMESTAMP);"
        "INSERT INTO PARKING_SESSION(slot_id,entry_time,status) VALUES("
        "'P01','2026-08-11 00:00:00','ACTIVE');");

    const std::filesystem::path sql_dir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");
    database.initialize(sql_dir / "schema.sql", sql_dir / "seed.sql");
    require(probe.integer(
                "SELECT COUNT(*) FROM pragma_table_info('PARKING_SESSION') "
                "WHERE name IN ('occupancy_attempt_id','entry_command_id',"
                "'exit_command_id','entry_time_epoch_ms','exit_time_epoch_ms');") ==
                5,
            "old PARKING_SESSION did not receive actor identity columns");
    require(probe.integer(
                "SELECT COUNT(*) FROM pragma_table_info('PARKING_SESSION') "
                "WHERE name IN ('hall_confirmed','iva_confirmed',"
                "'hall_occupied','iva_occupied');") == 4,
            "old PARKING_SESSION did not receive hybrid source-state columns");
    require(probe.text(
                "SELECT occupancy_attempt_id FROM PARKING_SESSION WHERE "
                "session_id=1;") == "legacy-attempt:1",
            "old active session did not receive deterministic attempt identity");
    require(probe.text(
                "SELECT entry_command_id FROM PARKING_SESSION WHERE "
                "session_id=1;") == "legacy-entry:1",
            "old active session did not receive deterministic command identity");
    require(probe.integer(
                "SELECT COUNT(*) FROM pragma_table_info("
                "'OCCUPANCY_COMMAND_INBOX') WHERE name='effect_state';") == 1,
            "idempotent migration did not install durable effect state");
    require(probe.integer(
                "SELECT COUNT(*) FROM pragma_foreign_key_check;") == 0,
            "idempotent old-database migration left a foreign-key violation");

    probe.execute(
        "UPDATE PARKING_SLOT SET status='OCCUPIED' WHERE slot_id='EV01';"
        "INSERT INTO PARKING_SESSION(slot_id,entry_time,status,"
        "occupancy_attempt_id,entry_command_id,entry_time_epoch_ms) VALUES("
        "'EV01','2026-08-11 01:00:00','ACTIVE',"
        "'occupancy:prepared-committed','prepared-committed',1786410000000);"
        "INSERT INTO OCCUPANCY_COMMAND_INBOX(command_id,slot_id,source_kind,"
        "sensor_id,source_identity,source_sequence,occurred_at,payload_json,"
        "due_at_epoch_ms,admission_ordinal,status,occupancy_attempt_id,"
        "result_code,result_session_id,effect_state) VALUES("
        "'prepared-committed','EV01','CAMERA_OBSERVATION','',"
        "'migration:prepared-committed',NULL,'2026-08-11T01:00:00Z',"
        "'{\"action\":\"INTRUSION\",\"object_id\":\"migrated-object\","
        "\"area_key\":\"IVA1\",\"configured_areas\":[\"IVA1\"],"
        "\"occurred_at_epoch_ms\":1786410000000,"
        "\"deadline_due_at_epoch_ms\":0}',0,1001,'PENDING_PREPARED',"
        "'occupancy:prepared-committed','SESSION_STARTED',NULL,'NONE');"
        "INSERT INTO OCCUPANCY_COMMAND_INBOX(command_id,slot_id,source_kind,"
        "sensor_id,source_identity,source_sequence,occurred_at,payload_json,"
        "due_at_epoch_ms,admission_ordinal,status,result_code,effect_state) "
        "VALUES('prepared-uncommitted','EV02','HALL_OBSERVATION','hall-ev02',"
        "'migration:prepared-uncommitted','1','2026-08-11T01:00:01Z',"
        "'{\"state\":\"OCCUPIED\",\"transport\":\"UART\","
        "\"occurred_at_epoch_ms\":1786410001000}',0,1002,"
        "'PENDING_PREPARED','SESSION_STARTED','NONE');");

    database.migrateRuntimeSchema();
    database.migrateRuntimeSchema();
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='prepared-committed';") == "APPLIED",
            "proven committed prepared row was not promoted");
    require(probe.text(
                "SELECT effect_state FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='prepared-committed';") == "PENDING",
            "proven committed prepared row lost its replayable effect");
    require(probe.integer(
                "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX c JOIN "
                "PARKING_SESSION s ON s.entry_command_id=c.command_id WHERE "
                "c.command_id='prepared-committed' AND "
                "c.result_session_id=s.session_id;") == 1,
            "prepared migration did not bind the exact committed session");
    require(probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='prepared-uncommitted';") ==
                "PENDING_UNPREPARED",
            "ambiguous prepared row was guessed successful instead of replayed");
    require(probe.text(
                "SELECT result_code FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='prepared-uncommitted';").empty(),
            "ambiguous prepared replay retained a fabricated result");

    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(actor.start(), "prepared migration actor did not start");
    require(actor.waitUntilIdle(3s),
            "prepared migration replay/effect did not settle");
    require(probe.integer(
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE "
                "entry_command_id='prepared-uncommitted';") == 1,
            "ambiguous prepared command was not replayed authoritatively");
    require(actor.stopAndDrain(2s),
            "prepared migration actor did not stop");
}

void testMigrationFailureKeepsActorIngressClosed() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    initialize(database);
    SqliteProbe probe(temporary.path);
    probe.execute(
        "INSERT INTO OCCUPANCY_COMMAND_INBOX(command_id,slot_id,source_kind,"
        "sensor_id,source_identity,source_sequence,occurred_at,payload_json,"
        "due_at_epoch_ms,admission_ordinal,status,effect_state) VALUES("
        "'migration-failure-command','P02','HALL_OBSERVATION','hall-p02',"
        "'migration-failure-source','1','2026-08-11T05:00:00Z',"
        "'{\"state\":\"OCCUPIED\",\"transport\":\"UART\","
        "\"occurred_at_epoch_ms\":1786424400000}',0,9001,"
        "'PENDING_PREPARED','NONE');"
        "CREATE TRIGGER fail_prepared_migration BEFORE UPDATE ON "
        "OCCUPANCY_COMMAND_INBOX WHEN OLD.status='PENDING_PREPARED' "
        "BEGIN SELECT RAISE(ABORT,'injected migration failure'); END;");

    bool migration_failed = false;
    try {
        database.migrateRuntimeSchema();
    } catch (const std::exception&) {
        migration_failed = true;
    }
    require(migration_failed,
            "injected runtime migration unexpectedly succeeded");
    require(!database.runtimeSchemaReady(),
            "failed migration left runtime schema marked ready");

    database::SessionTransitionStore store(database);
    auto actor = makeActor(store);
    require(!actor.start(),
            "slot actor opened ingress after runtime migration failure");
    require(actor.submit(hallCommand(
                "post-failed-migration", 2, "OCCUPIED", nowEpochMs()))
                .code == parking::SlotSubmitCode::Closed,
            "failed migration accepted external occupancy ingress");

    probe.execute("DROP TRIGGER fail_prepared_migration;");
    database.migrateRuntimeSchema();
    require(database.runtimeSchemaReady(),
            "successful migration recovery did not publish readiness");
    require(actor.start(), "actor did not start after migration recovery");
    require(actor.waitUntilIdle(3s),
            "recovered migration did not replay its prepared command");
    require(actor.stopAndDrain(2s),
            "migration-recovery actor did not stop");
}

}  // namespace

int main() {
    try {
        testAsyncAdmissionFailurePreservesSlotHead();
        testTransportQueueFullLeavesCameraStateUntouched();
        testSourceRedeliveryIgnoresOnlyLocalSchedulingTime();
        testConcurrentDurableCapacityHasOneWinner();
        testFullDurableInboxPreservesAndPacesDueDeadline();
        testHallCreateFailureRetriesWithoutConsumingSequence();
        testShutdownDoesNotClaimDueRetryWasDrained();
        testHallCloseFailureRetriesExactSession();
        testInvalidVacantTimestampDoesNotCloseState();
        testDurableHallConfirmationSurvivesRestart();
        testAdmittedExitPrecedesNewIntrusion();
        testIntrusionSupersedesScheduledExitBeforeAdmission();
        testCameraDeadlineStatesSurviveDatabaseReopen();
        testSameSlotOrdinalOccupiedVacantOccupied();
        testCommittedEffectReplaysAfterRestart();
        testEffectFailureDoesNotBlockAuthoritativeVacant();
        testCameraObjectOverlapAndLegacyTakeover();
        testQueuedIntrusionWinsBeforeDeadlineLinearization();
        testOverdueHallConfirmationPrecedesVacant();
        testHybridOrUsesOneSessionAndSourceAwareExit();
        testHybridSourceStateSurvivesDatabaseReopen();
        testInitializeMigratesOldDatabaseAndIsIdempotent();
        testMigrationFailureKeepsActorIngressClosed();
        std::cout << "[PASS] slot transition actor SQLite integration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] slot transition actor SQLite integration: "
                  << error.what() << '\n';
        return 1;
    }
}

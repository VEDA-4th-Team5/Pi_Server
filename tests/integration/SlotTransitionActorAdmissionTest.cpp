#include "database/EventDatabase.hpp"
#include "database/SessionTransitionStore.hpp"
#include "parking/SlotTransitionActor.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
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
            // Async admission can still own the SQLite write transaction.
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
               ("slot-transition-admission-" + unique + ".db");
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
    const std::string& command_id, const std::uint64_t sequence,
    const std::string& state, const std::int64_t occurred_epoch_ms,
    const std::int64_t due_epoch_ms = 0) {
    parking::SlotTransitionCommand command;
    command.commandId = command_id;
    command.kind = parking::SlotCommandKind::HallObservation;
    command.slotId = "P01";
    command.sensorId = "hall-p01";
    command.sourceIdentity = "hall-p01:LEGACY_V1::" +
                             std::to_string(sequence);
    command.sourceSequence = sequence;
    command.occurredAt = "2026-08-11T00:00:" +
                         std::to_string(sequence) + "Z";
    command.payloadJson = nlohmann::json{
        {"state", state},
        {"source_protocol", "LEGACY_V1"},
        {"transport", "UART"},
        {"occurred_at_epoch_ms", occurred_epoch_ms}}.dump();
    command.dueAtEpochMs = due_epoch_ms;
    return command;
}

parking::SlotTransitionCommand cameraCommand(
    const std::string& command_id, const std::string& action,
    const std::string& object_id, const std::int64_t occurred_epoch_ms,
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
        require(probe.text(
                    "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='hall-confirm-restart';") == "APPLIED",
                "restarted Hall confirmation did not become terminal");
        require(actor.stopAndDrain(2s),
                "confirmation restart actor did not stop cleanly");
    }
}

}  // namespace

int main() {
    try {
        testAsyncAdmissionFailurePreservesSlotHead();
        testTransportQueueFullLeavesCameraStateUntouched();
        testSourceRedeliveryIgnoresOnlyLocalSchedulingTime();
        testHallCloseFailureRetriesExactSession();
        testInvalidVacantTimestampDoesNotCloseState();
        testDurableHallConfirmationSurvivesRestart();
        std::cout << "Slot transition admission integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

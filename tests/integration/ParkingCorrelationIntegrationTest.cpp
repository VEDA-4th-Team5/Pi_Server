#include "database/EventDatabase.hpp"
#include "database/SessionTransitionStore.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "parking/SlotTransitionActor.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::int64_t systemNowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        static std::atomic<unsigned long long> ordinal{};
        const auto unique = std::to_string(systemNowEpochMs()) + "-" +
                            std::to_string(++ordinal);
        path = std::filesystem::temp_directory_path() /
               ("parking-correlation-" + unique + ".db");
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

    std::optional<std::int64_t> optionalInteger(
        const std::string& sql) const {
        sqlite3_stmt* statement{};
        prepare(sql, &statement);
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
        const auto value = optionalInteger(sql);
        if (!value) throw std::runtime_error("SQLite probe returned no row");
        return *value;
    }

    std::string text(const std::string& sql) const {
        sqlite3_stmt* statement{};
        prepare(sql, &statement);
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
    void prepare(const std::string& sql, sqlite3_stmt** statement) const {
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, statement,
                              nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite probe prepare failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

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
    const std::string& slot_id,
    const std::string& sensor_id,
    const std::uint64_t sequence,
    const std::string& state,
    const std::int64_t occurred_epoch_ms) {
    parking::SlotTransitionCommand command;
    command.commandId = command_id;
    command.kind = parking::SlotCommandKind::HallObservation;
    command.slotId = slot_id;
    command.sensorId = sensor_id;
    command.sourceIdentity = sensor_id + ":" + std::to_string(sequence);
    command.sourceSequence = sequence;
    command.occurredAt = state == "OCCUPIED"
        ? "2026-08-12T00:00:00Z"
        : "2026-08-12T00:00:01Z";
    command.payloadJson = nlohmann::json{
        {"state", state},
        {"transport", "UART"},
        {"occurred_at_epoch_ms", occurred_epoch_ms}}.dump();
    command.dueAtEpochMs = 0;
    return command;
}

parking::SlotTransitionCommand correlationCommand(
    const std::string& command_id,
    const std::string& slot_id,
    const std::string& sensor_id,
    const std::string& correlation_id,
    const std::string& camera_id,
    const std::string& channel_id,
    const std::string& video_source_token,
    const std::string& rule_name,
    const std::string& object_id,
    const std::int64_t occurred_epoch_ms,
    const std::int64_t expires_at_epoch_ms) {
    const std::string area_key = camera_id + "|" + video_source_token + "|" +
                                 rule_name;
    parking::SlotTransitionCommand command;
    command.commandId = command_id;
    command.kind = parking::SlotCommandKind::CameraObservation;
    command.slotId = slot_id;
    command.sensorId = sensor_id;
    command.sourceIdentity = "mqtt:" + correlation_id;
    command.occurredAt = "2026-08-12T00:00:00Z";
    command.payloadJson = nlohmann::json{
        {"action", "INTRUSION"},
        {"authoritative_exit", false},
        {"occupancy_authority", false},
        {"area_key", area_key},
        {"configured_areas", nlohmann::json::array({area_key})},
        {"camera_id", camera_id},
        {"channel_id", channel_id},
        {"video_source_token", video_source_token},
        {"rule_name", rule_name},
        {"object_id", object_id},
        {"correlation_id", correlation_id},
        {"correlation_expires_at_epoch_ms", expires_at_epoch_ms},
        {"transport", "camera-mqtt"},
        {"occurred_at_epoch_ms", occurred_epoch_ms},
        {"timestamp_authority", "CAMERA_UTC"},
        {"deadline_due_at_epoch_ms", 0}}.dump();
    command.dueAtEpochMs = 0;
    return command;
}

class TestContext {
public:
    TestContext()
        : database(temporary.path),
          probe(temporary.path),
          store(database),
          actor(store, actorConfig(),
                [](const parking::CommittedOccupancyTransition&) {
                    return true;
                }) {
        initialize(database);
        require(actor.start(), "slot transition actor did not start");
    }

    void submitAndSettle(const parking::SlotTransitionCommand& command,
                         const std::string& context) {
        const auto result = actor.submit(command);
        require(result.accepted(), context + " submit failed: " +
                                      result.message);
        require(actor.waitUntilIdle(3s), context + " did not settle");
    }

    void stop() {
        require(actor.stopAndDrain(2s),
                "slot transition actor did not stop cleanly");
    }

    TemporaryDatabase temporary;
    database::EventDatabase database;
    SqliteProbe probe;
    database::SessionTransitionStore store;
    parking::SlotTransitionActor actor;
};

void testHallCommitThenExactCorrelationResolvesUnique() {
    TestContext context;
    const auto base = systemNowEpochMs();
    context.submitAndSettle(
        hallCommand("case1-hall-occ", "P01", "hall-p01", 1,
                    "OCCUPIED", base),
        "case1 Hall OCCUPIED");

    const auto session_id = context.probe.integer(
        "SELECT session_id FROM PARKING_SESSION WHERE "
        "entry_command_id='case1-hall-occ';");
    const auto attempt_id = context.probe.text(
        "SELECT occupancy_attempt_id FROM PARKING_SESSION WHERE "
        "entry_command_id='case1-hall-occ';");
    require(!attempt_id.empty(), "case1 Hall session has no attempt identity");

    context.submitAndSettle(
        correlationCommand(
            "case1-camera", "P01", "hall-p01", "corr-case1", "cam01",
            "CH1", "vs-0", "name1", "object-case1", base + 10,
            base + 8000),
        "case1 CAMERA INTRUSION");

    require(context.probe.integer(
                "SELECT COUNT(*) FROM PARKING_CORRELATION_BINDING b JOIN "
                "PARKING_SESSION s ON s.session_id=b.session_id WHERE "
                "b.correlation_id='corr-case1' AND b.state='COMMITTED' AND "
                "b.slot_id='P01' AND b.occupancy_attempt_id="
                "s.occupancy_attempt_id AND s.status='ACTIVE' AND "
                "s.exit_time IS NULL;") == 1,
            "case1 correlation was not bound to the exact active attempt");

    std::int64_t logical_now = base + 20;
    parking::ParkingTriggerCoordinator coordinator(
        context.database, 8000, [&logical_now] { return logical_now; });
    const auto match = coordinator.resolve("cam01", "CH1", "object-case1");
    require(match.kind == parking::ParkingCorrelationMatchKind::Unique &&
                match.lease.has_value(),
            "case1 exact committed correlation did not resolve Unique");
    require(match.candidateCorrelationIds ==
                std::vector<std::string>{"corr-case1"},
            "case1 Unique diagnostics did not retain the exact correlation");
    require(match.lease->sessionId == session_id &&
                match.lease->occupancyAttemptId == attempt_id &&
                match.lease->slotId == "P01",
            "case1 lease does not identify the committed Hall session");
    context.stop();
}

void testFailedCorrelationCannotBindToLaterSession() {
    TestContext context;
    const auto base = systemNowEpochMs();
    context.submitAndSettle(
        correlationCommand(
            "case2-camera-a", "P01", "hall-p01", "corr-case2-a",
            "cam01", "CH1", "vs-0", "name1", "object-a", base,
            base + 8000),
        "case2 CAMERA A without Hall session");

    require(context.probe.text(
                "SELECT status FROM OCCUPANCY_COMMAND_INBOX WHERE "
                "command_id='case2-camera-a';") == "APPLIED" &&
                context.probe.text(
                    "SELECT result_code FROM OCCUPANCY_COMMAND_INBOX WHERE "
                    "command_id='case2-camera-a';") == "NO_CHANGE",
            "case2 failed CAMERA A did not become a terminal no-change fact");
    require(context.probe.integer(
                "SELECT COUNT(*) FROM PARKING_CORRELATION_BINDING WHERE "
                "correlation_id='corr-case2-a';") == 0,
            "case2 failed CAMERA A unexpectedly created a binding");

    context.submitAndSettle(
        hallCommand("case2-hall-occ", "P01", "hall-p01", 1,
                    "OCCUPIED", base + 100),
        "case2 later Hall OCCUPIED");
    context.submitAndSettle(
        correlationCommand(
            "case2-camera-b", "P01", "hall-p01", "corr-case2-b",
            "cam01", "CH1", "vs-0", "name1", "object-b", base + 110,
            base + 8110),
        "case2 CAMERA B");

    std::int64_t logical_now = base + 120;
    parking::ParkingTriggerCoordinator coordinator(
        context.database, 8000, [&logical_now] { return logical_now; });
    const auto failed_a = coordinator.resolve("cam01", "CH1", "object-a");
    const auto committed_b = coordinator.resolve("cam01", "CH1", "object-b");
    require(failed_a.kind == parking::ParkingCorrelationMatchKind::Unmatched &&
                !failed_a.lease.has_value() &&
                failed_a.candidateCorrelationIds.empty(),
            "case2 terminal CAMERA A leaked into the later session");
    require(committed_b.kind == parking::ParkingCorrelationMatchKind::Unique &&
                committed_b.lease.has_value() &&
                committed_b.lease->correlationId == "corr-case2-b",
            "case2 CAMERA B did not bind independently to the later session");
    require(context.probe.integer(
                "SELECT COUNT(*) FROM PARKING_CORRELATION_BINDING WHERE "
                "correlation_id='corr-case2-a';") == 0,
            "case2 later session retroactively bound failed CAMERA A");
    context.stop();
}

void testSharedChannelObjectIsAmbiguousAndSorted() {
    TestContext context;
    const auto base = systemNowEpochMs();
    context.submitAndSettle(
        hallCommand("case3-hall-p01", "P01", "hall-p01", 1,
                    "OCCUPIED", base),
        "case3 P01 Hall OCCUPIED");
    context.submitAndSettle(
        hallCommand("case3-hall-p02", "P02", "hall-p02", 1,
                    "OCCUPIED", base + 1),
        "case3 P02 Hall OCCUPIED");
    context.submitAndSettle(
        correlationCommand(
            "case3-camera-z", "P01", "hall-p01", "corr-z", "cam01",
            "CH1", "vs-0", "name1", "shared-object", base + 10,
            base + 8010),
        "case3 P01 CAMERA");
    context.submitAndSettle(
        correlationCommand(
            "case3-camera-a", "P02", "hall-p02", "corr-a", "cam01",
            "CH1", "vs-0", "name1", "shared-object", base + 11,
            base + 8011),
        "case3 P02 CAMERA");

    require(context.probe.integer(
                "SELECT COUNT(*) FROM PARKING_CORRELATION_BINDING WHERE "
                "state='COMMITTED' AND camera_id='cam01' AND "
                "channel_id='CH1' AND object_id='shared-object';") == 2,
            "case3 did not create both exact committed candidates");
    std::int64_t logical_now = base + 20;
    parking::ParkingTriggerCoordinator coordinator(
        context.database, 8000, [&logical_now] { return logical_now; });
    const auto match = coordinator.resolve("cam01", "CH1", "shared-object");
    require(match.kind == parking::ParkingCorrelationMatchKind::Ambiguous &&
                !match.lease.has_value(),
            "case3 shared channel/ObjectId did not fail closed as Ambiguous");
    require(match.candidateCorrelationIds ==
                std::vector<std::string>{"corr-a", "corr-z"},
            "case3 Ambiguous candidates are not deterministically sorted");
    context.stop();
}

void testAttachIsIdempotentAndLatePlateIsRejected() {
    TestContext context;
    const auto base = systemNowEpochMs();
    context.submitAndSettle(
        hallCommand("case4-hall-occ", "P01", "hall-p01", 1,
                    "OCCUPIED", base),
        "case4 Hall OCCUPIED");
    context.submitAndSettle(
        correlationCommand(
            "case4-camera", "P01", "hall-p01", "corr-case4", "cam01",
            "CH1", "vs-0", "name1", "object-case4", base + 10,
            base + 10000),
        "case4 CAMERA INTRUSION");

    std::int64_t logical_now = base + 20;
    parking::ParkingTriggerCoordinator coordinator(
        context.database, 10000, [&logical_now] { return logical_now; });
    const auto match = coordinator.resolve("cam01", "CH1", "object-case4");
    require(match.kind == parking::ParkingCorrelationMatchKind::Unique &&
                match.lease.has_value(),
            "case4 active exact lease did not resolve");

    const auto attached = coordinator.attachBestShotIfActive(
        *match.lease, parking::BestShotEvidenceKind::Vehicle,
        "vehicle-ref", "/tmp/case4-vehicle.jpg");
    require(attached.code == parking::BestShotAttachCode::Attached &&
                attached.imageId >= 0,
            "case4 active exact Vehicle evidence was not attached");
    const auto duplicate = coordinator.attachBestShotIfActive(
        *match.lease, parking::BestShotEvidenceKind::Vehicle,
        "vehicle-ref", "/tmp/case4-vehicle.jpg");
    require(duplicate.code == parking::BestShotAttachCode::AlreadyAttached &&
                duplicate.imageId == attached.imageId,
            "case4 identical Vehicle evidence was not idempotent");
    require(context.probe.integer(
                "SELECT COUNT(*) FROM IMAGE_LOG;") == 1,
            "case4 idempotent attach created an extra IMAGE_LOG row");

    context.submitAndSettle(
        hallCommand("case4-hall-vac", "P01", "hall-p01", 2,
                    "VACANT", base + 1000),
        "case4 Hall VACANT");
    require(context.probe.text(
                "SELECT state FROM PARKING_CORRELATION_BINDING WHERE "
                "correlation_id='corr-case4';") == "ENDED",
            "case4 exact Hall close did not end the correlation binding");
    require(context.probe.text(
                "SELECT status FROM PARKING_SESSION WHERE "
                "entry_command_id='case4-hall-occ';") == "ENDED",
            "case4 exact Hall close did not end the bound session");

    logical_now = base + 1100;
    const auto late_plate = coordinator.attachBestShotIfActive(
        *match.lease, parking::BestShotEvidenceKind::Plate,
        "late-plate-ref", "/tmp/case4-late-plate.jpg", "12A3456");
    require(late_plate.code == parking::BestShotAttachCode::Inactive,
            "case4 late Plate was not rejected after exact Hall close");
    require(context.probe.integer(
                "SELECT COUNT(*) FROM IMAGE_LOG;") == 1,
            "case4 late Plate changed IMAGE_LOG after Hall VACANT");
    context.stop();
}

void testExpiredLeaseRejectsEvidence() {
    TestContext context;
    const auto base = systemNowEpochMs();
    context.submitAndSettle(
        hallCommand("case5-hall-occ", "P01", "hall-p01", 1,
                    "OCCUPIED", base),
        "case5 Hall OCCUPIED");
    context.submitAndSettle(
        correlationCommand(
            "case5-camera", "P01", "hall-p01", "corr-case5", "cam01",
            "CH1", "vs-0", "name1", "object-case5", base + 10,
            base + 100),
        "case5 CAMERA INTRUSION");

    std::int64_t logical_now = base + 50;
    parking::ParkingTriggerCoordinator coordinator(
        context.database, 100, [&logical_now] { return logical_now; });
    const auto match = coordinator.resolve("cam01", "CH1", "object-case5");
    require(match.kind == parking::ParkingCorrelationMatchKind::Unique &&
                match.lease.has_value(),
            "case5 pre-expiry lease did not resolve");

    logical_now = base + 101;
    const auto expired = coordinator.attachBestShotIfActive(
        *match.lease, parking::BestShotEvidenceKind::Vehicle,
        "expired-ref", "/tmp/case5-expired.jpg");
    require(expired.code == parking::BestShotAttachCode::Expired,
            "case5 stale correlation lease did not fail closed as Expired");
    require(context.probe.integer(
                "SELECT COUNT(*) FROM IMAGE_LOG;") == 0,
            "case5 expired lease inserted IMAGE_LOG evidence");
    context.stop();
}

}  // namespace

int main() {
    try {
        testHallCommitThenExactCorrelationResolvesUnique();
        testFailedCorrelationCannotBindToLaterSession();
        testSharedChannelObjectIsAmbiguousAndSorted();
        testAttachIsIdempotentAndLatePlateIsRejected();
        testExpiredLeaseRejectsEvidence();
        std::cout << "[PASS] parking correlation SQLite integration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] parking correlation SQLite integration: "
                  << error.what() << '\n';
        return 1;
    }
}

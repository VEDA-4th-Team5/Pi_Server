#include "database/EventDatabase.hpp"

#include "sensor/SensorSequencePolicy.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace database {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const std::string_view sql)
        : database_(database) {
        if (sqlite3_prepare_v2(database_, sql.data(),
                              static_cast<int>(sql.size()), &statement_,
                              nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite occupancy prepare failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

    void text(const int index, const std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(),
                             static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error("SQLite occupancy text bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    void integer(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("SQLite occupancy integer bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    void null(const int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error("SQLite occupancy NULL bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string columnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string{}
        : std::string{reinterpret_cast<const char*>(value)};
}

std::optional<std::int64_t> optionalInt64(sqlite3_stmt* statement,
                                          const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL)
        return std::nullopt;
    return sqlite3_column_int64(statement, column);
}

void done(sqlite3* database, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error("SQLite occupancy statement failed: " +
                                 std::string(sqlite3_errmsg(database)));
    }
}

std::optional<std::uint64_t> parseUnsigned(const std::string& value) {
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed{};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::string resultCodeName(const parking::CommittedOccupancyCode code) {
    switch (code) {
    case parking::CommittedOccupancyCode::SessionStarted:
        return "SESSION_STARTED";
    case parking::CommittedOccupancyCode::SessionEnded:
        return "SESSION_ENDED";
    case parking::CommittedOccupancyCode::NoChange:
        return "NO_CHANGE";
    case parking::CommittedOccupancyCode::ExitScheduled:
        return "EXIT_SCHEDULED";
    case parking::CommittedOccupancyCode::AlreadyApplied:
        return "ALREADY_APPLIED";
    case parking::CommittedOccupancyCode::RejectedInvalidTimestamp:
        return "REJECTED_INVALID_TIMESTAMP";
    case parking::CommittedOccupancyCode::StaleGeneration:
        return "STALE_GENERATION";
    case parking::CommittedOccupancyCode::RetryableFailure:
        return "RETRYABLE_FAILURE";
    }
    return "RETRYABLE_FAILURE";
}

parking::CommittedOccupancyCode resultCodeFromName(
    const std::string& value) {
    if (value == "SESSION_STARTED")
        return parking::CommittedOccupancyCode::SessionStarted;
    if (value == "SESSION_ENDED")
        return parking::CommittedOccupancyCode::SessionEnded;
    if (value == "NO_CHANGE")
        return parking::CommittedOccupancyCode::NoChange;
    if (value == "EXIT_SCHEDULED")
        return parking::CommittedOccupancyCode::ExitScheduled;
    if (value == "ALREADY_APPLIED")
        return parking::CommittedOccupancyCode::AlreadyApplied;
    if (value == "REJECTED_INVALID_TIMESTAMP")
        return parking::CommittedOccupancyCode::RejectedInvalidTimestamp;
    if (value == "STALE_GENERATION")
        return parking::CommittedOccupancyCode::StaleGeneration;
    return parking::CommittedOccupancyCode::RetryableFailure;
}

parking::DurableSlotCommand readCommand(sqlite3_stmt* statement) {
    parking::DurableSlotCommand result;
    result.command.commandId = columnText(statement, 0);
    result.command.kind = parking::slotCommandKindFromString(
        columnText(statement, 1)).value_or(
            parking::SlotCommandKind::HallObservation);
    result.command.slotId = columnText(statement, 2);
    result.command.sensorId = columnText(statement, 3);
    result.command.sourceIdentity = columnText(statement, 4);
    if (sqlite3_column_type(statement, 5) != SQLITE_NULL)
        result.command.sourceSequence = parseUnsigned(columnText(statement, 5));
    result.command.occurredAt = columnText(statement, 6);
    result.command.payloadJson = columnText(statement, 7);
    result.command.dueAtEpochMs = sqlite3_column_int64(statement, 8);
    result.admissionOrdinal = sqlite3_column_int64(statement, 9);
    result.status = parking::slotCommandStatusFromString(
        columnText(statement, 10)).value_or(
            parking::SlotCommandStatus::PendingUnprepared);
    result.occupancyAttemptId = columnText(statement, 11);
    result.correlationId = columnText(statement, 12);
    result.observationGeneration = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, sqlite3_column_int64(statement, 13)));
    result.deadlineId = columnText(statement, 14);
    result.expectedSessionId = optionalInt64(statement, 15);
    result.attemptCount = sqlite3_column_int(statement, 16);
    result.nextAttemptAtEpochMs = sqlite3_column_int64(statement, 17);
    result.lastError = columnText(statement, 18);
    return result;
}

constexpr std::string_view kCommandColumns =
    "command_id,source_kind,slot_id,sensor_id,source_identity,"
    "source_sequence,occurred_at,payload_json,due_at_epoch_ms,"
    "admission_ordinal,status,occupancy_attempt_id,correlation_id,"
    "observation_generation,deadline_id,expected_session_id,attempt_count,"
    "next_attempt_at_epoch_ms,last_error";

bool terminalStatus(const std::string& status) {
    return status == "APPLIED" || status == "REJECTED_INVALID";
}

sensor::SensorSequenceFact hallSequenceFact(
    const parking::SlotTransitionCommand& command,
    const nlohmann::json& payload) {
    sensor::SensorSequenceFact fact;
    const std::string encoded = payload.value(
        "source_protocol", std::string{"LEGACY_V1"});
    const auto protocol = sensor::sensorProtocolVersionFromString(encoded);
    if (!protocol) {
        throw std::invalid_argument("unsupported Hall source protocol");
    }
    fact.protocolVersion = *protocol;
    fact.sequence = command.sourceSequence;
    if (payload.contains("source_boot_id") &&
        payload["source_boot_id"].is_string()) {
        fact.bootId = payload["source_boot_id"].get<std::string>();
    }
    return fact;
}

sensor::SensorSequenceState loadHallSequenceState(
    sqlite3* database,
    const std::string& sensor_id) {
    sensor::SensorSequenceState state;
    Statement cursor(database,
        "SELECT protocol_mode,active_boot_id,last_sequence FROM "
        "OCCUPANCY_SENSOR_SEQUENCE_STATE WHERE sensor_id=? LIMIT 1;");
    cursor.text(1, sensor_id);
    const int step = sqlite3_step(cursor.get());
    if (step == SQLITE_ROW) {
        const std::string mode = columnText(cursor.get(), 0);
        if (mode == "LEGACY") {
            state.mode = sensor::SensorSequenceMode::Legacy;
        } else if (mode == "VERSIONED") {
            state.mode = sensor::SensorSequenceMode::Versioned;
        } else {
            throw std::runtime_error("invalid Hall sequence protocol mode");
        }
        const std::string active_boot = columnText(cursor.get(), 1);
        if (!active_boot.empty()) state.activeBootId = active_boot;
        state.lastSequence = parseUnsigned(columnText(cursor.get(), 2));
    } else if (step != SQLITE_DONE) {
        throw std::runtime_error("Hall sequence state lookup failed");
    }

    Statement retired(database,
        "SELECT boot_id FROM SENSOR_RETIRED_BOOT_ID WHERE "
        "source_kind='HALL' AND sensor_id=?;");
    retired.text(1, sensor_id);
    for (;;) {
        const int retired_step = sqlite3_step(retired.get());
        if (retired_step == SQLITE_DONE) break;
        if (retired_step != SQLITE_ROW) {
            throw std::runtime_error("Hall retired boot lookup failed");
        }
        state.retiredBootIds.insert(columnText(retired.get(), 0));
    }
    return state;
}

}  // namespace

parking::SlotAdmissionResult EventDatabase::admitSlotTransitionCommand(
    const parking::SlotTransitionCommand& command,
    const std::size_t pending_capacity) {
    parking::SlotAdmissionResult result;
    result.commandId = command.commandId;
    if (command.commandId.empty() || command.slotId.empty() ||
        command.sourceIdentity.empty() || command.occurredAt.empty() ||
        command.payloadJson.empty() || pending_capacity == 0) {
        result.code = parking::SlotAdmissionCode::Conflict;
        result.message = "invalid slot command fields";
        return result;
    }

    std::string command_correlation_id;
    if (command.kind == parking::SlotCommandKind::CameraObservation) {
        try {
            const auto payload = nlohmann::json::parse(command.payloadJson);
            command_correlation_id = payload.value("correlation_id", "");
        } catch (const std::exception& error) {
            result.code = parking::SlotAdmissionCode::Conflict;
            result.message = std::string("invalid camera command JSON: ") +
                             error.what();
            return result;
        }
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        result.code = parking::SlotAdmissionCode::RetryableFailure;
        result.message = "occupancy database is closed";
        return result;
    }

    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement existing(db_,
            "SELECT command_id,slot_id,sensor_id,source_identity,"
            "COALESCE(source_sequence,''),occurred_at,payload_json,"
            "due_at_epoch_ms,status FROM OCCUPANCY_COMMAND_INBOX "
            "WHERE source_kind=? AND source_identity=? LIMIT 1;");
        existing.text(1, parking::toString(command.kind));
        existing.text(2, command.sourceIdentity);
        const int existing_step = sqlite3_step(existing.get());
        if (existing_step == SQLITE_ROW) {
            parking::SlotTransitionCommand stored;
            stored.commandId = columnText(existing.get(), 0);
            stored.kind = command.kind;
            stored.slotId = columnText(existing.get(), 1);
            stored.sensorId = columnText(existing.get(), 2);
            stored.sourceIdentity = columnText(existing.get(), 3);
            const std::string stored_sequence = columnText(existing.get(), 4);
            if (!stored_sequence.empty())
                stored.sourceSequence = parseUnsigned(stored_sequence);
            stored.occurredAt = columnText(existing.get(), 5);
            stored.payloadJson = columnText(existing.get(), 6);
            stored.dueAtEpochMs = sqlite3_column_int64(existing.get(), 7);
            const bool matches = parking::sameSlotSourceFact(stored, command);
            result.commandId = columnText(existing.get(), 0);
            result.code = matches
                ? (terminalStatus(columnText(existing.get(), 8))
                    ? parking::SlotAdmissionCode::AlreadyTerminal
                    : parking::SlotAdmissionCode::Existing)
                : parking::SlotAdmissionCode::Conflict;
            result.message = matches ? "command already admitted"
                                     : "source identity payload conflict";
            executeSqlUnlocked("COMMIT;");
            return result;
        }
        if (existing_step != SQLITE_DONE)
            throw std::runtime_error("occupancy identity lookup failed");

        if (command.kind == parking::SlotCommandKind::HallObservation) {
            if (command.sensorId.empty()) {
                executeSqlUnlocked("ROLLBACK;");
                result.code = parking::SlotAdmissionCode::Conflict;
                result.message = "Hall command sensor ID is empty";
                return result;
            }
            try {
                const auto hall_payload =
                    nlohmann::json::parse(command.payloadJson);
                const auto fact = hallSequenceFact(command, hall_payload);
                const auto cursor = loadHallSequenceState(db_, command.sensorId);
                const auto decision =
                    sensor::evaluateSensorSequence(cursor, fact);
                if (!decision.accepted()) {
                    executeSqlUnlocked("COMMIT;");
                    result.code = decision.code ==
                            sensor::SensorSequenceDecisionCode::Invalid
                        ? parking::SlotAdmissionCode::Conflict
                        : parking::SlotAdmissionCode::RejectedStale;
                    result.message = decision.reason;
                    return result;
                }
            } catch (const std::invalid_argument& error) {
                executeSqlUnlocked("ROLLBACK;");
                result.code = parking::SlotAdmissionCode::Conflict;
                result.message = error.what();
                return result;
            }
        }

        // Hall OCCUPIED confirmation is itself durable. Repeated OCCUPIED
        // frames share the first not-yet-due candidate instead of filling the
        // inbox. A newer VACANT atomically cancels that candidate before its
        // own admission, so a restart or queue-full condition cannot resurrect
        // a pre-confirmation occupancy.
        if (command.kind == parking::SlotCommandKind::HallObservation) {
            const auto payload = nlohmann::json::parse(command.payloadJson);
            const std::string state = payload.value("state", "");
            if (state != "OCCUPIED" && state != "VACANT") {
                executeSqlUnlocked("ROLLBACK;");
                result.code = parking::SlotAdmissionCode::Conflict;
                result.message = "hall command state is invalid";
                return result;
            }
            Statement candidate(db_,
                "SELECT command_id,due_at_epoch_ms FROM "
                "OCCUPANCY_COMMAND_INBOX WHERE "
                "slot_id=? AND source_kind='HALL_OBSERVATION' AND "
                "status='PENDING_UNPREPARED' AND "
                "json_extract(payload_json,'$.state')='OCCUPIED' AND "
                "due_at_epoch_ms>0 ORDER BY admission_ordinal LIMIT 1;");
            candidate.text(1, command.slotId);
            const int candidate_step = sqlite3_step(candidate.get());
            if (candidate_step == SQLITE_ROW) {
                const std::string candidate_id = columnText(candidate.get(), 0);
                const auto candidate_due =
                    sqlite3_column_int64(candidate.get(), 1);
                if (state == "OCCUPIED") {
                    executeSqlUnlocked("COMMIT;");
                    result.commandId = candidate_id;
                    result.code = parking::SlotAdmissionCode::Existing;
                    result.message = "hall confirmation already durable";
                    return result;
                }
                const auto vacant_observed = payload.value<std::int64_t>(
                    "occurred_at_epoch_ms", command.dueAtEpochMs);
                if (vacant_observed < candidate_due) {
                    Statement cancel(db_,
                        "UPDATE OCCUPANCY_COMMAND_INBOX SET status='APPLIED',"
                        "result_code='NO_CHANGE',last_error='canceled by "
                        "VACANT before confirmation',"
                        "updated_at=CURRENT_TIMESTAMP WHERE command_id=? "
                        "AND status='PENDING_UNPREPARED';");
                    cancel.text(1, candidate_id);
                    done(db_, cancel.get());
                }
            }
            if (candidate_step != SQLITE_ROW && candidate_step != SQLITE_DONE)
                throw std::runtime_error(
                    "Hall confirmation candidate lookup failed");
        }

        Statement count(db_,
            "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX "
            "WHERE status='PENDING_UNPREPARED';");
        if (sqlite3_step(count.get()) != SQLITE_ROW)
            throw std::runtime_error("occupancy pending count failed");
        const auto pending = sqlite3_column_int64(count.get(), 0);
        if (pending >= static_cast<std::int64_t>(pending_capacity)) {
            // Admission has not happened. Roll back any reducer-side
            // preparation (notably Hall confirmation cancellation) so a
            // queue-full return is a strict zero-domain-mutation boundary.
            executeSqlUnlocked("ROLLBACK;");
            result.code = parking::SlotAdmissionCode::CapacityFull;
            result.message = "durable occupancy inbox is full";
            return result;
        }

        Statement ordinal(db_,
            "SELECT COALESCE(MAX(admission_ordinal),0)+1 "
            "FROM OCCUPANCY_COMMAND_INBOX;");
        if (sqlite3_step(ordinal.get()) != SQLITE_ROW)
            throw std::runtime_error("occupancy ordinal allocation failed");
        const auto next_ordinal = sqlite3_column_int64(ordinal.get(), 0);

        Statement insert(db_,
            "INSERT INTO OCCUPANCY_COMMAND_INBOX("
            "command_id,slot_id,source_kind,sensor_id,source_identity,"
            "source_sequence,occurred_at,payload_json,due_at_epoch_ms,"
            "admission_ordinal,status,correlation_id,"
            "next_attempt_at_epoch_ms) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,'PENDING_UNPREPARED',?,0);");
        insert.text(1, command.commandId);
        insert.text(2, command.slotId);
        insert.text(3, parking::toString(command.kind));
        insert.text(4, command.sensorId);
        insert.text(5, command.sourceIdentity);
        if (command.sourceSequence)
            insert.text(6, std::to_string(*command.sourceSequence));
        else
            insert.null(6);
        insert.text(7, command.occurredAt);
        insert.text(8, command.payloadJson);
        insert.integer(9, command.dueAtEpochMs);
        insert.integer(10, next_ordinal);
        insert.text(11, command_correlation_id);
        done(db_, insert.get());
        executeSqlUnlocked("COMMIT;");
        result.code = parking::SlotAdmissionCode::Admitted;
        result.message = "command durably admitted";
        return result;
    } catch (const std::exception& error) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        result.code = parking::SlotAdmissionCode::RetryableFailure;
        result.message = error.what();
        return result;
    }
}

std::size_t EventDatabase::admitDueSlotDeadlines(
    const std::int64_t now_epoch_ms,
    const std::size_t pending_capacity,
    const std::vector<std::string>& blocked_slots) {
    if (pending_capacity == 0) return 0;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return 0;
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement count(db_,
            "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX "
            "WHERE status='PENDING_UNPREPARED';");
        if (sqlite3_step(count.get()) != SQLITE_ROW)
            throw std::runtime_error("occupancy pending count failed");
        std::size_t available = pending_capacity;
        const auto pending = sqlite3_column_int64(count.get(), 0);
        if (pending >= static_cast<std::int64_t>(pending_capacity)) {
            executeSqlUnlocked("COMMIT;");
            return 0;
        }
        available -= static_cast<std::size_t>(pending);

        struct Due {
            std::string deadlineId;
            std::string slotId;
            std::string attemptId;
            std::int64_t sessionId{};
            std::int64_t generation{};
            std::int64_t dueAt{};
            std::string occupancyPolicy;
        };
        std::vector<Due> due;
        Statement select(db_,
            "SELECT deadline_id,slot_id,occupancy_attempt_id,"
            "expected_session_id,observation_generation,due_at_epoch_ms,"
            "occupancy_policy "
            "FROM OCCUPANCY_EXIT_DEADLINE WHERE state='SCHEDULED' "
            "AND due_at_epoch_ms<=? ORDER BY due_at_epoch_ms,deadline_id "
            ";");
        select.integer(1, now_epoch_ms);
        for (;;) {
            const int step = sqlite3_step(select.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW)
                throw std::runtime_error("due deadline lookup failed");
            const std::string slot_id = columnText(select.get(), 1);
            if (std::find(blocked_slots.begin(), blocked_slots.end(),
                          slot_id) != blocked_slots.end()) {
                continue;
            }
            due.push_back({columnText(select.get(), 0),
                           slot_id,
                           columnText(select.get(), 2),
                           sqlite3_column_int64(select.get(), 3),
                           sqlite3_column_int64(select.get(), 4),
                           sqlite3_column_int64(select.get(), 5),
                           columnText(select.get(), 6)});
            if (due.size() >= available) break;
        }

        Statement ordinal(db_,
            "SELECT COALESCE(MAX(admission_ordinal),0) "
            "FROM OCCUPANCY_COMMAND_INBOX;");
        if (sqlite3_step(ordinal.get()) != SQLITE_ROW)
            throw std::runtime_error("deadline ordinal lookup failed");
        std::int64_t next_ordinal = sqlite3_column_int64(ordinal.get(), 0);
        std::size_t admitted{};
        for (const auto& item : due) {
            const std::string command_id =
                "deadline-command:" + item.deadlineId;
            nlohmann::json payload{
                {"deadline_id", item.deadlineId},
                {"expected_session_id", item.sessionId},
                {"observation_generation", item.generation},
                {"occupancy_attempt_id", item.attemptId},
                {"occupancy_policy", item.occupancyPolicy}};
            Statement insert(db_,
                "INSERT OR IGNORE INTO OCCUPANCY_COMMAND_INBOX("
                "command_id,slot_id,source_kind,sensor_id,source_identity,"
                "occurred_at,payload_json,due_at_epoch_ms,admission_ordinal,"
                "status,occupancy_attempt_id,observation_generation,"
                "deadline_id,expected_session_id,next_attempt_at_epoch_ms) "
                "VALUES(?,?,'EXIT_DEADLINE','',?,"
                "strftime('%Y-%m-%dT%H:%M:%fZ',?/1000.0,'unixepoch'),"
                "?,?,?,?,?,?,?,?,0);");
            insert.text(1, command_id);
            insert.text(2, item.slotId);
            insert.text(3, item.deadlineId);
            insert.integer(4, item.dueAt);
            insert.text(5, payload.dump());
            insert.integer(6, item.dueAt);
            insert.integer(7, ++next_ordinal);
            insert.text(8, "PENDING_UNPREPARED");
            insert.text(9, item.attemptId);
            insert.integer(10, item.generation);
            insert.text(11, item.deadlineId);
            insert.integer(12, item.sessionId);
            done(db_, insert.get());

            Statement mark(db_,
                "UPDATE OCCUPANCY_EXIT_DEADLINE SET state='ADMITTED',"
                "admitted_command_id=?,updated_at=CURRENT_TIMESTAMP "
                "WHERE deadline_id=? AND state='SCHEDULED';");
            mark.text(1, command_id);
            mark.text(2, item.deadlineId);
            done(db_, mark.get());
            if (sqlite3_changes(db_) == 1) ++admitted;
        }
        executeSqlUnlocked("COMMIT;");
        return admitted;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<parking::DurableSlotCommand>
EventDatabase::listRunnableSlotTransitionCommands(
    const std::int64_t now_epoch_ms) const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return {};
    const std::string sql =
        "SELECT " + std::string(kCommandColumns) +
        " FROM OCCUPANCY_COMMAND_INBOX c "
        "WHERE c.status='PENDING_UNPREPARED' "
        "AND c.due_at_epoch_ms<=? AND c.next_attempt_at_epoch_ms<=? "
        "AND NOT EXISTS (SELECT 1 FROM OCCUPANCY_COMMAND_INBOX earlier "
        "WHERE earlier.slot_id=c.slot_id "
        "AND earlier.status='PENDING_UNPREPARED' "
        "AND earlier.admission_ordinal<c.admission_ordinal) "
        "ORDER BY c.admission_ordinal;";
    Statement statement(db_, sql);
    statement.integer(1, now_epoch_ms);
    statement.integer(2, now_epoch_ms);
    std::vector<parking::DurableSlotCommand> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            throw std::runtime_error("runnable occupancy lookup failed");
        result.push_back(readCommand(statement.get()));
    }
    return result;
}

bool EventDatabase::deferSlotTransitionCommand(
    const std::string& command_id,
    const std::int64_t next_attempt_at_epoch_ms,
    const std::string& error) noexcept {
    try {
        std::lock_guard lock(db_mutex_);
        if (!opened_ || db_ == nullptr || command_id.empty()) return false;
        Statement statement(db_,
            "UPDATE OCCUPANCY_COMMAND_INBOX SET attempt_count=attempt_count+1,"
            "next_attempt_at_epoch_ms=?,last_error=?,"
            "updated_at=CURRENT_TIMESTAMP WHERE command_id=? "
            "AND status='PENDING_UNPREPARED';");
        statement.integer(1, next_attempt_at_epoch_ms);
        statement.text(2, error);
        statement.text(3, command_id);
        done(db_, statement.get());
        return sqlite3_changes(db_) == 1;
    } catch (...) {
        return false;
    }
}

std::optional<std::int64_t>
EventDatabase::nextScheduledSlotDeadlineEpochMs() const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;
    Statement statement(db_,
        "SELECT MIN(due_at_epoch_ms) FROM OCCUPANCY_EXIT_DEADLINE "
        "WHERE state='SCHEDULED';");
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW)
        throw std::runtime_error("occupancy deadline lookup failed");
    if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL)
        return std::nullopt;
    return sqlite3_column_int64(statement.get(), 0);
}

std::size_t EventDatabase::pendingSlotTransitionCommandCount() const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return 0;
    Statement statement(db_,
        "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX "
        "WHERE status='PENDING_UNPREPARED';");
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error("occupancy pending count failed");
    return static_cast<std::size_t>(
        std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 0)));
}

parking::CommittedOccupancyTransition
EventDatabase::applySlotTransitionCommand(const std::string& command_id) {
    parking::CommittedOccupancyTransition outcome;
    outcome.commandId = command_id;
    if (command_id.empty()) {
        outcome.message = "empty occupancy command id";
        return outcome;
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        outcome.message = "occupancy database is closed";
        return outcome;
    }

    try {
        executeSqlUnlocked("BEGIN IMMEDIATE;");
        const std::string sql =
            "SELECT " + std::string(kCommandColumns) +
            ",result_code,result_session_id FROM OCCUPANCY_COMMAND_INBOX "
            "WHERE command_id=? LIMIT 1;";
        Statement select(db_, sql);
        select.text(1, command_id);
        const int step = sqlite3_step(select.get());
        if (step != SQLITE_ROW) {
            executeSqlUnlocked("COMMIT;");
            outcome.message = step == SQLITE_DONE
                ? "occupancy command does not exist"
                : "occupancy command lookup failed";
            return outcome;
        }
        parking::DurableSlotCommand durable = readCommand(select.get());
        const std::string stored_result = columnText(select.get(), 19);
        const auto stored_session = optionalInt64(select.get(), 20);
        outcome.sourceKind = durable.command.kind;
        outcome.slotId = durable.command.slotId;
        outcome.sensorId = durable.command.sensorId;
        outcome.occurredAt = durable.command.occurredAt;
        outcome.occupancyAttemptId = durable.occupancyAttemptId;
        outcome.correlationId = durable.correlationId;
        outcome.observationGeneration = durable.observationGeneration;
        if (!stored_result.empty())
            outcome.code = resultCodeFromName(stored_result);
        if (stored_session) outcome.sessionId = *stored_session;

        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(durable.command.payloadJson);
            outcome.sourceTransport = payload.value("transport", "unknown");
            outcome.occurredAtEpochMs = payload.value<std::int64_t>(
                "occurred_at_epoch_ms", durable.command.dueAtEpochMs);
        } catch (const std::exception& error) {
            Statement reject(db_,
                "UPDATE OCCUPANCY_COMMAND_INBOX SET status='REJECTED_INVALID',"
                "result_code='REJECTED_INVALID_TIMESTAMP',last_error=?,"
                "updated_at=CURRENT_TIMESTAMP WHERE command_id=?;");
            reject.text(1, std::string("invalid command JSON: ") + error.what());
            reject.text(2, command_id);
            done(db_, reject.get());
            executeSqlUnlocked("COMMIT;");
            outcome.code =
                parking::CommittedOccupancyCode::RejectedInvalidTimestamp;
            outcome.message = "invalid command payload";
            return outcome;
        }

        // Every production entrypoint runs migrateRuntimeSchema() before the
        // actor starts.  A remaining legacy prepared row is deliberately not
        // guessed here: exact committed rows are promoted by migration and
        // ambiguous rows are returned to PENDING_UNPREPARED for replay.
        if (durable.status == parking::SlotCommandStatus::PendingPrepared) {
            executeSqlUnlocked("COMMIT;");
            outcome.message = "committed transition awaiting effects";
            return outcome;
        }
        if (durable.status == parking::SlotCommandStatus::Applied ||
            durable.status == parking::SlotCommandStatus::RejectedInvalid) {
            executeSqlUnlocked("COMMIT;");
            if (stored_result.empty())
                outcome.code = parking::CommittedOccupancyCode::AlreadyApplied;
            outcome.message = "transition already terminal";
            return outcome;
        }

        const auto occurred_epoch_ms = payload.value<std::int64_t>(
            "occurred_at_epoch_ms", durable.command.dueAtEpochMs);
        const bool hybrid_or =
            payload.value("occupancy_policy", "") == "HYBRID_OR";

        struct ActiveSession {
            std::int64_t id{-1};
            std::string attemptId;
            std::int64_t entryEpochMs{};
            std::string entryTime;
            bool hallConfirmed{};
            bool ivaConfirmed{};
            bool hallOccupied{};
            bool ivaOccupied{};
        };
        const auto find_active = [&]() -> std::optional<ActiveSession> {
            Statement active(db_,
                "SELECT session_id,COALESCE(occupancy_attempt_id,''),"
                "COALESCE(entry_time_epoch_ms,CAST((julianday(entry_time)-"
                "2440587.5)*86400000 AS INTEGER)),entry_time,"
                "hall_confirmed,iva_confirmed,hall_occupied,iva_occupied "
                "FROM PARKING_SESSION WHERE slot_id=? AND exit_time IS NULL "
                "AND status IN ('ACTIVE','VIOLATION') "
                "ORDER BY session_id DESC LIMIT 1;");
            active.text(1, durable.command.slotId);
            const int active_step = sqlite3_step(active.get());
            if (active_step == SQLITE_DONE) return std::nullopt;
            if (active_step != SQLITE_ROW)
                throw std::runtime_error("active occupancy lookup failed");
            return ActiveSession{
                sqlite3_column_int64(active.get(), 0),
                columnText(active.get(), 1),
                sqlite3_column_int64(active.get(), 2),
                columnText(active.get(), 3),
                sqlite3_column_int(active.get(), 4) != 0,
                sqlite3_column_int(active.get(), 5) != 0,
                sqlite3_column_int(active.get(), 6) != 0,
                sqlite3_column_int(active.get(), 7) != 0};
        };

        const auto create_session = [&](const std::string& attempt_id,
                                        const std::string& event_type,
                                        const std::string& message) {
            Statement slot(db_,
                "UPDATE PARKING_SLOT SET status='OCCUPIED',updated_at=? "
                "WHERE slot_id=?;");
            slot.text(1, durable.command.occurredAt);
            slot.text(2, durable.command.slotId);
            done(db_, slot.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error("occupancy slot does not exist");

            Statement insert(db_,
                "INSERT INTO PARKING_SESSION(vehicle_id,slot_id,plate_number,"
                "entry_time,status,occupancy_attempt_id,entry_command_id,"
                "entry_time_epoch_ms,hall_confirmed,iva_confirmed,"
                "hall_occupied,iva_occupied) "
                "VALUES(NULL,?,NULL,?,'ACTIVE',?,?,?,?,?,?,?);");
            const bool from_hall = durable.command.kind ==
                parking::SlotCommandKind::HallObservation;
            const bool from_camera = durable.command.kind ==
                parking::SlotCommandKind::CameraObservation;
            insert.text(1, durable.command.slotId);
            insert.text(2, durable.command.occurredAt);
            insert.text(3, attempt_id);
            insert.text(4, command_id);
            insert.integer(5, occurred_epoch_ms);
            insert.integer(6, from_hall ? 1 : 0);
            insert.integer(7, from_camera ? 1 : 0);
            insert.integer(8, from_hall ? 1 : 0);
            insert.integer(9, from_camera ? 1 : 0);
            done(db_, insert.get());
            const auto session_id = sqlite3_last_insert_rowid(db_);

            Statement log(db_,
                "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
                "VALUES(?,?,?,?);");
            log.integer(1, session_id);
            log.text(2, durable.command.slotId);
            log.text(3, event_type);
            log.text(4, message);
            done(db_, log.get());
            return session_id;
        };

        const auto close_session = [&](const ActiveSession& active,
                                       const std::string& event_type,
                                       const std::string& message) {
            if (occurred_epoch_ms < active.entryEpochMs) return false;
            Statement close(db_,
                "UPDATE PARKING_SESSION SET status='ENDED',exit_time=?,"
                "exit_time_epoch_ms=?,duration_sec=MAX(0,(?-"
                "COALESCE(entry_time_epoch_ms,?))/1000),exit_command_id=?,"
                "hall_occupied=0,iva_occupied=0 "
                "WHERE session_id=? AND slot_id=? AND "
                "COALESCE(occupancy_attempt_id,'')=? AND exit_time IS NULL "
                "AND status IN ('ACTIVE','VIOLATION');");
            close.text(1, durable.command.occurredAt);
            close.integer(2, occurred_epoch_ms);
            close.integer(3, occurred_epoch_ms);
            close.integer(4, active.entryEpochMs);
            close.text(5, command_id);
            close.integer(6, active.id);
            close.text(7, durable.command.slotId);
            close.text(8, active.attemptId);
            done(db_, close.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error("exact occupancy close lost ownership");

            Statement slot(db_,
                "UPDATE PARKING_SLOT SET status='VACANT',updated_at=? "
                "WHERE slot_id=?;");
            slot.text(1, durable.command.occurredAt);
            slot.text(2, durable.command.slotId);
            done(db_, slot.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error(
                    "occupancy slot projection update lost ownership");

            Statement end_bindings(db_,
                "UPDATE PARKING_CORRELATION_BINDING SET state='ENDED',"
                "ended_at_epoch_ms=?,updated_at_epoch_ms=? "
                "WHERE session_id=? AND slot_id=? AND "
                "occupancy_attempt_id=? AND state='COMMITTED';");
            end_bindings.integer(1, occurred_epoch_ms);
            end_bindings.integer(2, occurred_epoch_ms);
            end_bindings.integer(3, active.id);
            end_bindings.text(4, durable.command.slotId);
            end_bindings.text(5, active.attemptId);
            done(db_, end_bindings.get());

            Statement clear_iva(db_,
                "UPDATE IVA_SLOT_OBSERVATION_STATE SET "
                "occupancy_attempt_id='',active_session_id=NULL,"
                "observed_state='VACANT',updated_at=CURRENT_TIMESTAMP "
                "WHERE slot_id=? AND active_session_id=?;");
            clear_iva.text(1, durable.command.slotId);
            clear_iva.integer(2, active.id);
            done(db_, clear_iva.get());

            Statement log(db_,
                "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
                "VALUES(?,?,?,?);");
            log.integer(1, active.id);
            log.text(2, durable.command.slotId);
            log.text(3, event_type);
            log.text(4, message);
            done(db_, log.get());
            return true;
        };

        const auto update_hall_state = [&](const std::int64_t session_id,
                                           const bool confirmed,
                                           const bool occupied) {
            Statement update(db_,
                "UPDATE PARKING_SESSION SET "
                "hall_confirmed=MAX(hall_confirmed,?),hall_occupied=? "
                "WHERE session_id=? AND exit_time IS NULL AND "
                "status IN ('ACTIVE','VIOLATION');");
            update.integer(1, confirmed ? 1 : 0);
            update.integer(2, occupied ? 1 : 0);
            update.integer(3, session_id);
            done(db_, update.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error(
                    "Hall source state update lost active session");
        };

        const auto update_iva_state = [&](const std::int64_t session_id,
                                          const bool confirmed,
                                          const bool occupied) {
            Statement update(db_,
                "UPDATE PARKING_SESSION SET "
                "iva_confirmed=MAX(iva_confirmed,?),iva_occupied=? "
                "WHERE session_id=? AND exit_time IS NULL AND "
                "status IN ('ACTIVE','VIOLATION');");
            update.integer(1, confirmed ? 1 : 0);
            update.integer(2, occupied ? 1 : 0);
            update.integer(3, session_id);
            done(db_, update.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error(
                    "IVA source state update lost active session");
        };

        const auto commit_sequence = [&]() {
            const auto fact = hallSequenceFact(durable.command, payload);
            if (!fact.sequence || durable.command.sensorId.empty()) return;
            const auto cursor = loadHallSequenceState(
                db_, durable.command.sensorId);
            const auto decision = sensor::evaluateSensorSequence(cursor, fact);
            if (!decision.accepted()) {
                throw std::runtime_error(
                    "Hall sequence commit lost ownership: " +
                    decision.reason);
            }

            if (fact.protocolVersion ==
                    sensor::SensorProtocolVersion::BootEpochV2 &&
                cursor.mode == sensor::SensorSequenceMode::Versioned &&
                cursor.activeBootId && fact.bootId &&
                *cursor.activeBootId != *fact.bootId) {
                Statement retire(db_,
                    "INSERT OR IGNORE INTO SENSOR_RETIRED_BOOT_ID("
                    "source_kind,sensor_id,boot_id) VALUES('HALL',?,?);");
                retire.text(1, durable.command.sensorId);
                retire.text(2, *cursor.activeBootId);
                done(db_, retire.get());
            }

            const bool versioned = fact.protocolVersion ==
                sensor::SensorProtocolVersion::BootEpochV2;
            Statement sequence(db_,
                "INSERT INTO OCCUPANCY_SENSOR_SEQUENCE_STATE("
                "sensor_id,protocol_mode,active_boot_id,last_sequence,"
                "last_command_id,updated_at) VALUES(?,?,?,?,?,"
                "CURRENT_TIMESTAMP) ON CONFLICT(sensor_id) DO UPDATE SET "
                "protocol_mode=excluded.protocol_mode,"
                "active_boot_id=excluded.active_boot_id,"
                "last_sequence=excluded.last_sequence,"
                "last_command_id=excluded.last_command_id,"
                "updated_at=CURRENT_TIMESTAMP;");
            sequence.text(1, durable.command.sensorId);
            sequence.text(2, versioned ? "VERSIONED" : "LEGACY");
            if (versioned) sequence.text(3, *fact.bootId);
            else sequence.null(3);
            sequence.text(4, std::to_string(*fact.sequence));
            sequence.text(5, command_id);
            done(db_, sequence.get());
        };

        const auto persist_iva = [&](const std::string& attempt_id,
                                     const std::optional<std::int64_t> session_id,
                                     const std::uint64_t generation,
                                     const std::string& state,
                                     const nlohmann::json& configured,
                                     const nlohmann::json& areas) {
            Statement update(db_,
                "INSERT INTO IVA_SLOT_OBSERVATION_STATE("
                "slot_id,occupancy_attempt_id,active_session_id,"
                "observation_generation,observed_state,configured_areas_json,"
                "area_states_json,last_source_command_id,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP) "
                "ON CONFLICT(slot_id) DO UPDATE SET "
                "occupancy_attempt_id=excluded.occupancy_attempt_id,"
                "active_session_id=excluded.active_session_id,"
                "observation_generation=excluded.observation_generation,"
                "observed_state=excluded.observed_state,"
                "configured_areas_json=excluded.configured_areas_json,"
                "area_states_json=excluded.area_states_json,"
                "last_source_command_id=excluded.last_source_command_id,"
                "updated_at=CURRENT_TIMESTAMP;");
            update.text(1, durable.command.slotId);
            update.text(2, attempt_id);
            if (session_id) update.integer(3, *session_id); else update.null(3);
            update.integer(4, static_cast<std::int64_t>(generation));
            update.text(5, state);
            update.text(6, configured.dump());
            update.text(7, areas.dump());
            update.text(8, command_id);
            done(db_, update.get());
        };

        const auto commit_correlation = [&query_payload = payload, &durable,
                                         &command_id, &outcome, this](
            const std::string& attempt_id,
            const std::int64_t session_id) -> std::string {
            const std::string proposed_id =
                query_payload.value("correlation_id", "");
            const std::string camera_id =
                query_payload.value("camera_id", "");
            const std::string channel_id =
                query_payload.value("channel_id", "");
            const std::string video_source_token =
                query_payload.value("video_source_token", "");
            const std::string rule_name =
                query_payload.value("rule_name", "");
            const std::string object_id =
                query_payload.value("object_id", "");
            const auto expires_at = query_payload.value<std::int64_t>(
                "correlation_expires_at_epoch_ms", 0);
            if (proposed_id.empty() || attempt_id.empty() || session_id < 0 ||
                camera_id.empty() || channel_id.empty() ||
                video_source_token.empty() || rule_name.empty() ||
                object_id.empty() || expires_at <= 0) {
                return {};
            }

            Statement existing(db_,
                "SELECT correlation_id,binding_revision,expires_at_epoch_ms,"
                "state,session_id,slot_id,channel_id "
                "FROM PARKING_CORRELATION_BINDING WHERE "
                "occupancy_attempt_id=? AND camera_id=? AND "
                "video_source_token=? AND rule_name=? AND object_id=? "
                "LIMIT 1;");
            existing.text(1, attempt_id);
            existing.text(2, camera_id);
            existing.text(3, video_source_token);
            existing.text(4, rule_name);
            existing.text(5, object_id);
            const int existing_step = sqlite3_step(existing.get());
            if (existing_step == SQLITE_ROW) {
                const std::string correlation_id =
                    columnText(existing.get(), 0);
                const auto current_revision = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(
                        1, sqlite3_column_int64(existing.get(), 1)));
                const auto current_expiry =
                    sqlite3_column_int64(existing.get(), 2);
                if (columnText(existing.get(), 3) != "COMMITTED" ||
                    sqlite3_column_int64(existing.get(), 4) != session_id ||
                    columnText(existing.get(), 5) != durable.command.slotId ||
                    columnText(existing.get(), 6) != channel_id) {
                    throw std::runtime_error(
                        "correlation track is already owned by another "
                        "session or channel");
                }
                std::uint64_t revision = current_revision;
                if (expires_at > current_expiry) {
                    Statement refresh(db_,
                        "UPDATE PARKING_CORRELATION_BINDING SET "
                        "expires_at_epoch_ms=?,binding_revision="
                        "binding_revision+1,updated_at_epoch_ms=? "
                        "WHERE correlation_id=? AND binding_revision=? AND "
                        "state='COMMITTED';");
                    refresh.integer(1, expires_at);
                    refresh.integer(2, outcome.occurredAtEpochMs);
                    refresh.text(3, correlation_id);
                    refresh.integer(4,
                        static_cast<std::int64_t>(current_revision));
                    done(db_, refresh.get());
                    if (sqlite3_changes(db_) != 1) {
                        throw std::runtime_error(
                            "correlation refresh lost ownership");
                    }
                    revision = current_revision + 1;
                }
                (void)revision;
                return correlation_id;
            }
            if (existing_step != SQLITE_DONE) {
                throw std::runtime_error(
                    "correlation track lookup failed");
            }

            Statement insert(db_,
                "INSERT INTO PARKING_CORRELATION_BINDING("
                "correlation_id,source_command_id,occupancy_attempt_id,"
                "session_id,slot_id,camera_id,video_source_token,rule_name,"
                "object_id,channel_id,binding_revision,state,"
                "created_at_epoch_ms,expires_at_epoch_ms,"
                "updated_at_epoch_ms,ended_at_epoch_ms) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,1,'COMMITTED',?,?,?,NULL);");
            insert.text(1, proposed_id);
            insert.text(2, command_id);
            insert.text(3, attempt_id);
            insert.integer(4, session_id);
            insert.text(5, durable.command.slotId);
            insert.text(6, camera_id);
            insert.text(7, video_source_token);
            insert.text(8, rule_name);
            insert.text(9, object_id);
            insert.text(10, channel_id);
            insert.integer(11, outcome.occurredAtEpochMs);
            insert.integer(12, expires_at);
            insert.integer(13, outcome.occurredAtEpochMs);
            done(db_, insert.get());
            return proposed_id;
        };

        std::string attempt_id = durable.occupancyAttemptId;
        std::uint64_t generation = durable.observationGeneration;

        if (durable.command.kind ==
            parking::SlotCommandKind::HallObservation) {
            const std::string state = payload.value("state", "");
            if (state != "OCCUPIED" && state != "VACANT")
                throw std::invalid_argument("unsupported Hall state");

            // A later command may have committed while this one waited.
            // Re-evaluate the complete protocol epoch in this transaction.
            const auto sequence_fact = hallSequenceFact(
                durable.command, payload);
            const auto sequence_cursor = loadHallSequenceState(
                db_, durable.command.sensorId);
            const auto sequence_decision = sensor::evaluateSensorSequence(
                sequence_cursor, sequence_fact);
            if (!sequence_decision.accepted()) {
                outcome.code = parking::CommittedOccupancyCode::NoChange;
                outcome.message = "Hall command rejected before apply: " +
                    sequence_decision.reason;
            } else if (state == "OCCUPIED") {
                if (const auto active = find_active()) {
                    update_hall_state(active->id, true, true);
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    outcome.sessionId = active->id;
                    attempt_id = active->attemptId;
                    outcome.message = hybrid_or
                        ? "active session confirmed by Hall sensor"
                        : "slot already has an active session";
                    commit_sequence();
                } else {
                    attempt_id = "occupancy:" + command_id;
                    outcome.sessionId = create_session(
                        attempt_id, "HALL_OCCUPIED",
                        "hall sensor=" + durable.command.sensorId +
                        " command=" + command_id);
                    outcome.code =
                        parking::CommittedOccupancyCode::SessionStarted;
                    outcome.message = "Hall session committed";
                    commit_sequence();
                }
            } else {
                const auto active = find_active();
                if (!active) {
                    Statement slot(db_,
                        "UPDATE PARKING_SLOT SET status='VACANT',updated_at=? "
                        "WHERE slot_id=?;");
                    slot.text(1, durable.command.occurredAt);
                    slot.text(2, durable.command.slotId);
                    done(db_, slot.get());
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    outcome.message = "slot already vacant";
                    commit_sequence();
                } else if (occurred_epoch_ms < active->entryEpochMs) {
                    outcome.code = parking::CommittedOccupancyCode::
                        RejectedInvalidTimestamp;
                    outcome.sessionId = active->id;
                    attempt_id = active->attemptId;
                    outcome.message = "VACANT timestamp precedes session entry";
                } else {
                    outcome.sessionId = active->id;
                    attempt_id = active->attemptId;
                    if (hybrid_or && !active->hallConfirmed) {
                        outcome.code =
                            parking::CommittedOccupancyCode::NoChange;
                        outcome.message =
                            "Hall VACANT ignored because Hall never confirmed "
                            "this session";
                        commit_sequence();
                    } else {
                        update_hall_state(
                            active->id, active->hallConfirmed, false);
                        if (hybrid_or && active->ivaConfirmed &&
                            active->ivaOccupied) {
                            outcome.code =
                                parking::CommittedOccupancyCode::NoChange;
                            outcome.message =
                                "Hall VACANT recorded; active IVA still holds "
                                "the session";
                            commit_sequence();
                        } else {
                            if (!close_session(*active, "HALL_VACANT",
                                               "hall sensor=" +
                                                   durable.command.sensorId +
                                                   " command=" + command_id)) {
                                throw std::runtime_error(
                                    "Hall close timestamp rejected");
                            }
                            outcome.code = parking::CommittedOccupancyCode::
                                SessionEnded;
                            outcome.message = "Hall session ended";
                            commit_sequence();
                        }
                    }
                }
            }
        } else if (durable.command.kind ==
                   parking::SlotCommandKind::CameraObservation) {
            const std::string action = payload.value("action", "");
            const bool authoritative_exit =
                payload.value("authoritative_exit", true);
            const bool occupancy_authority =
                payload.value("occupancy_authority", true);
            const std::string area_key = payload.value("area_key", "");
            const std::string observation_object_id =
                payload.value("object_id", "");
            nlohmann::json configured = payload.value(
                "configured_areas", nlohmann::json::array());
            if (!configured.is_array() || configured.empty() ||
                area_key.empty()) {
                throw std::invalid_argument("camera area topology is missing");
            }
            if ((action == "INTRUSION" ||
                 (action == "EXIT" && authoritative_exit)) &&
                observation_object_id.empty()) {
                throw std::invalid_argument(
                    "authoritative camera observation has no ObjectId");
            }

            if (!occupancy_authority) {
                if (action != "INTRUSION" ||
                    observation_object_id.empty()) {
                    throw std::invalid_argument(
                        "Hall correlation accepts exact INTRUSION only");
                }
                const auto active = find_active();
                if (!active) {
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    outcome.message =
                        "camera observation has no active Hall occupancy";
                } else {
                    attempt_id = active->attemptId;
                    outcome.sessionId = active->id;
                    outcome.correlationId = commit_correlation(
                        attempt_id, active->id);
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    outcome.message = outcome.correlationId.empty()
                        ? "Hall correlation payload is incomplete"
                        : "camera observation bound to active Hall occupancy";
                }
            } else {

            nlohmann::json areas = nlohmann::json::object();
            std::string observed_state = "UNKNOWN";
            Statement current(db_,
                "SELECT occupancy_attempt_id,active_session_id,"
                "observation_generation,observed_state,"
                "configured_areas_json,area_states_json "
                "FROM IVA_SLOT_OBSERVATION_STATE WHERE slot_id=?;");
            current.text(1, durable.command.slotId);
            const int current_step = sqlite3_step(current.get());
            if (current_step == SQLITE_ROW) {
                attempt_id = columnText(current.get(), 0);
                generation = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(
                        0, sqlite3_column_int64(current.get(), 2)));
                observed_state = columnText(current.get(), 3);
                const auto stored_configured = nlohmann::json::parse(
                    columnText(current.get(), 4));
                if (stored_configured != configured)
                    throw std::runtime_error(
                        "camera area topology changed without migration");
                areas = nlohmann::json::parse(columnText(current.get(), 5));
            }
            if (current_step != SQLITE_ROW && current_step != SQLITE_DONE)
                throw std::runtime_error(
                    "camera observation state lookup failed");

            const auto normalize_tracks = [&areas](const std::string& key)
                -> nlohmann::json& {
                auto& tracks = areas[key];
                if (tracks.is_null()) tracks = nlohmann::json::array();
                if (tracks.is_boolean()) {
                    const bool legacy_active = tracks.get<bool>();
                    tracks = nlohmann::json::array();
                    if (legacy_active) tracks.push_back("__legacy_active__");
                }
                if (!tracks.is_array()) {
                    throw std::runtime_error(
                        "camera track state has an invalid shape");
                }
                return tracks;
            };

            const auto active_at_observation = action == "EXIT"
                ? find_active() : std::optional<ActiveSession>{};

            if (action == "EXIT" && active_at_observation &&
                occurred_epoch_ms < active_at_observation->entryEpochMs) {
                outcome.code = parking::CommittedOccupancyCode::
                    RejectedInvalidTimestamp;
                outcome.sessionId = active_at_observation->id;
                attempt_id = active_at_observation->attemptId;
                outcome.message =
                    "camera EXIT timestamp precedes active session entry";
            } else if (action == "ENTER" ||
                (action == "EXIT" && !authoritative_exit)) {
                outcome.code = parking::CommittedOccupancyCode::NoChange;
                outcome.message = "camera action ignored by occupancy policy";
            } else if (action == "INTRUSION") {
                const std::string& object_id = observation_object_id;
                const std::string track_id = object_id.empty()
                    ? "__area_active__" : object_id;
                auto& tracks = normalize_tracks(area_key);
                // A legacy boolean state has no object identity. The first
                // exact observation transfers ownership to that track instead
                // of leaving an immortal sentinel behind.
                nlohmann::json exact_tracks = nlohmann::json::array();
                for (const auto& track : tracks) {
                    if (!track.is_string() ||
                        track.get<std::string>() != "__legacy_active__") {
                        exact_tracks.push_back(track);
                    }
                }
                tracks = std::move(exact_tracks);
                const bool changed = std::find(
                    tracks.begin(), tracks.end(), track_id) == tracks.end();
                if (changed) tracks.push_back(track_id);
                if (changed || observed_state != "OCCUPIED") ++generation;
                Statement supersede(db_,
                    "UPDATE OCCUPANCY_EXIT_DEADLINE SET state='SUPERSEDED',"
                    "updated_at=CURRENT_TIMESTAMP WHERE slot_id=? "
                    "AND state='SCHEDULED';");
                supersede.text(1, durable.command.slotId);
                done(db_, supersede.get());

                if (const auto active = find_active()) {
                    update_iva_state(active->id, true, true);
                    outcome.sessionId = active->id;
                    attempt_id = active->attemptId;
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    if (hybrid_or && !active->ivaConfirmed) {
                        outcome.message =
                            "active session confirmed by IVA intrusion";
                    } else {
                        outcome.message = changed
                            ? "camera occupancy refreshed"
                            : "duplicate camera intrusion";
                    }
                } else {
                    attempt_id = "occupancy:" + command_id;
                    outcome.sessionId = create_session(
                        attempt_id, "CAMERA_IVA_OCCUPIED",
                        "area=" + area_key + " object=" +
                            payload.value("object_id", "") +
                            " command=" + command_id);
                    outcome.code =
                        parking::CommittedOccupancyCode::SessionStarted;
                    outcome.message = "camera occupancy session committed";
                }
                persist_iva(attempt_id, outcome.sessionId, generation,
                            "OCCUPIED", configured, areas);
                outcome.correlationId = commit_correlation(
                    attempt_id, outcome.sessionId);
            } else if (action == "EXIT") {
                const std::string& object_id = observation_object_id;
                auto& tracks = normalize_tracks(area_key);
                const auto before = tracks.size();
                if (object_id.empty()) {
                    tracks = nlohmann::json::array();
                } else {
                    nlohmann::json retained = nlohmann::json::array();
                    for (const auto& track : tracks) {
                        if (!track.is_string()) {
                            throw std::runtime_error(
                                "camera track state contains a non-string id");
                        }
                        const auto stored_track = track.get<std::string>();
                        if (stored_track != object_id &&
                            stored_track != "__legacy_active__") {
                            retained.push_back(track);
                        }
                    }
                    tracks = std::move(retained);
                }
                const bool changed = tracks.size() != before;
                if (changed) ++generation;
                bool all_known = true;
                bool any_active = false;
                for (const auto& configured_area : configured) {
                    const auto key = configured_area.get<std::string>();
                    if (!areas.contains(key)) {
                        all_known = false;
                        continue;
                    }
                    any_active = any_active || !normalize_tracks(key).empty();
                }
                const auto active = active_at_observation;
                if (!active || !all_known || any_active) {
                    observed_state = active ? "OCCUPIED" : "VACANT";
                    if (active) {
                        attempt_id = active->attemptId;
                        persist_iva(
                            attempt_id,
                            std::optional<std::int64_t>{active->id},
                            generation, observed_state, configured, areas);
                    } else {
                        attempt_id.clear();
                        persist_iva(attempt_id, std::nullopt, generation,
                                    observed_state, configured, areas);
                    }
                    outcome.code = parking::CommittedOccupancyCode::NoChange;
                    outcome.sessionId = active ? active->id : -1;
                    outcome.message = !all_known
                        ? "camera exit waits for all configured areas"
                        : (any_active ? "another camera area remains active"
                                      : "camera exit has no active session");
                } else {
                    attempt_id = active->attemptId;
                    outcome.sessionId = active->id;
                    Statement live(db_,
                        "SELECT deadline_id FROM OCCUPANCY_EXIT_DEADLINE "
                        "WHERE slot_id=? AND state IN ('SCHEDULED','ADMITTED') "
                        "LIMIT 1;");
                    live.text(1, durable.command.slotId);
                    const int live_step = sqlite3_step(live.get());
                    if (live_step == SQLITE_DONE) {
                        const std::string deadline_id =
                            "iva-exit:" + durable.command.slotId + ":" +
                            std::to_string(generation) + ":" +
                            std::to_string(active->id);
                        Statement deadline(db_,
                            "INSERT INTO OCCUPANCY_EXIT_DEADLINE("
                            "deadline_id,slot_id,occupancy_attempt_id,"
                            "expected_session_id,observation_generation,"
                            "due_at_epoch_ms,occupancy_policy,state) "
                            "VALUES(?,?,?,?,?,?,?,'SCHEDULED');");
                        deadline.text(1, deadline_id);
                        deadline.text(2, durable.command.slotId);
                        deadline.text(3, attempt_id);
                        deadline.integer(4, active->id);
                        deadline.integer(5,
                            static_cast<std::int64_t>(generation));
                        deadline.integer(6, payload.value<std::int64_t>(
                            "deadline_due_at_epoch_ms",
                            durable.command.dueAtEpochMs));
                        deadline.text(7, hybrid_or ? "HYBRID_OR"
                                                   : "CAMERA_IVA");
                        done(db_, deadline.get());
                    }
                    if (live_step != SQLITE_ROW && live_step != SQLITE_DONE)
                        throw std::runtime_error(
                            "camera live deadline lookup failed");
                    persist_iva(attempt_id, active->id, generation,
                                "VACANT_PENDING", configured, areas);
                    outcome.code =
                        parking::CommittedOccupancyCode::ExitScheduled;
                    outcome.message = "camera exit deadline committed";
                }
            } else {
                throw std::invalid_argument("unsupported camera action");
            }
            }
        } else {
            const std::string deadline_id = payload.value("deadline_id", "");
            const auto expected_session = payload.value<std::int64_t>(
                "expected_session_id", -1);
            const auto expected_generation = payload.value<std::uint64_t>(
                "observation_generation", 0);
            const std::string expected_attempt = payload.value(
                "occupancy_attempt_id", "");
            Statement deadline(db_,
                "SELECT state,slot_id,occupancy_attempt_id,"
                "expected_session_id,observation_generation "
                "FROM OCCUPANCY_EXIT_DEADLINE WHERE deadline_id=?;");
            deadline.text(1, deadline_id);
            const int deadline_step = sqlite3_step(deadline.get());
            if (deadline_step != SQLITE_ROW && deadline_step != SQLITE_DONE)
                throw std::runtime_error("exit deadline lookup failed");
            if (deadline_step != SQLITE_ROW ||
                columnText(deadline.get(), 0) != "ADMITTED" ||
                columnText(deadline.get(), 1) != durable.command.slotId ||
                columnText(deadline.get(), 2) != expected_attempt ||
                sqlite3_column_int64(deadline.get(), 3) != expected_session ||
                static_cast<std::uint64_t>(
                    sqlite3_column_int64(deadline.get(), 4)) !=
                    expected_generation) {
                outcome.code =
                    parking::CommittedOccupancyCode::StaleGeneration;
                outcome.message = "exit deadline no longer owns the slot";
            } else {
                Statement iva(db_,
                    "SELECT occupancy_attempt_id,active_session_id,"
                    "observation_generation,observed_state,"
                    "configured_areas_json,area_states_json "
                    "FROM IVA_SLOT_OBSERVATION_STATE WHERE slot_id=?;");
                iva.text(1, durable.command.slotId);
                const int iva_step = sqlite3_step(iva.get());
                if (iva_step != SQLITE_ROW && iva_step != SQLITE_DONE)
                    throw std::runtime_error(
                        "IVA ownership state lookup failed");
                const bool owns = iva_step == SQLITE_ROW &&
                    columnText(iva.get(), 0) == expected_attempt &&
                    optionalInt64(iva.get(), 1) ==
                        std::optional<std::int64_t>{expected_session} &&
                    static_cast<std::uint64_t>(
                        sqlite3_column_int64(iva.get(), 2)) ==
                        expected_generation &&
                    columnText(iva.get(), 3) == "VACANT_PENDING";
                const auto active = find_active();
                if (!owns || !active || active->id != expected_session ||
                    active->attemptId != expected_attempt) {
                    Statement stale(db_,
                        "UPDATE OCCUPANCY_EXIT_DEADLINE SET "
                        "state='SUPERSEDED',updated_at=CURRENT_TIMESTAMP "
                        "WHERE deadline_id=? AND state='ADMITTED';");
                    stale.text(1, deadline_id);
                    done(db_, stale.get());
                    outcome.code =
                        parking::CommittedOccupancyCode::StaleGeneration;
                    outcome.message = "exit deadline generation is stale";
                } else {
                    update_iva_state(
                        active->id, active->ivaConfirmed, false);
                    Statement applied(db_,
                        "UPDATE OCCUPANCY_EXIT_DEADLINE SET state='APPLIED',"
                        "updated_at=CURRENT_TIMESTAMP WHERE deadline_id=? "
                        "AND state='ADMITTED';");
                    applied.text(1, deadline_id);
                    done(db_, applied.get());
                    const auto configured = nlohmann::json::parse(
                        columnText(iva.get(), 4));
                    const auto areas = nlohmann::json::parse(
                        columnText(iva.get(), 5));
                    outcome.sessionId = active->id;
                    attempt_id = active->attemptId;
                    generation = expected_generation;
                    if (hybrid_or && active->hallConfirmed &&
                        active->hallOccupied) {
                        persist_iva(attempt_id, active->id,
                                    expected_generation, "VACANT",
                                    configured, areas);
                        outcome.code =
                            parking::CommittedOccupancyCode::NoChange;
                        outcome.message =
                            "IVA VACANT recorded; active Hall sensor still "
                            "holds the session";
                    } else {
                        if (!close_session(*active, "CAMERA_IVA_VACANT",
                                           "deadline=" + deadline_id +
                                               " command=" + command_id)) {
                            throw std::runtime_error(
                                "camera deadline timestamp rejected");
                        }
                        persist_iva("", std::nullopt, expected_generation,
                                    "VACANT", configured, areas);
                        outcome.code =
                            parking::CommittedOccupancyCode::SessionEnded;
                        outcome.message = "camera exit deadline applied";
                    }
                }
            }
        }

        outcome.occupancyAttemptId = attempt_id;
        outcome.observationGeneration = generation;
        const bool rejected = outcome.code ==
            parking::CommittedOccupancyCode::RejectedInvalidTimestamp;
        const std::string inbox_status = rejected
            ? "REJECTED_INVALID" : "APPLIED";
        const bool has_effect = !rejected &&
            (outcome.code == parking::CommittedOccupancyCode::SessionStarted ||
             outcome.code == parking::CommittedOccupancyCode::SessionEnded);
        Statement finish(db_,
            "UPDATE OCCUPANCY_COMMAND_INBOX SET status=?,"
            "occupancy_attempt_id=?,correlation_id=?,"
            "observation_generation=?,"
            "result_code=?,result_session_id=?,last_error=?,effect_state=?,"
            "effect_attempt_count=0,effect_next_attempt_at_epoch_ms=0,"
            "effect_last_error='',"
            "updated_at=CURRENT_TIMESTAMP WHERE command_id=? AND "
            "status='PENDING_UNPREPARED';");
        finish.text(1, inbox_status);
        finish.text(2, attempt_id);
        finish.text(3, outcome.correlationId);
        finish.integer(4, static_cast<std::int64_t>(generation));
        finish.text(5, resultCodeName(outcome.code));
        if (outcome.sessionId >= 0)
            finish.integer(6, outcome.sessionId);
        else
            finish.null(6);
        finish.text(7, rejected ? outcome.message : std::string{});
        finish.text(8, has_effect ? "PENDING" : "NONE");
        finish.text(9, command_id);
        done(db_, finish.get());
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("occupancy inbox ownership changed");
        executeSqlUnlocked("COMMIT;");
        return outcome;
    } catch (const std::invalid_argument& error) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        // Invalid payloads are deterministic; make them terminal in a second
        // small transaction while leaving all domain state untouched.
        try {
            executeSqlUnlocked("BEGIN IMMEDIATE;");
            Statement reject(db_,
                "UPDATE OCCUPANCY_COMMAND_INBOX SET status='REJECTED_INVALID',"
                "result_code='REJECTED_INVALID_TIMESTAMP',last_error=?,"
                "updated_at=CURRENT_TIMESTAMP WHERE command_id=? AND "
                "status='PENDING_UNPREPARED';");
            reject.text(1, error.what());
            reject.text(2, command_id);
            done(db_, reject.get());
            executeSqlUnlocked("COMMIT;");
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        outcome.code =
            parking::CommittedOccupancyCode::RejectedInvalidTimestamp;
        outcome.message = error.what();
        return outcome;
    } catch (const std::exception& error) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        outcome.code = parking::CommittedOccupancyCode::RetryableFailure;
        outcome.message = error.what();
        return outcome;
    }
}

std::vector<parking::CommittedOccupancyTransition>
EventDatabase::listPendingSlotTransitionEffects(
    const std::int64_t now_epoch_ms) const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return {};
    Statement statement(db_,
        "SELECT command_id,source_kind,slot_id,sensor_id,occurred_at,"
        "payload_json,occupancy_attempt_id,correlation_id,"
        "observation_generation,"
        "result_code,result_session_id FROM OCCUPANCY_COMMAND_INBOX c "
        "WHERE c.status='APPLIED' AND c.effect_state='PENDING' "
        "AND c.effect_next_attempt_at_epoch_ms<=? "
        "AND NOT EXISTS (SELECT 1 FROM OCCUPANCY_COMMAND_INBOX earlier "
        "WHERE earlier.slot_id=c.slot_id "
        "AND earlier.status='APPLIED' AND earlier.effect_state='PENDING' "
        "AND earlier.admission_ordinal<c.admission_ordinal) "
        "ORDER BY c.admission_ordinal;");
    statement.integer(1, now_epoch_ms);
    std::vector<parking::CommittedOccupancyTransition> effects;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            throw std::runtime_error("occupancy effect lookup failed");
        parking::CommittedOccupancyTransition effect;
        effect.commandId = columnText(statement.get(), 0);
        effect.sourceKind = parking::slotCommandKindFromString(
            columnText(statement.get(), 1)).value_or(
                parking::SlotCommandKind::HallObservation);
        effect.slotId = columnText(statement.get(), 2);
        effect.sensorId = columnText(statement.get(), 3);
        effect.occurredAt = columnText(statement.get(), 4);
        effect.occupancyAttemptId = columnText(statement.get(), 6);
        effect.correlationId = columnText(statement.get(), 7);
        effect.observationGeneration = static_cast<std::uint64_t>(
            std::max<std::int64_t>(
                0, sqlite3_column_int64(statement.get(), 8)));
        effect.code = resultCodeFromName(columnText(statement.get(), 9));
        if (const auto session = optionalInt64(statement.get(), 10))
            effect.sessionId = *session;
        try {
            const auto payload = nlohmann::json::parse(
                columnText(statement.get(), 5));
            effect.sourceTransport = payload.value("transport", "unknown");
            effect.occurredAtEpochMs = payload.value<std::int64_t>(
                "occurred_at_epoch_ms", 0);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "persisted occupancy effect payload is invalid: " +
                std::string(error.what()));
        }
        if (effect.code != parking::CommittedOccupancyCode::SessionStarted &&
            effect.code != parking::CommittedOccupancyCode::SessionEnded) {
            throw std::runtime_error(
                "pending occupancy effect has a non-effect result");
        }
        effects.push_back(std::move(effect));
    }
    return effects;
}

bool EventDatabase::completeSlotTransitionEffects(
    const std::string& command_id) {
    if (command_id.empty()) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    Statement statement(db_,
        "UPDATE OCCUPANCY_COMMAND_INBOX SET effect_state='APPLIED',"
        "effect_next_attempt_at_epoch_ms=0,effect_last_error='',"
        "updated_at=CURRENT_TIMESTAMP WHERE command_id=? "
        "AND status='APPLIED' AND effect_state='PENDING';");
    statement.text(1, command_id);
    done(db_, statement.get());
    return sqlite3_changes(db_) == 1;
}

bool EventDatabase::deferSlotTransitionEffects(
    const std::string& command_id,
    const std::int64_t next_attempt_at_epoch_ms,
    const std::string& error) noexcept {
    try {
        std::lock_guard lock(db_mutex_);
        if (!opened_ || db_ == nullptr || command_id.empty()) return false;
        Statement statement(db_,
            "UPDATE OCCUPANCY_COMMAND_INBOX SET "
            "effect_attempt_count=effect_attempt_count+1,"
            "effect_next_attempt_at_epoch_ms=?,effect_last_error=?,"
            "updated_at=CURRENT_TIMESTAMP WHERE command_id=? "
            "AND status='APPLIED' AND effect_state='PENDING';");
        statement.integer(1, next_attempt_at_epoch_ms);
        statement.text(2, error);
        statement.text(3, command_id);
        done(db_, statement.get());
        return sqlite3_changes(db_) == 1;
    } catch (...) {
        return false;
    }
}

std::size_t EventDatabase::pendingSlotTransitionEffectCount() const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return 0;
    Statement statement(db_,
        "SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX "
        "WHERE status='APPLIED' AND effect_state='PENDING';");
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW)
        throw std::runtime_error("occupancy pending effect count failed");
    return static_cast<std::size_t>(
        std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 0)));
}

std::size_t EventDatabase::pendingSlotTransitionDrainCount(
    const std::int64_t shutdown_cutoff_epoch_ms) const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr)
        throw std::runtime_error("occupancy drain count requires an open DB");
    Statement statement(db_,
        "SELECT "
        "(SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
        "status='PENDING_UNPREPARED' AND due_at_epoch_ms<=?) + "
        "(SELECT COUNT(*) FROM OCCUPANCY_COMMAND_INBOX WHERE "
        "status='APPLIED' AND effect_state='PENDING') + "
        "(SELECT COUNT(*) FROM OCCUPANCY_EXIT_DEADLINE WHERE "
        "state='SCHEDULED' AND due_at_epoch_ms<=?);");
    statement.integer(1, shutdown_cutoff_epoch_ms);
    statement.integer(2, shutdown_cutoff_epoch_ms);
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW)
        throw std::runtime_error("occupancy drain count failed");
    return static_cast<std::size_t>(
        std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 0)));
}

}  // namespace database

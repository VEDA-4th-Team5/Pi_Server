#include "database/EventDatabase.hpp"

#include "sensor/SensorSequencePolicy.hpp"
#include "util/Logger.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>
#include <string_view>

namespace database {
namespace {

class FireStatement {
public:
    FireStatement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error(
                "SQLite Fire prepare failed: " +
                std::string(sqlite3_errmsg(database_)));
        }
    }

    ~FireStatement() {
        if (statement_) sqlite3_finalize(statement_);
    }

    FireStatement(const FireStatement&) = delete;
    FireStatement& operator=(const FireStatement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bindText(const int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(), -1,
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error("SQLite Fire text bind failed");
        }
    }

    void bindOptionalText(const int index,
                          const std::optional<std::string>& value) {
        if (value) {
            bindText(index, *value);
        } else if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error("SQLite Fire null bind failed");
        }
    }

    void bindInt64(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("SQLite Fire integer bind failed");
        }
    }

    void bindInt(const int index, const int value) {
        if (sqlite3_bind_int(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("SQLite Fire integer bind failed");
        }
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string columnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::optional<std::string> columnOptionalText(sqlite3_stmt* statement,
                                              const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(statement, column);
}

std::optional<std::uint64_t> parseOptionalSequence(
    sqlite3_stmt* statement, const int column) {
    const auto text = columnOptionalText(statement, column);
    if (!text) return std::nullopt;
    std::uint64_t value{};
    const auto* begin = text->data();
    const auto* end = begin + text->size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::runtime_error("invalid Fire source sequence in SQLite");
    }
    return value;
}

std::int64_t checkedInt64(const std::uint64_t value,
                          const std::string_view field) {
    if (value > event::kMaxFireRevision) {
        throw std::invalid_argument(std::string(field) + " exceeds SQLite INTEGER");
    }
    return static_cast<std::int64_t>(value);
}

void requireDone(sqlite3* database, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite Fire statement failed: " +
            std::string(sqlite3_errmsg(database)));
    }
}

event::FireAlarmStateRecord readState(sqlite3_stmt* statement) {
    event::FireAlarmStateRecord row;
    row.channelId = columnText(statement, 0);
    row.sensorId = columnText(statement, 1);
    row.retainedTopic = columnText(statement, 2);
    const auto lifecycle =
        event::fireAlarmLifecycleFromString(columnText(statement, 3));
    const auto protocol =
        event::fireProtocolModeFromString(columnText(statement, 7));
    const auto revision = sqlite3_column_int64(statement, 6);
    if (!lifecycle || !protocol || revision <= 0) {
        throw std::runtime_error("invalid Fire state row in SQLite");
    }
    row.desiredLifecycle = *lifecycle;
    row.activeAlarmId = columnText(statement, 4);
    row.lastEventId = columnText(statement, 5);
    row.fireRevision = static_cast<std::uint64_t>(revision);
    row.protocolMode = *protocol;
    row.activeBootId = columnOptionalText(statement, 8);
    row.lastSourceSequence = parseOptionalSequence(statement, 9);
    row.lastSignalJson = columnText(statement, 10);
    row.updatedAt = columnText(statement, 11);
    return row;
}

event::FireOutboxRecord readDelivery(sqlite3_stmt* statement) {
    event::FireOutboxRecord row;
    row.deliveryKey = columnText(statement, 0);
    const auto sink = event::fireDeliverySinkFromString(columnText(statement, 1));
    const auto state =
        event::fireDeliveryStateFromString(columnText(statement, 15));
    const auto revision = sqlite3_column_int64(statement, 7);
    const auto attempts = sqlite3_column_int64(statement, 12);
    if (!sink || !state || revision <= 0 || attempts < 0) {
        throw std::runtime_error("invalid Fire outbox row in SQLite");
    }
    row.sinkKind = *sink;
    row.logicalKey = columnText(statement, 2);
    row.sensorId = columnText(statement, 3);
    row.channelId = columnText(statement, 4);
    row.eventId = columnText(statement, 5);
    row.alarmId = columnText(statement, 6);
    row.fireRevision = static_cast<std::uint64_t>(revision);
    row.topic = columnText(statement, 8);
    row.payloadJson = columnText(statement, 9);
    row.qos = sqlite3_column_int(statement, 10);
    row.retain = sqlite3_column_int(statement, 11) != 0;
    row.attemptCount = static_cast<std::uint64_t>(attempts);
    row.nextAttemptAt = columnText(statement, 13);
    row.lastError = columnText(statement, 14);
    row.deliveryState = *state;
    const auto acknowledged = sqlite3_column_int64(statement, 16);
    if (sqlite3_column_type(statement, 16) != SQLITE_NULL) {
        if (acknowledged <= 0) {
            throw std::runtime_error("invalid acknowledged Fire revision");
        }
        row.acknowledgedRevision = static_cast<std::uint64_t>(acknowledged);
    }
    row.createdAt = columnText(statement, 17);
    row.updatedAt = columnText(statement, 18);
    return row;
}

constexpr const char* kStateColumns =
    "channel_id,sensor_id,retained_topic,desired_lifecycle,"
    "COALESCE(active_alarm_id,''),last_event_id,fire_revision,protocol_mode,"
    "active_boot_id,last_source_sequence,last_signal_json,updated_at";

constexpr const char* kDeliveryColumns =
    "delivery_key,sink_kind,logical_key,sensor_id,channel_id,event_id,"
    "alarm_id,fire_revision,topic,payload_json,qos,retain,attempt_count,"
    "next_attempt_at,last_error,delivery_state,acked_revision,created_at,"
    "updated_at";

bool sameStateSemantic(const event::FireAlarmStateRecord& left,
                       const event::FireAlarmStateRecord& right) {
    return left.channelId == right.channelId &&
           left.sensorId == right.sensorId &&
           left.retainedTopic == right.retainedTopic &&
           left.desiredLifecycle == right.desiredLifecycle &&
           left.activeAlarmId == right.activeAlarmId &&
           left.lastEventId == right.lastEventId &&
           left.fireRevision == right.fireRevision &&
           left.protocolMode == right.protocolMode &&
           left.activeBootId == right.activeBootId &&
           left.lastSourceSequence == right.lastSourceSequence &&
           left.lastSignalJson == right.lastSignalJson;
}

bool sameDeliverySemantic(const event::FireOutboxRecord& left,
                          const event::FireOutboxRecord& right) {
    return left.deliveryKey == right.deliveryKey &&
           left.sinkKind == right.sinkKind &&
           left.logicalKey == right.logicalKey &&
           left.sensorId == right.sensorId &&
           left.channelId == right.channelId &&
           left.eventId == right.eventId &&
           left.alarmId == right.alarmId &&
           left.fireRevision == right.fireRevision &&
           left.topic == right.topic &&
           left.payloadJson == right.payloadJson &&
           left.qos == right.qos && left.retain == right.retain;
}

bool validDelivery(const event::FireOutboxRecord& row) {
    return !row.deliveryKey.empty() && !row.logicalKey.empty() &&
           !row.sensorId.empty() && !row.channelId.empty() &&
           !row.eventId.empty() && !row.alarmId.empty() &&
           row.fireRevision > 0 &&
           row.fireRevision <= event::kMaxFireRevision && !row.topic.empty() &&
           !row.payloadJson.empty() && row.qos == 1 &&
           row.retain ==
               (row.sinkKind == event::FireDeliverySinkKind::RetainedState) &&
           !row.nextAttemptAt.empty() && !row.createdAt.empty() &&
           !row.updatedAt.empty();
}

bool validBootstrap(const event::FireChannelBootstrap& bootstrap) {
    const auto& state = bootstrap.initialState;
    const auto& retained = bootstrap.retainedDelivery;
    return !state.channelId.empty() && !state.sensorId.empty() &&
           !state.retainedTopic.empty() && state.fireRevision > 0 &&
           state.fireRevision <= event::kMaxFireRevision &&
           state.desiredLifecycle == event::FireAlarmLifecycle::Resolved &&
           state.activeAlarmId.empty() && !state.lastEventId.empty() &&
           state.protocolMode == event::FireProtocolMode::Unseen &&
           !state.activeBootId && !state.lastSourceSequence &&
           !state.lastSignalJson.empty() && !state.updatedAt.empty() &&
           validDelivery(retained) &&
           retained.sinkKind == event::FireDeliverySinkKind::RetainedState &&
           retained.logicalKey == state.retainedTopic &&
           retained.sensorId == state.sensorId &&
           retained.channelId == state.channelId &&
           retained.eventId == state.lastEventId &&
           retained.fireRevision == state.fireRevision &&
           retained.topic == state.retainedTopic &&
           retained.attemptCount == 0 && retained.lastError.empty() &&
           retained.deliveryState == event::FireDeliveryState::Pending &&
           !retained.acknowledgedRevision;
}

std::optional<event::FireAlarmStateRecord> selectState(
    sqlite3* database, const std::string& channel_id) {
    const std::string sql =
        std::string("SELECT ") + kStateColumns +
        " FROM FIRE_ALARM_STATE WHERE channel_id=? LIMIT 1;";
    FireStatement statement(database, sql.c_str());
    statement.bindText(1, channel_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return readState(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error("SQLite Fire state read failed");
}

std::optional<event::FireOutboxRecord> selectDelivery(
    sqlite3* database, const std::string& delivery_key) {
    const std::string sql =
        std::string("SELECT ") + kDeliveryColumns +
        " FROM FIRE_MQTT_OUTBOX WHERE delivery_key=? LIMIT 1;";
    FireStatement statement(database, sql.c_str());
    statement.bindText(1, delivery_key);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return readDelivery(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error("SQLite Fire delivery read failed");
}

void insertState(sqlite3* database,
                 const event::FireAlarmStateRecord& row) {
    FireStatement statement(database,
        "INSERT INTO FIRE_ALARM_STATE("
        "channel_id,sensor_id,retained_topic,desired_lifecycle,"
        "active_alarm_id,last_event_id,fire_revision,protocol_mode,"
        "active_boot_id,last_source_sequence,last_signal_json,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
    statement.bindText(1, row.channelId);
    statement.bindText(2, row.sensorId);
    statement.bindText(3, row.retainedTopic);
    statement.bindText(4, event::toString(row.desiredLifecycle));
    if (row.activeAlarmId.empty()) {
        statement.bindOptionalText(5, std::nullopt);
    } else {
        statement.bindText(5, row.activeAlarmId);
    }
    statement.bindText(6, row.lastEventId);
    statement.bindInt64(7, checkedInt64(row.fireRevision, "fire revision"));
    statement.bindText(8, event::toString(row.protocolMode));
    statement.bindOptionalText(9, row.activeBootId);
    if (row.lastSourceSequence) {
        statement.bindText(10, std::to_string(*row.lastSourceSequence));
    } else {
        statement.bindOptionalText(10, std::nullopt);
    }
    statement.bindText(11, row.lastSignalJson);
    statement.bindText(12, row.updatedAt);
    requireDone(database, statement.get());
}

void insertDelivery(sqlite3* database,
                    const event::FireOutboxRecord& row) {
    FireStatement statement(database,
        "INSERT INTO FIRE_MQTT_OUTBOX("
        "delivery_key,sink_kind,logical_key,sensor_id,channel_id,event_id,"
        "alarm_id,fire_revision,topic,payload_json,qos,retain,attempt_count,"
        "next_attempt_at,last_error,delivery_state,acked_revision,created_at,"
        "updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    statement.bindText(1, row.deliveryKey);
    statement.bindText(2, event::toString(row.sinkKind));
    statement.bindText(3, row.logicalKey);
    statement.bindText(4, row.sensorId);
    statement.bindText(5, row.channelId);
    statement.bindText(6, row.eventId);
    statement.bindText(7, row.alarmId);
    statement.bindInt64(8, checkedInt64(row.fireRevision, "fire revision"));
    statement.bindText(9, row.topic);
    statement.bindText(10, row.payloadJson);
    statement.bindInt(11, row.qos);
    statement.bindInt(12, row.retain ? 1 : 0);
    statement.bindInt64(13, checkedInt64(row.attemptCount, "attempt count"));
    statement.bindText(14, row.nextAttemptAt);
    statement.bindText(15, row.lastError);
    statement.bindText(16, event::toString(row.deliveryState));
    if (row.acknowledgedRevision) {
        statement.bindInt64(
            17, checkedInt64(*row.acknowledgedRevision,
                             "acknowledged revision"));
    } else {
        statement.bindOptionalText(17, std::nullopt);
    }
    statement.bindText(18, row.createdAt);
    statement.bindText(19, row.updatedAt);
    requireDone(database, statement.get());
}

event::FireStoreMutationResult failure(
    const event::FireStoreMutationOutcome outcome,
    const std::uint64_t revision,
    std::string error) {
    return {outcome, revision, std::move(error)};
}

}  // namespace

event::FireStoreMutationResult EventDatabase::initializeFireTopology(
    const std::vector<event::FireChannelBootstrap>& topology) {
    if (topology.empty()) {
        return failure(event::FireStoreMutationOutcome::Invalid, 0,
                       "Fire topology is empty");
    }

    std::set<std::string> channels;
    std::set<std::string> sensors;
    std::set<std::string> topics;
    std::set<std::string> delivery_keys;
    for (const auto& bootstrap : topology) {
        if (!validBootstrap(bootstrap) ||
            !channels.insert(bootstrap.initialState.channelId).second ||
            !sensors.insert(bootstrap.initialState.sensorId).second ||
            !topics.insert(bootstrap.initialState.retainedTopic).second ||
            !delivery_keys.insert(
                bootstrap.retainedDelivery.deliveryKey).second) {
            return failure(event::FireStoreMutationOutcome::Invalid, 0,
                           "invalid Fire topology bootstrap");
        }
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        return failure(event::FireStoreMutationOutcome::Failed, 0,
                       "Fire database is not open");
    }

    bool transaction_started = false;
    try {
        executeSqlUnlocked("BEGIN IMMEDIATE;");
        transaction_started = true;
        bool inserted = false;
        std::uint64_t highest_revision{};

        for (const auto& bootstrap : topology) {
            const auto& initial_state = bootstrap.initialState;
            const auto& retained_delivery = bootstrap.retainedDelivery;
            const auto existing = selectState(db_, initial_state.channelId);
            if (existing) {
                highest_revision =
                    std::max(highest_revision, existing->fireRevision);
                if (existing->sensorId != initial_state.sensorId ||
                    existing->retainedTopic != initial_state.retainedTopic) {
                    executeSqlUnlocked("ROLLBACK;");
                    transaction_started = false;
                    return failure(
                        event::FireStoreMutationOutcome::Conflict,
                        existing->fireRevision,
                        "configured Fire mapping differs from durable state");
                }

                const auto retained =
                    selectDelivery(db_, retained_delivery.deliveryKey);
                if (!retained ||
                    retained->sinkKind !=
                        event::FireDeliverySinkKind::RetainedState ||
                    retained->logicalKey != existing->retainedTopic ||
                    retained->sensorId != existing->sensorId ||
                    retained->channelId != existing->channelId ||
                    retained->eventId != existing->lastEventId ||
                    retained->fireRevision != existing->fireRevision ||
                    retained->topic != existing->retainedTopic) {
                    throw std::runtime_error(
                        "durable Fire state and retained outbox disagree");
                }
                continue;
            }

            insertState(db_, initial_state);
            insertDelivery(db_, retained_delivery);
            inserted = true;
            highest_revision =
                std::max(highest_revision, initial_state.fireRevision);
        }

        FireStatement state_count(
            db_, "SELECT COUNT(*) FROM FIRE_ALARM_STATE;");
        if (sqlite3_step(state_count.get()) != SQLITE_ROW) {
            throw std::runtime_error("durable Fire topology count failed");
        }
        const auto durable_channel_count =
            sqlite3_column_int64(state_count.get(), 0);
        if (sqlite3_step(state_count.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "durable Fire topology count did not finish");
        }

        FireStatement retained_count(
            db_, "SELECT COUNT(*) FROM FIRE_MQTT_OUTBOX "
                 "WHERE sink_kind='RETAINED_STATE';");
        if (sqlite3_step(retained_count.get()) != SQLITE_ROW) {
            throw std::runtime_error("durable Fire retained count failed");
        }
        const auto durable_retained_count =
            sqlite3_column_int64(retained_count.get(), 0);
        if (sqlite3_step(retained_count.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "durable Fire retained count did not finish");
        }

        const auto configured_count =
            static_cast<std::int64_t>(topology.size());
        if (durable_channel_count != configured_count ||
            durable_retained_count != configured_count) {
            executeSqlUnlocked("ROLLBACK;");
            transaction_started = false;
            return failure(
                event::FireStoreMutationOutcome::Conflict,
                highest_revision,
                "durable Fire channels do not match configured topology");
        }

        executeSqlUnlocked("COMMIT;");
        transaction_started = false;
        return {inserted ? event::FireStoreMutationOutcome::Committed
                         : event::FireStoreMutationOutcome::Idempotent,
                highest_revision, {}};
    } catch (const std::exception& error) {
        if (transaction_started) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        util::logError("Fire topology bootstrap failed: " +
                       std::string(error.what()));
        return failure(event::FireStoreMutationOutcome::Failed, 0,
                       error.what());
    }
}

event::FireStoreMutationResult EventDatabase::applyFireStateMutation(
    const event::FireStateMutation& mutation) {
    const bool has_retained = mutation.retainedDelivery.has_value();
    const bool has_lifecycle = mutation.lifecycleDelivery.has_value();
    const bool transition = has_retained && has_lifecycle;
    if (mutation.expectedRevision > event::kMaxFireRevision ||
        mutation.nextState.fireRevision > event::kMaxFireRevision ||
        mutation.nextState.channelId.empty() ||
        mutation.nextState.sensorId.empty() ||
        mutation.nextState.retainedTopic.empty() ||
        mutation.nextState.lastEventId.empty() ||
        mutation.nextState.lastSignalJson.empty() ||
        mutation.nextState.updatedAt.empty() || has_retained != has_lifecycle ||
        (transition &&
         (mutation.expectedRevision == event::kMaxFireRevision ||
           mutation.nextState.fireRevision != mutation.expectedRevision + 1)) ||
        (!transition &&
         mutation.nextState.fireRevision != mutation.expectedRevision) ||
        (transition &&
         (!validDelivery(*mutation.retainedDelivery) ||
          !validDelivery(*mutation.lifecycleDelivery) ||
          mutation.retainedDelivery->sinkKind !=
              event::FireDeliverySinkKind::RetainedState ||
          mutation.lifecycleDelivery->sinkKind !=
              event::FireDeliverySinkKind::LifecycleEvent ||
          mutation.retainedDelivery->fireRevision !=
              mutation.nextState.fireRevision ||
          mutation.lifecycleDelivery->fireRevision !=
              mutation.nextState.fireRevision ||
          mutation.retainedDelivery->channelId !=
              mutation.nextState.channelId ||
          mutation.lifecycleDelivery->channelId !=
              mutation.nextState.channelId))) {
        return failure(event::FireStoreMutationOutcome::Invalid,
                       mutation.expectedRevision,
                       "invalid Fire state mutation");
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        return failure(event::FireStoreMutationOutcome::Failed,
                       mutation.expectedRevision,
                       "Fire database is not open");
    }
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        const auto current = selectState(db_, mutation.nextState.channelId);
        if (!current) {
            executeSqlUnlocked("ROLLBACK;");
            return failure(event::FireStoreMutationOutcome::Conflict, 0,
                           "Fire channel has not been initialized");
        }

        if (current->fireRevision == mutation.nextState.fireRevision &&
            sameStateSemantic(*current, mutation.nextState)) {
            bool deliveries_match = true;
            if (transition) {
                const auto retained = selectDelivery(
                    db_, mutation.retainedDelivery->deliveryKey);
                const auto lifecycle = selectDelivery(
                    db_, mutation.lifecycleDelivery->deliveryKey);
                deliveries_match = retained && lifecycle &&
                    sameDeliverySemantic(*retained,
                                         *mutation.retainedDelivery) &&
                    sameDeliverySemantic(*lifecycle,
                                         *mutation.lifecycleDelivery);
            }
            if (!deliveries_match) {
                throw std::runtime_error(
                    "same Fire revision has different delivery payload");
            }
            executeSqlUnlocked("COMMIT;");
            return {event::FireStoreMutationOutcome::Idempotent,
                    current->fireRevision, {}};
        }

        if (current->fireRevision != mutation.expectedRevision) {
            executeSqlUnlocked("ROLLBACK;");
            return failure(event::FireStoreMutationOutcome::Conflict,
                           current->fireRevision,
                           "Fire revision compare-and-swap conflict");
        }
        if (current->sensorId != mutation.nextState.sensorId ||
            current->retainedTopic != mutation.nextState.retainedTopic) {
            executeSqlUnlocked("ROLLBACK;");
            return failure(event::FireStoreMutationOutcome::Conflict,
                           current->fireRevision,
                           "Fire mapping cannot change during a mutation");
        }
        const bool cursor_changed =
            current->protocolMode != mutation.nextState.protocolMode ||
            current->activeBootId != mutation.nextState.activeBootId ||
            current->lastSourceSequence !=
                mutation.nextState.lastSourceSequence ||
            current->lastSignalJson != mutation.nextState.lastSignalJson;
        if (cursor_changed) {
            if (mutation.nextState.protocolMode ==
                event::FireProtocolMode::Unseen) {
                executeSqlUnlocked("ROLLBACK;");
                return failure(event::FireStoreMutationOutcome::Invalid,
                               current->fireRevision,
                               "Fire protocol cannot return to unseen");
            }

            sensor::SensorSequenceState cursor;
            if (current->protocolMode == event::FireProtocolMode::Legacy) {
                cursor.mode = sensor::SensorSequenceMode::Legacy;
            } else if (current->protocolMode ==
                       event::FireProtocolMode::Versioned) {
                cursor.mode = sensor::SensorSequenceMode::Versioned;
            }
            cursor.activeBootId = current->activeBootId;
            cursor.lastSequence = current->lastSourceSequence;
            if (mutation.nextState.activeBootId) {
                FireStatement retired(db_,
                    "SELECT 1 FROM SENSOR_RETIRED_BOOT_ID WHERE "
                    "source_kind='FIRE' AND sensor_id=? AND boot_id=? "
                    "LIMIT 1;");
                retired.bindText(1, mutation.nextState.sensorId);
                retired.bindText(2, *mutation.nextState.activeBootId);
                const int retired_step = sqlite3_step(retired.get());
                if (retired_step != SQLITE_ROW &&
                    retired_step != SQLITE_DONE) {
                    throw std::runtime_error(
                        "Fire retired boot lookup failed");
                }
                if (retired_step == SQLITE_ROW) {
                    cursor.retiredBootIds.insert(
                        *mutation.nextState.activeBootId);
                }
            }
            const sensor::SensorSequenceFact fact{
                mutation.nextState.protocolMode ==
                        event::FireProtocolMode::Versioned
                    ? sensor::SensorProtocolVersion::BootEpochV2
                    : sensor::SensorProtocolVersion::LegacyV1,
                mutation.nextState.activeBootId,
                mutation.nextState.lastSourceSequence};
            const auto decision =
                sensor::evaluateSensorSequence(cursor, fact);
            if (!decision.accepted()) {
                executeSqlUnlocked("ROLLBACK;");
                return failure(event::FireStoreMutationOutcome::Invalid,
                               current->fireRevision,
                               decision.reason);
            }

            if (cursor.mode == sensor::SensorSequenceMode::Versioned &&
                cursor.activeBootId && fact.bootId &&
                *cursor.activeBootId != *fact.bootId) {
                FireStatement retire(db_,
                    "INSERT OR IGNORE INTO SENSOR_RETIRED_BOOT_ID("
                    "source_kind,sensor_id,boot_id) VALUES('FIRE',?,?);");
                retire.bindText(1, mutation.nextState.sensorId);
                retire.bindText(2, *cursor.activeBootId);
                requireDone(db_, retire.get());
            }
        }

        FireStatement update(db_,
            "UPDATE FIRE_ALARM_STATE SET desired_lifecycle=?,"
            "active_alarm_id=?,last_event_id=?,fire_revision=?,"
            "protocol_mode=?,active_boot_id=?,last_source_sequence=?,"
            "last_signal_json=?,updated_at=? "
            "WHERE channel_id=? AND fire_revision=?;");
        update.bindText(1, event::toString(mutation.nextState.desiredLifecycle));
        if (mutation.nextState.activeAlarmId.empty()) {
            update.bindOptionalText(2, std::nullopt);
        } else {
            update.bindText(2, mutation.nextState.activeAlarmId);
        }
        update.bindText(3, mutation.nextState.lastEventId);
        update.bindInt64(
            4, checkedInt64(mutation.nextState.fireRevision, "fire revision"));
        update.bindText(5, event::toString(mutation.nextState.protocolMode));
        update.bindOptionalText(6, mutation.nextState.activeBootId);
        if (mutation.nextState.lastSourceSequence) {
            update.bindText(
                7, std::to_string(*mutation.nextState.lastSourceSequence));
        } else {
            update.bindOptionalText(7, std::nullopt);
        }
        update.bindText(8, mutation.nextState.lastSignalJson);
        update.bindText(9, mutation.nextState.updatedAt);
        update.bindText(10, mutation.nextState.channelId);
        update.bindInt64(
            11, checkedInt64(mutation.expectedRevision, "expected revision"));
        requireDone(db_, update.get());
        if (sqlite3_changes(db_) != 1) {
            throw std::runtime_error("Fire state compare-and-swap changed no row");
        }

        if (transition) {
            const auto retained = selectDelivery(
                db_, mutation.retainedDelivery->deliveryKey);
            if (!retained ||
                retained->sinkKind !=
                    event::FireDeliverySinkKind::RetainedState ||
                retained->channelId != mutation.nextState.channelId ||
                retained->logicalKey !=
                    mutation.retainedDelivery->logicalKey) {
                throw std::runtime_error(
                    "Fire retained delivery identity is missing or changed");
            }
            FireStatement update_retained(db_,
                "UPDATE FIRE_MQTT_OUTBOX SET sensor_id=?,channel_id=?,"
                "event_id=?,alarm_id=?,fire_revision=?,topic=?,payload_json=?,"
                "qos=1,retain=1,attempt_count=0,next_attempt_at=?,"
                "last_error='',delivery_state='PENDING',updated_at=? "
                "WHERE delivery_key=?;");
            update_retained.bindText(1, mutation.retainedDelivery->sensorId);
            update_retained.bindText(2, mutation.retainedDelivery->channelId);
            update_retained.bindText(3, mutation.retainedDelivery->eventId);
            update_retained.bindText(4, mutation.retainedDelivery->alarmId);
            update_retained.bindInt64(
                5, checkedInt64(mutation.retainedDelivery->fireRevision,
                                "fire revision"));
            update_retained.bindText(6, mutation.retainedDelivery->topic);
            update_retained.bindText(7,
                                     mutation.retainedDelivery->payloadJson);
            update_retained.bindText(8,
                                     mutation.retainedDelivery->nextAttemptAt);
            update_retained.bindText(9,
                                     mutation.retainedDelivery->updatedAt);
            update_retained.bindText(10,
                                     mutation.retainedDelivery->deliveryKey);
            requireDone(db_, update_retained.get());
            if (sqlite3_changes(db_) != 1) {
                throw std::runtime_error("Fire retained delivery update failed");
            }

            const auto existing_lifecycle = selectDelivery(
                db_, mutation.lifecycleDelivery->deliveryKey);
            if (existing_lifecycle) {
                if (!sameDeliverySemantic(*existing_lifecycle,
                                          *mutation.lifecycleDelivery)) {
                    throw std::runtime_error(
                        "same Fire lifecycle ID has different payload");
                }
            } else {
                insertDelivery(db_, *mutation.lifecycleDelivery);
            }
        }

        executeSqlUnlocked("COMMIT;");
        return {event::FireStoreMutationOutcome::Committed,
                mutation.nextState.fireRevision, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        util::logError("Fire state transaction failed: " +
                       std::string(error.what()));
        return failure(event::FireStoreMutationOutcome::Failed,
                       mutation.expectedRevision, error.what());
    }
}

std::optional<event::FireAlarmStateRecord> EventDatabase::getFireAlarmState(
    const std::string& channel_id) const {
    if (channel_id.empty()) return std::nullopt;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;
    return selectState(db_, channel_id);
}

std::vector<event::FireAlarmStateRecord> EventDatabase::listFireAlarmStates()
    const {
    std::lock_guard lock(db_mutex_);
    std::vector<event::FireAlarmStateRecord> rows;
    if (!opened_ || db_ == nullptr) return rows;
    const std::string sql = std::string("SELECT ") + kStateColumns +
        " FROM FIRE_ALARM_STATE ORDER BY channel_id;";
    FireStatement statement(db_, sql.c_str());
    int result{};
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        rows.push_back(readState(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite Fire state list failed: " +
            std::string(sqlite3_errmsg(db_)));
    }
    return rows;
}

std::optional<event::FireOutboxRecord> EventDatabase::getFireDelivery(
    const std::string& delivery_key) const {
    if (delivery_key.empty()) return std::nullopt;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;
    return selectDelivery(db_, delivery_key);
}

std::vector<event::FireOutboxRecord>
EventDatabase::listFireRetainedDeliveries() const {
    std::lock_guard lock(db_mutex_);
    std::vector<event::FireOutboxRecord> rows;
    if (!opened_ || db_ == nullptr) return rows;
    const std::string sql = std::string("SELECT ") + kDeliveryColumns +
        " FROM FIRE_MQTT_OUTBOX WHERE sink_kind='RETAINED_STATE' "
        "ORDER BY channel_id;";
    FireStatement statement(db_, sql.c_str());
    int result{};
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        rows.push_back(readDelivery(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite Fire retained list failed: " +
            std::string(sqlite3_errmsg(db_)));
    }
    return rows;
}

std::vector<event::FireOutboxRecord>
EventDatabase::listFireLifecycleDeliveries() const {
    std::lock_guard lock(db_mutex_);
    std::vector<event::FireOutboxRecord> rows;
    if (!opened_ || db_ == nullptr) return rows;
    const std::string sql = std::string("SELECT ") + kDeliveryColumns +
        " FROM FIRE_MQTT_OUTBOX WHERE sink_kind='LIFECYCLE_EVENT' "
        "ORDER BY fire_revision,created_at,delivery_key;";
    FireStatement statement(db_, sql.c_str());
    int result{};
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        rows.push_back(readDelivery(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite Fire lifecycle list failed: " +
            std::string(sqlite3_errmsg(db_)));
    }
    return rows;
}

std::vector<event::FireOutboxRecord>
EventDatabase::listPendingFireLifecycleDeliveries(
    const std::optional<std::string>& channel_id) const {
    std::lock_guard lock(db_mutex_);
    std::vector<event::FireOutboxRecord> rows;
    if (!opened_ || db_ == nullptr) return rows;
    std::string sql = std::string("SELECT ") + kDeliveryColumns +
        " FROM FIRE_MQTT_OUTBOX WHERE sink_kind='LIFECYCLE_EVENT' "
        "AND delivery_state!='ACKED'";
    if (channel_id) sql += " AND channel_id=?";
    sql += " ORDER BY fire_revision,created_at,delivery_key;";
    FireStatement statement(db_, sql.c_str());
    if (channel_id) statement.bindText(1, *channel_id);
    int result{};
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        rows.push_back(readDelivery(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite Fire pending lifecycle list failed: " +
            std::string(sqlite3_errmsg(db_)));
    }
    return rows;
}

bool EventDatabase::markFireDeliveryInFlight(
    const std::string& delivery_key, const std::uint64_t fire_revision) {
    if (delivery_key.empty() || fire_revision == 0) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    try {
        FireStatement statement(db_,
            "UPDATE FIRE_MQTT_OUTBOX SET delivery_state='IN_FLIGHT',"
            "attempt_count=attempt_count+1,last_error='',"
            "updated_at=CURRENT_TIMESTAMP "
            "WHERE delivery_key=? AND fire_revision=?;");
        statement.bindText(1, delivery_key);
        statement.bindInt64(2, checkedInt64(fire_revision, "fire revision"));
        requireDone(db_, statement.get());
        return sqlite3_changes(db_) == 1;
    } catch (...) {
        return false;
    }
}

bool EventDatabase::markFireDeliveryPending(
    const std::string& delivery_key, const std::uint64_t fire_revision,
    const std::string& error) {
    if (delivery_key.empty() || fire_revision == 0) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    try {
        FireStatement statement(db_,
            "UPDATE FIRE_MQTT_OUTBOX SET delivery_state='PENDING',"
            "last_error=?,next_attempt_at=CURRENT_TIMESTAMP,"
            "updated_at=CURRENT_TIMESTAMP "
            "WHERE delivery_key=? AND fire_revision=?;");
        statement.bindText(1, error);
        statement.bindText(2, delivery_key);
        statement.bindInt64(3, checkedInt64(fire_revision, "fire revision"));
        requireDone(db_, statement.get());
        return sqlite3_changes(db_) == 1;
    } catch (...) {
        return false;
    }
}

bool EventDatabase::acknowledgeFireDelivery(
    const std::string& delivery_key, const std::uint64_t fire_revision) {
    if (delivery_key.empty() || fire_revision == 0) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    try {
        FireStatement statement(db_,
            "UPDATE FIRE_MQTT_OUTBOX SET delivery_state='ACKED',"
            "acked_revision=?,last_error='',updated_at=CURRENT_TIMESTAMP "
            "WHERE delivery_key=? AND fire_revision=? "
            "AND delivery_state='IN_FLIGHT';");
        statement.bindInt64(1, checkedInt64(fire_revision, "fire revision"));
        statement.bindText(2, delivery_key);
        statement.bindInt64(3, checkedInt64(fire_revision, "fire revision"));
        requireDone(db_, statement.get());
        return sqlite3_changes(db_) == 1;
    } catch (...) {
        return false;
    }
}

bool EventDatabase::resetFireInFlightDeliveries() {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    try {
        FireStatement statement(db_,
            "UPDATE FIRE_MQTT_OUTBOX SET delivery_state='PENDING',"
            "last_error='process restart before PUBACK',"
            "next_attempt_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP "
            "WHERE delivery_state='IN_FLIGHT';");
        requireDone(db_, statement.get());
        return true;
    } catch (...) {
        return false;
    }
}

bool EventDatabase::isSensorBootIdRetired(
    const std::string& source_kind,
    const std::string& sensor_id,
    const std::string& boot_id) const {
    if ((source_kind != "HALL" && source_kind != "FIRE") ||
        sensor_id.empty() || boot_id.empty()) {
        return true;
    }
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    FireStatement statement(db_,
        "SELECT 1 FROM SENSOR_RETIRED_BOOT_ID WHERE source_kind=? "
        "AND sensor_id=? AND boot_id=? LIMIT 1;");
    statement.bindText(1, source_kind);
    statement.bindText(2, sensor_id);
    statement.bindText(3, boot_id);
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        throw std::runtime_error("retired sensor boot lookup failed");
    }
    return step == SQLITE_ROW;
}

}  // namespace database

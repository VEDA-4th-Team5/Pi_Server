#include "database/EventDatabase.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace database {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const std::string_view sql)
        : database_(database) {
        const int result = sqlite3_prepare_v2(
            database_, sql.data(), static_cast<int>(sql.size()),
            &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error(
                "SQLite correlation prepare failed: " +
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
            throw std::runtime_error(
                "SQLite correlation text bind failed: " +
                std::string(sqlite3_errmsg(database_)));
        }
    }

    void integer(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error(
                "SQLite correlation integer bind failed: " +
                std::string(sqlite3_errmsg(database_)));
        }
    }

    void null(const int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error(
                "SQLite correlation null bind failed: " +
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

void done(sqlite3* database, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite correlation statement failed: " +
            std::string(sqlite3_errmsg(database)));
    }
}

const char* evidenceKindName(const parking::BestShotEvidenceKind kind) {
    switch (kind) {
    case parking::BestShotEvidenceKind::Vehicle:
        return "BESTSHOT_VEHICLE";
    case parking::BestShotEvidenceKind::Plate:
        return "BESTSHOT_PLATE";
    }
    return "BESTSHOT_UNKNOWN";
}

parking::BestShotAttachResult attachResult(
    const parking::BestShotAttachCode code,
    const std::string_view message,
    const std::int64_t image_id = -1) {
    return {code, image_id, std::string(message)};
}

struct CorrelationCandidate {
    bool committed{};
    parking::CommittedCorrelationLease lease;
};

}  // namespace

parking::ParkingCorrelationMatch EventDatabase::resolveParkingCorrelation(
    const std::string& camera_id,
    const std::string& channel_id,
    const std::string& object_id,
    const std::int64_t now_epoch_ms) const {
    parking::ParkingCorrelationMatch result;
    if (camera_id.empty() || channel_id.empty() || object_id.empty()) {
        return result;
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || !occupancy_schema_ready_) {
        return result;
    }

    // The map intentionally deduplicates one correlation transitioning from
    // the durable inbox to the committed binding table. Its ordered key also
    // makes Ambiguous diagnostics independent of SQLite insertion order.
    std::map<std::string, CorrelationCandidate> candidates;

    Statement pending(
        db_,
        "WITH normalized AS ("
        "SELECT correlation_id,CASE WHEN json_valid(payload_json) "
        "THEN payload_json ELSE '{}' END AS payload "
        "FROM OCCUPANCY_COMMAND_INBOX "
        "WHERE source_kind='CAMERA_OBSERVATION' "
        "AND status IN ('PENDING_UNPREPARED','PENDING_PREPARED') "
        "AND correlation_id!='') "
        "SELECT correlation_id FROM normalized WHERE "
        "json_extract(payload,'$.action')='INTRUSION' AND "
        "json_extract(payload,'$.camera_id')=? AND "
        "json_extract(payload,'$.channel_id')=? AND "
        "json_extract(payload,'$.object_id')=? AND "
        "CAST(COALESCE("
        "json_extract(payload,'$.correlation_expires_at_epoch_ms'),"
        "json_extract(payload,'$.expires_at_epoch_ms'),0) AS INTEGER)>? "
        "ORDER BY correlation_id;");
    pending.text(1, camera_id);
    pending.text(2, channel_id);
    pending.text(3, object_id);
    pending.integer(4, now_epoch_ms);
    for (;;) {
        const int step = sqlite3_step(pending.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            throw std::runtime_error(
                "SQLite pending correlation lookup failed: " +
                std::string(sqlite3_errmsg(db_)));
        }
        const std::string correlation_id = columnText(pending.get(), 0);
        candidates.try_emplace(
            correlation_id,
            CorrelationCandidate{false, {}});
    }

    Statement committed(
        db_,
        "SELECT correlation_id,occupancy_attempt_id,session_id,slot_id,"
        "camera_id,video_source_token,rule_name,object_id,channel_id,"
        "binding_revision,expires_at_epoch_ms "
        "FROM PARKING_CORRELATION_BINDING WHERE state='COMMITTED' "
        "AND camera_id=? AND channel_id=? AND object_id=? "
        "AND expires_at_epoch_ms>? ORDER BY correlation_id;");
    committed.text(1, camera_id);
    committed.text(2, channel_id);
    committed.text(3, object_id);
    committed.integer(4, now_epoch_ms);
    for (;;) {
        const int step = sqlite3_step(committed.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            throw std::runtime_error(
                "SQLite committed correlation lookup failed: " +
                std::string(sqlite3_errmsg(db_)));
        }
        parking::CommittedCorrelationLease lease;
        lease.correlationId = columnText(committed.get(), 0);
        lease.occupancyAttemptId = columnText(committed.get(), 1);
        lease.sessionId = sqlite3_column_int64(committed.get(), 2);
        lease.slotId = columnText(committed.get(), 3);
        lease.cameraId = columnText(committed.get(), 4);
        lease.videoSourceToken = columnText(committed.get(), 5);
        lease.ruleName = columnText(committed.get(), 6);
        lease.objectId = columnText(committed.get(), 7);
        lease.channelId = columnText(committed.get(), 8);
        lease.bindingRevision = static_cast<std::uint64_t>(
            sqlite3_column_int64(committed.get(), 9));
        lease.expiresAtEpochMs = sqlite3_column_int64(committed.get(), 10);
        const std::string correlation_id = lease.correlationId;
        candidates[correlation_id] = {true, std::move(lease)};
    }

    result.candidateCorrelationIds.reserve(candidates.size());
    for (const auto& [correlation_id, candidate] : candidates) {
        (void)candidate;
        result.candidateCorrelationIds.push_back(correlation_id);
    }
    if (candidates.empty()) return result;
    if (candidates.size() > 1) {
        result.kind = parking::ParkingCorrelationMatchKind::Ambiguous;
        return result;
    }

    const auto& candidate = candidates.begin()->second;
    if (!candidate.committed) {
        result.kind = parking::ParkingCorrelationMatchKind::Pending;
        return result;
    }
    result.kind = parking::ParkingCorrelationMatchKind::Unique;
    result.lease = candidate.lease;
    return result;
}

parking::BestShotAttachResult EventDatabase::attachBestShotIfActive(
    const parking::CommittedCorrelationLease& lease,
    const parking::BestShotEvidenceKind kind,
    const std::string& image_ref,
    const std::string& image_path,
    const std::string& plate_text,
    const std::int64_t now_epoch_ms) {
    if (lease.correlationId.empty() || lease.occupancyAttemptId.empty() ||
        lease.sessionId < 0 || lease.slotId.empty() || lease.cameraId.empty() ||
        lease.videoSourceToken.empty() || lease.ruleName.empty() ||
        lease.objectId.empty() || lease.channelId.empty() ||
        lease.bindingRevision == 0 || image_ref.empty() || image_path.empty() ||
        lease.bindingRevision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return attachResult(
            parking::BestShotAttachCode::Conflict,
            "BestShot lease or evidence identity is incomplete");
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || !occupancy_schema_ready_) {
        return attachResult(
            parking::BestShotAttachCode::RetryableFailure,
            "parking correlation database is not ready");
    }

    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement binding(
            db_,
            "SELECT occupancy_attempt_id,COALESCE(session_id,-1),slot_id,"
            "camera_id,video_source_token,rule_name,object_id,channel_id,"
            "binding_revision,state,expires_at_epoch_ms "
            "FROM PARKING_CORRELATION_BINDING WHERE correlation_id=? LIMIT 1;");
        binding.text(1, lease.correlationId);
        const int binding_step = sqlite3_step(binding.get());
        if (binding_step == SQLITE_DONE) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Unmatched,
                "committed correlation does not exist");
        }
        if (binding_step != SQLITE_ROW) {
            throw std::runtime_error("SQLite correlation binding lookup failed");
        }

        const std::string stored_attempt = columnText(binding.get(), 0);
        const std::int64_t stored_session =
            sqlite3_column_int64(binding.get(), 1);
        const std::string stored_slot = columnText(binding.get(), 2);
        const std::string stored_camera = columnText(binding.get(), 3);
        const std::string stored_token = columnText(binding.get(), 4);
        const std::string stored_rule = columnText(binding.get(), 5);
        const std::string stored_object = columnText(binding.get(), 6);
        const std::string stored_channel = columnText(binding.get(), 7);
        const std::int64_t stored_revision =
            sqlite3_column_int64(binding.get(), 8);
        const std::string stored_state = columnText(binding.get(), 9);
        const std::int64_t stored_expiry =
            sqlite3_column_int64(binding.get(), 10);

        const bool lease_matches =
            stored_attempt == lease.occupancyAttemptId &&
            stored_session == lease.sessionId &&
            stored_slot == lease.slotId &&
            stored_camera == lease.cameraId &&
            stored_token == lease.videoSourceToken &&
            stored_rule == lease.ruleName &&
            stored_object == lease.objectId &&
            stored_channel == lease.channelId &&
            stored_revision == static_cast<std::int64_t>(lease.bindingRevision) &&
            stored_expiry == lease.expiresAtEpochMs;
        if (!lease_matches) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Conflict,
                "committed correlation lease is stale or mismatched");
        }

        const std::string kind_name = evidenceKindName(kind);
        Statement existing(
            db_,
            "SELECT image_id,COALESCE(session_id,-1),"
            "COALESCE(original_image_path,''),COALESCE(camera_object_id,''),"
            "COALESCE(correlation_binding_revision,-1),"
            "COALESCE(ocr_result,'') FROM IMAGE_LOG "
            "WHERE correlation_id=? AND enhancement_type=? AND image_ref=? "
            "LIMIT 1;");
        existing.text(1, lease.correlationId);
        existing.text(2, kind_name);
        existing.text(3, image_ref);
        const int existing_step = sqlite3_step(existing.get());
        if (existing_step == SQLITE_ROW) {
            const auto image_id = sqlite3_column_int64(existing.get(), 0);
            const bool canonical =
                sqlite3_column_int64(existing.get(), 1) == lease.sessionId &&
                columnText(existing.get(), 2) == image_path &&
                columnText(existing.get(), 3) == lease.objectId &&
                sqlite3_column_int64(existing.get(), 4) ==
                    static_cast<std::int64_t>(lease.bindingRevision) &&
                columnText(existing.get(), 5) == plate_text;
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                canonical ? parking::BestShotAttachCode::AlreadyAttached
                          : parking::BestShotAttachCode::Conflict,
                canonical ? "BestShot evidence was already attached"
                          : "BestShot evidence identity conflicts with its "
                            "canonical row",
                canonical ? image_id : -1);
        }
        if (existing_step != SQLITE_DONE) {
            throw std::runtime_error("SQLite BestShot identity lookup failed");
        }

        if (stored_state == "EXPIRED" || stored_expiry <= now_epoch_ms) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Expired,
                "committed correlation has expired");
        }
        if (stored_state != "COMMITTED") {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                stored_state == "ENDED"
                    ? parking::BestShotAttachCode::Inactive
                    : parking::BestShotAttachCode::Unmatched,
                "correlation is not attachable");
        }

        Statement session(
            db_,
            "SELECT slot_id,COALESCE(occupancy_attempt_id,''),status,exit_time "
            "FROM PARKING_SESSION WHERE session_id=? LIMIT 1;");
        session.integer(1, lease.sessionId);
        const int session_step = sqlite3_step(session.get());
        if (session_step == SQLITE_DONE) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Inactive,
                "bound parking session does not exist");
        }
        if (session_step != SQLITE_ROW) {
            throw std::runtime_error("SQLite bound session lookup failed");
        }
        const std::string session_slot = columnText(session.get(), 0);
        const std::string session_attempt = columnText(session.get(), 1);
        const std::string session_state = columnText(session.get(), 2);
        const bool session_ended =
            sqlite3_column_type(session.get(), 3) != SQLITE_NULL;
        if (session_slot != lease.slotId ||
            session_attempt != lease.occupancyAttemptId) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Conflict,
                "binding and parking session identity disagree");
        }
        if (session_ended ||
            (session_state != "ACTIVE" && session_state != "VIOLATION")) {
            executeSqlUnlocked("COMMIT;");
            return attachResult(
                parking::BestShotAttachCode::Inactive,
                "bound parking session is no longer active");
        }

        Statement insert(
            db_,
            "INSERT INTO IMAGE_LOG(session_id,original_image_path,"
            "enhanced_image_path,enhancement_type,evidence_reason,ocr_result,"
            "captured_at,correlation_id,camera_object_id,image_ref,"
            "correlation_binding_revision) VALUES(?,?,NULL,?,NULL,?,"
            "strftime('%Y-%m-%dT%H:%M:%fZ',?/1000.0,'unixepoch'),?,?,?,?);");
        insert.integer(1, lease.sessionId);
        insert.text(2, image_path);
        insert.text(3, kind_name);
        if (plate_text.empty())
            insert.null(4);
        else
            insert.text(4, plate_text);
        insert.integer(5, now_epoch_ms);
        insert.text(6, lease.correlationId);
        insert.text(7, lease.objectId);
        insert.text(8, image_ref);
        insert.integer(9, static_cast<std::int64_t>(lease.bindingRevision));
        done(db_, insert.get());
        const auto image_id = sqlite3_last_insert_rowid(db_);
        executeSqlUnlocked("COMMIT;");
        return attachResult(
            parking::BestShotAttachCode::Attached,
            "BestShot evidence attached to active exact session",
            image_id);
    } catch (const std::exception& error) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return attachResult(
            parking::BestShotAttachCode::RetryableFailure, error.what());
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return attachResult(
            parking::BestShotAttachCode::RetryableFailure,
            "unknown SQLite BestShot attachment failure");
    }
}

}  // namespace database

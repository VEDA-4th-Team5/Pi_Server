#include "database/EventDatabase.hpp"

#include "ocr/PlateMatcher.hpp"
#include "util/Logger.hpp"

#include <sqlite3.h>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }
    void text(const int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(), -1,
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }
    void integer(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }
    void real(const int index, const double value) {
        if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }
    void null(const int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }
private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

void requireDone(sqlite3* database, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(database));
}

std::string columnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{}
                            : reinterpret_cast<const char*>(value);
}

struct EntranceCandidate {
    std::int64_t eventId{-1};
    std::int64_t vehicleId{-1};
    std::string plate;
    bool isEv{};
    double confidence{};
    std::int64_t firstSeenEpochMs{};
    ocr::PlateSimilarity similarity;
};

}  // namespace

namespace database {

std::int64_t EventDatabase::createEntranceRecognition(
    const std::string& camera_id, const std::string& channel_id,
    const std::string& object_id, const std::int64_t first_seen_epoch_ms) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || camera_id.empty() || channel_id.empty() ||
        object_id.empty()) return -1;
    try {
        Statement insert(
            db_, "INSERT INTO ENTRANCE_RECOGNITION("
                 "camera_id,channel_id,object_id,state,first_seen_epoch_ms,"
                 "updated_at_epoch_ms) VALUES(?,?,?,'COLLECTING',?,?);");
        insert.text(1, camera_id);
        insert.text(2, channel_id);
        insert.text(3, object_id);
        insert.integer(4, first_seen_epoch_ms);
        insert.integer(5, first_seen_epoch_ms);
        requireDone(db_, insert.get());
        return sqlite3_last_insert_rowid(db_);
    } catch (const std::exception& error) {
        util::logError("Entrance DB create failed: " +
                       std::string(error.what()));
        return -1;
    }
}

bool EventDatabase::updateEntranceImage(const std::int64_t event_id,
                                        const bool plate,
                                        const std::string& image_path) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || event_id < 0 || image_path.empty())
        return false;
    try {
        const char* sql = plate
            ? "UPDATE ENTRANCE_RECOGNITION SET plate_image_path=?,"
              "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
              "WHERE entrance_event_id=?;"
            : "UPDATE ENTRANCE_RECOGNITION SET vehicle_image_path=?,"
              "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
              "WHERE entrance_event_id=?;";
        Statement update(db_, sql);
        update.text(1, image_path);
        update.integer(2, event_id);
        requireDone(db_, update.get());
        return sqlite3_changes(db_) == 1;
    } catch (const std::exception& error) {
        util::logError("Entrance DB image update failed: " +
                       std::string(error.what()));
        return false;
    }
}

bool EventDatabase::updateEntranceVisionAnalysis(
    const std::int64_t event_id, const EntranceVisionAnalysis& analysis) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || event_id < 0 ||
        analysis.processing_ms < 0.0) return false;
    const bool validDecision =
        (analysis.decision == "EV_CANDIDATE" && analysis.is_ev == true) ||
        (analysis.decision == "NON_EV_CANDIDATE" && analysis.is_ev == false) ||
        (analysis.decision == "REVIEW" && !analysis.is_ev.has_value());
    if (!validDecision) return false;
    try {
        sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        Statement update(
            db_, "UPDATE ENTRANCE_RECOGNITION SET state='OCR_QUEUED',"
                 "vision_is_ev=?,"
                 "vision_decision=?,vision_reason=?,vision_model_version=?,"
                 "vision_processing_ms=?,vision_result_path=?,vision_error=?,"
                 "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                 "WHERE entrance_event_id=?;");
        if (analysis.is_ev.has_value())
            update.integer(1, *analysis.is_ev ? 1 : 0);
        else
            update.null(1);
        update.text(2, analysis.decision);
        update.text(3, analysis.reason);
        update.text(4, analysis.model_version);
        update.real(5, analysis.processing_ms);
        if (analysis.result_path.empty()) update.null(6);
        else update.text(6, analysis.result_path);
        update.text(7, analysis.error);
        update.integer(8, event_id);
        requireDone(db_, update.get());
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("entrance event row was not found");
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        return true;
    } catch (const std::exception& failure) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        util::logError("Entrance DB vision update failed: " +
                       std::string(failure.what()));
        return false;
    }
}

std::string EventDatabase::finishEntranceRecognition(
    const std::int64_t event_id, const std::string& plate_number,
    const double confidence, const int attempts, const std::string& error) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || event_id < 0) return "DB_ERROR";
    try {
        sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        if (plate_number.empty()) {
            Statement failed(
                db_, "UPDATE ENTRANCE_RECOGNITION SET state='FAILED',"
                     "confidence=?,ocr_attempts=?,last_error=?,"
                     "artifact_state='RETAINED_FAILURE',"
                     "completed_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000,"
                     "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                     "WHERE entrance_event_id=?;");
            failed.real(1, confidence);
            failed.integer(2, attempts);
            failed.text(3, error);
            failed.integer(4, event_id);
            requireDone(db_, failed.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error("entrance event row was not found");
            sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
            return "OCR_FAILED";
        }

        std::optional<bool> isEv;
        {
            Statement analysis(
                db_, "SELECT vision_is_ev FROM ENTRANCE_RECOGNITION "
                     "WHERE entrance_event_id=?;");
            analysis.integer(1, event_id);
            if (sqlite3_step(analysis.get()) != SQLITE_ROW)
                throw std::runtime_error("entrance event row was not found");
            if (sqlite3_column_type(analysis.get(), 0) != SQLITE_NULL)
                isEv = sqlite3_column_int(analysis.get(), 0) != 0;
        }
        if (!isEv.has_value()) {
            Statement failed(
                db_, "UPDATE ENTRANCE_RECOGNITION SET state='FAILED',"
                     "plate_number=?,confidence=?,ocr_attempts=?,"
                     "last_error='EV icon decision unavailable',"
                     "artifact_state='RETAINED_FAILURE',"
                     "completed_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000,"
                     "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                     "WHERE entrance_event_id=?;");
            failed.text(1, plate_number);
            failed.real(2, confidence);
            failed.integer(3, attempts);
            failed.integer(4, event_id);
            requireDone(db_, failed.get());
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error("entrance event row was not found");
            sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
            return "EV_FAILED";
        }

        const std::string classification = *isEv ? "EV" : "NON_EV";
        Statement upsertVehicle(
            db_, "INSERT INTO VEHICLE(plate_number,is_ev) VALUES(?,?) "
                 "ON CONFLICT(plate_number) DO UPDATE SET is_ev=excluded.is_ev;");
        upsertVehicle.text(1, plate_number);
        upsertVehicle.integer(2, *isEv ? 1 : 0);
        requireDone(db_, upsertVehicle.get());

        std::int64_t resolvedVehicleId{};
        {
            Statement vehicleId(
                db_, "SELECT vehicle_id FROM VEHICLE WHERE plate_number=?;");
            vehicleId.text(1, plate_number);
            if (sqlite3_step(vehicleId.get()) != SQLITE_ROW)
                throw std::runtime_error("entrance vehicle upsert was not found");
            resolvedVehicleId = sqlite3_column_int64(vehicleId.get(), 0);
        }

        Statement update(
            db_, "UPDATE ENTRANCE_RECOGNITION SET state='COMPLETED',"
                 "vehicle_id=?,plate_number=?,classification=?,"
                 "registered_is_ev=?,resolved_is_ev=?,decision_source='VISION',"
                 "artifact_state='DELETE_PENDING',"
                 "confidence=?,"
                 "ocr_attempts=?,last_error=?,"
                 "completed_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000,"
                 "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                 "WHERE entrance_event_id=?;");
        update.integer(1, resolvedVehicleId);
        update.text(2, plate_number);
        update.text(3, classification);
        update.integer(4, *isEv ? 1 : 0);
        update.integer(5, *isEv ? 1 : 0);
        update.real(6, confidence);
        update.integer(7, attempts);
        update.text(8, error);
        update.integer(9, event_id);
        requireDone(db_, update.get());
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("entrance event row was not found");
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        return classification;
    } catch (const std::exception& failure) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        util::logError("Entrance DB completion failed: " +
                       std::string(failure.what()));
        return "DB_ERROR";
    }
}

ParkingPlateResolution EventDatabase::applyPlateOcrWithEntrance(
    const int session_id, const std::string& slot_id,
    const std::string& image_path, const std::string& plate_number,
    const double confidence, const std::int64_t entrance_match_window_ms,
    const double entrance_min_confidence) {
    ParkingPlateResolution result;
    result.observed_plate = plate_number;
    result.canonical_plate = plate_number;
    if (session_id < 0 || plate_number.empty() ||
        entrance_match_window_ms <= 0 || entrance_min_confidence < 0.0 ||
        entrance_min_confidence > 1.0) {
        return result;
    }

    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return result;
    try {
        sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

        if (!image_path.empty()) {
            Statement image(
                db_, "UPDATE IMAGE_LOG SET ocr_result=? "
                     "WHERE original_image_path=?;");
            image.text(1, plate_number);
            image.text(2, image_path);
            requireDone(db_, image.get());
        }

        std::int64_t sessionEntryEpochMs{};
        std::int64_t existingEntranceEventId{-1};
        std::string existingResolutionSource{"UNRESOLVED"};
        double existingMatchScore{};
        {
            Statement session(
                db_, "SELECT COALESCE(entry_time_epoch_ms,"
                     "CAST((julianday(entry_time)-2440587.5)*86400000 AS INTEGER)),"
                     "COALESCE(entrance_event_id,-1),plate_resolution_source,"
                     "COALESCE(plate_match_score,0.0) "
                     "FROM PARKING_SESSION WHERE session_id=? "
                     "AND exit_time IS NULL;");
            session.integer(1, session_id);
            if (sqlite3_step(session.get()) != SQLITE_ROW)
                throw std::runtime_error("active parking session was not found");
            sessionEntryEpochMs = sqlite3_column_int64(session.get(), 0);
            existingEntranceEventId = sqlite3_column_int64(session.get(), 1);
            existingResolutionSource = columnText(session.get(), 2);
            existingMatchScore = sqlite3_column_double(session.get(), 3);
        }

        std::optional<EntranceCandidate> selected;
        if (existingEntranceEventId >= 0) {
            Statement existing(
                db_, "SELECT entrance_event_id,vehicle_id,plate_number,"
                     "resolved_is_ev,confidence,first_seen_epoch_ms "
                     "FROM ENTRANCE_RECOGNITION WHERE entrance_event_id=? "
                     "AND state='COMPLETED';");
            existing.integer(1, existingEntranceEventId);
            if (sqlite3_step(existing.get()) == SQLITE_ROW) {
                EntranceCandidate candidate;
                candidate.eventId = sqlite3_column_int64(existing.get(), 0);
                candidate.vehicleId = sqlite3_column_int64(existing.get(), 1);
                candidate.plate = columnText(existing.get(), 2);
                candidate.isEv = sqlite3_column_int(existing.get(), 3) != 0;
                candidate.confidence = sqlite3_column_double(existing.get(), 4);
                candidate.firstSeenEpochMs =
                    sqlite3_column_int64(existing.get(), 5);
                candidate.similarity =
                    ocr::comparePlateNumbers(plate_number, candidate.plate);
                selected = std::move(candidate);
            }
        } else {
            Statement candidates(
                db_, "SELECT e.entrance_event_id,e.vehicle_id,e.plate_number,"
                     "e.resolved_is_ev,e.confidence,e.first_seen_epoch_ms "
                     "FROM ENTRANCE_RECOGNITION e "
                     "WHERE e.state='COMPLETED' AND e.vehicle_id IS NOT NULL "
                     "AND e.resolved_is_ev IS NOT NULL AND e.confidence>=? "
                     "AND e.first_seen_epoch_ms BETWEEN ? AND ? "
                     "AND NOT EXISTS (SELECT 1 FROM PARKING_SESSION p "
                     "WHERE p.entrance_event_id=e.entrance_event_id "
                     "AND p.session_id<>?) "
                     "ORDER BY e.first_seen_epoch_ms DESC;");
            candidates.real(1, entrance_min_confidence);
            candidates.integer(2,
                               sessionEntryEpochMs - entrance_match_window_ms);
            candidates.integer(3, sessionEntryEpochMs);
            candidates.integer(4, session_id);

            bool ambiguous{};
            while (sqlite3_step(candidates.get()) == SQLITE_ROW) {
                EntranceCandidate candidate;
                candidate.eventId = sqlite3_column_int64(candidates.get(), 0);
                candidate.vehicleId = sqlite3_column_int64(candidates.get(), 1);
                candidate.plate = columnText(candidates.get(), 2);
                candidate.isEv = sqlite3_column_int(candidates.get(), 3) != 0;
                candidate.confidence = sqlite3_column_double(candidates.get(), 4);
                candidate.firstSeenEpochMs =
                    sqlite3_column_int64(candidates.get(), 5);
                candidate.similarity =
                    ocr::comparePlateNumbers(plate_number, candidate.plate);
                if (!candidate.similarity.comparable) continue;

                if (!selected.has_value() ||
                    candidate.similarity.score >
                        selected->similarity.score + 0.000001) {
                    selected = candidate;
                    ambiguous = false;
                    continue;
                }
                if (std::abs(candidate.similarity.score -
                             selected->similarity.score) > 0.000001) {
                    continue;
                }
                if (candidate.vehicleId == selected->vehicleId &&
                    candidate.plate == selected->plate) {
                    if (candidate.firstSeenEpochMs >
                        selected->firstSeenEpochMs) {
                        selected = candidate;
                    }
                    continue;
                }
                const double confidenceGap =
                    candidate.confidence - selected->confidence;
                if (std::abs(confidenceGap) >= 0.05) {
                    if (confidenceGap > 0.0) selected = candidate;
                    ambiguous = false;
                } else {
                    ambiguous = true;
                }
            }
            if (ambiguous) selected.reset();
        }

        if (selected.has_value()) {
            result.entrance_event_id = selected->eventId;
            result.vehicle_id = selected->vehicleId;
            result.canonical_plate = selected->plate;
            const bool keepsExistingResolution = existingEntranceEventId >= 0 &&
                (existingResolutionSource == "ENTRANCE_EXACT" ||
                 existingResolutionSource == "ENTRANCE_FUZZY");
            result.match_score = keepsExistingResolution
                ? existingMatchScore
                : (selected->similarity.comparable
                       ? selected->similarity.score : 1.0);
            result.source = keepsExistingResolution
                ? existingResolutionSource
                : (selected->similarity.exact
                       ? "ENTRANCE_EXACT" : "ENTRANCE_FUZZY");
            result.classification = selected->isEv ? "EV" : "NON_EV";
        } else {
            Statement vehicle(
                db_, "SELECT vehicle_id,is_ev FROM VEHICLE "
                     "WHERE plate_number=?;");
            vehicle.text(1, plate_number);
            if (sqlite3_step(vehicle.get()) == SQLITE_ROW) {
                result.vehicle_id = sqlite3_column_int64(vehicle.get(), 0);
                result.classification =
                    sqlite3_column_int(vehicle.get(), 1) != 0 ? "EV" : "NON_EV";
                result.source = "VEHICLE_EXACT";
                result.match_score = 1.0;
            } else {
                result.classification = "UNKNOWN";
                result.source = "UNRESOLVED";
                result.match_score = 0.0;
            }
        }

        Statement update(
            db_, "UPDATE PARKING_SESSION SET vehicle_id=?,plate_number=?,"
                 "parking_ocr_plate=?,parking_ocr_confidence=?,"
                 "entrance_event_id=?,plate_match_score=?,"
                 "plate_resolution_source=? WHERE session_id=? "
                 "AND exit_time IS NULL;");
        if (result.vehicle_id >= 0) update.integer(1, result.vehicle_id);
        else update.null(1);
        update.text(2, result.canonical_plate);
        update.text(3, result.observed_plate);
        update.real(4, confidence);
        if (result.entrance_event_id >= 0)
            update.integer(5, result.entrance_event_id);
        else
            update.null(5);
        update.real(6, result.match_score);
        update.text(7, result.source);
        update.integer(8, session_id);
        requireDone(db_, update.get());
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("parking session OCR update was not applied");

        Statement event(
            db_, "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
                 "VALUES(?,?,?,?);");
        event.integer(1, session_id);
        if (slot_id.empty()) event.null(2);
        else event.text(2, slot_id);
        event.text(3, "PLATE_OCR_" + result.classification);
        event.text(4, "observed=" + result.observed_plate +
                          " canonical=" + result.canonical_plate +
                          " classification=" + result.classification +
                          " source=" + result.source +
                          " score=" + std::to_string(result.match_score) +
                          " confidence=" + std::to_string(confidence) +
                          " image=" + image_path);
        requireDone(db_, event.get());

        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        result.persisted = true;
        return result;
    } catch (const std::exception& failure) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        util::logError("Parking plate correlation failed: " +
                       std::string(failure.what()));
        result.classification = "DB_ERROR";
        return result;
    }
}

bool EventDatabase::incrementEntranceDuplicateCount(
    const std::int64_t event_id) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || event_id < 0) return false;
    try {
        Statement update(
            db_, "UPDATE ENTRANCE_RECOGNITION SET "
                 "duplicate_count=duplicate_count+1,"
                 "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                 "WHERE entrance_event_id=?;");
        update.integer(1, event_id);
        requireDone(db_, update.get());
        return sqlite3_changes(db_) == 1;
    } catch (const std::exception& error) {
        util::logError("Entrance duplicate count update failed: " +
                       std::string(error.what()));
        return false;
    }
}

bool EventDatabase::markEntranceArtifactsDeleted(
    const std::int64_t event_id) {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr || event_id < 0) return false;
    try {
        Statement update(
            db_, "UPDATE ENTRANCE_RECOGNITION SET "
                 "state=CASE WHEN state IN ('COLLECTING','OCR_QUEUED',"
                 "'OCR_PROCESSING') THEN 'FAILED' ELSE state END,"
                 "last_error=CASE WHEN state IN ('COLLECTING','OCR_QUEUED',"
                 "'OCR_PROCESSING') AND last_error='' "
                 "THEN 'stale entrance artifacts cleaned after retention' "
                 "ELSE last_error END,"
                 "completed_at_epoch_ms=COALESCE(completed_at_epoch_ms,"
                 "CAST(strftime('%s','now') AS INTEGER)*1000),"
                 "vehicle_image_path=NULL,"
                 "plate_image_path=NULL,vision_result_path=NULL,"
                 "artifact_state='DELETED',"
                 "artifacts_deleted_at_epoch_ms="
                 "CAST(strftime('%s','now') AS INTEGER)*1000,"
                 "updated_at_epoch_ms=CAST(strftime('%s','now') AS INTEGER)*1000 "
                 "WHERE entrance_event_id=?;");
        update.integer(1, event_id);
        requireDone(db_, update.get());
        return sqlite3_changes(db_) == 1;
    } catch (const std::exception& error) {
        util::logError("Entrance artifact DB cleanup failed: " +
                       std::string(error.what()));
        return false;
    }
}

std::vector<EntranceArtifactRecord>
EventDatabase::listEntranceArtifactsForCleanup(
    const std::int64_t failed_before_epoch_ms) const {
    std::lock_guard lock(db_mutex_);
    std::vector<EntranceArtifactRecord> records;
    if (!opened_ || db_ == nullptr) return records;
    try {
        Statement query(
            db_, "SELECT entrance_event_id,state,"
                 "COALESCE(plate_image_path,''),"
                 "COALESCE(vision_result_path,'') FROM ENTRANCE_RECOGNITION "
                 "WHERE artifact_state='DELETE_PENDING' OR "
                 "(artifact_state='RETAINED_FAILURE' "
                 "AND completed_at_epoch_ms IS NOT NULL "
                 "AND completed_at_epoch_ms<=?) OR "
                 "(artifact_state='WORKING' AND updated_at_epoch_ms<=?) "
                 "ORDER BY entrance_event_id LIMIT 100;");
        query.integer(1, failed_before_epoch_ms);
        query.integer(2, failed_before_epoch_ms);
        while (sqlite3_step(query.get()) == SQLITE_ROW) {
            records.push_back({sqlite3_column_int64(query.get(), 0),
                               columnText(query.get(), 1),
                               columnText(query.get(), 2),
                               columnText(query.get(), 3)});
        }
    } catch (const std::exception& error) {
        util::logError("Entrance artifact cleanup query failed: " +
                       std::string(error.what()));
    }
    return records;
}

}  // namespace database

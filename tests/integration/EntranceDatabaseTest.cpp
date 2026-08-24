#include "database/EventDatabase.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        static std::atomic<unsigned long long> ordinal{};
        path = std::filesystem::temp_directory_path() /
               ("entrance-db-" + std::to_string(++ordinal) + ".sqlite3");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path;
};

std::int64_t rowCount(const std::filesystem::path& path) {
    sqlite3* connection{};
    if (sqlite3_open_v2(path.string().c_str(), &connection,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite probe open failed");
    sqlite3_stmt* statement{};
    if (sqlite3_prepare_v2(connection,
                          "SELECT COUNT(*) FROM ENTRANCE_RECOGNITION;", -1,
                          &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(connection);
        throw std::runtime_error("SQLite probe prepare failed");
    }
    const int step = sqlite3_step(statement);
    const auto count = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0)
                                          : -1;
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    return count;
}

void executeSql(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* connection{};
    if (sqlite3_open(path.string().c_str(), &connection) != SQLITE_OK)
        throw std::runtime_error("SQLite mutation open failed");
    char* error{};
    const int result = sqlite3_exec(connection, sql.c_str(), nullptr, nullptr,
                                    &error);
    const std::string message = error == nullptr ? "" : error;
    sqlite3_free(error);
    sqlite3_close(connection);
    if (result != SQLITE_OK)
        throw std::runtime_error("SQLite mutation failed: " + message);
}

int scalarInt(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* connection{};
    if (sqlite3_open_v2(path.string().c_str(), &connection,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite scalar open failed");
    sqlite3_stmt* statement{};
    if (sqlite3_prepare_v2(connection, sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(connection);
        throw std::runtime_error("SQLite scalar prepare failed");
    }
    const int value = sqlite3_step(statement) == SQLITE_ROW
        ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    return value;
}

std::string scalarText(const std::filesystem::path& path,
                       const std::string& sql) {
    sqlite3* connection{};
    if (sqlite3_open_v2(path.string().c_str(), &connection,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite text probe open failed");
    sqlite3_stmt* statement{};
    if (sqlite3_prepare_v2(connection, sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(connection);
        throw std::runtime_error("SQLite text probe prepare failed");
    }
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) value = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    return value;
}

void schemaAndLifecyclePersist() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");

    const auto first = database.createEntranceRecognition(
        "cam01", "ch02", "123", 1000);
    require(first > 0, "entrance event was not created");
    require(database.updateEntranceImage(first, true, "plate.jpg"),
            "plate image path was not stored");
    database::EntranceVisionAnalysis evAnalysis;
    evAnalysis.is_ev = true;
    evAnalysis.decision = "EV_CANDIDATE";
    evAnalysis.reason = "test-policy";
    evAnalysis.model_version = "test-v1";
    evAnalysis.processing_ms = 12.5;
    evAnalysis.result_path = "ev_analysis/result.json";
    require(database.updateEntranceVisionAnalysis(first, evAnalysis),
            "EV analysis was not stored");
    require(database.finishEntranceRecognition(
                first, "90가1234", 0.95, 1, {}) == "EV",
            "EV icon decision was not persisted");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM ENTRANCE_RECOGNITION e "
                      "JOIN VEHICLE v ON v.vehicle_id=e.vehicle_id WHERE "
                      "e.entrance_event_id=" + std::to_string(first) +
                      " AND e.plate_number='90가1234' AND v.plate_number='90가1234' "
                      "AND v.is_ev=1 AND e.registered_is_ev=1 AND "
                      "e.vision_is_ev=1 AND e.resolved_is_ev=1 AND "
                      "e.decision_source='VISION';") == 1,
            "plate and icon result were not linked to one vehicle row");
    require(database.incrementEntranceDuplicateCount(first),
            "entrance duplicate count was not incremented");
    require(database.listEntranceArtifactsForCleanup(INT64_MAX).size() == 1,
            "completed entrance artifacts were not scheduled for cleanup");
    require(database.markEntranceArtifactsDeleted(first),
            "entrance artifact deletion was not persisted");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM ENTRANCE_RECOGNITION WHERE "
                      "entrance_event_id=" + std::to_string(first) +
                      " AND artifact_state='DELETED' AND duplicate_count=1 "
                      "AND plate_image_path IS NULL AND vision_result_path IS NULL "
                      "AND plate_number='90가1234' AND resolved_is_ev=1 "
                      "AND vehicle_id IS NOT NULL AND confidence=0.95;") == 1,
            "artifact cleanup removed retained entrance recognition data");

    // ObjectId는 TTL 이후 재사용될 수 있으므로 DB에 영구 UNIQUE 제약을 두지 않는다.
    const auto reused = database.createEntranceRecognition(
        "cam01", "ch02", "123", 62000);
    require(reused > first, "reused ObjectId was incorrectly rejected");
    require(rowCount(temporary.path) == 2,
            "entrance lifecycle rows were not preserved independently");
}

void parkingOcrUsesRecentEntranceAsAuthoritativeSource() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");

    const auto entranceId = database.createEntranceRecognition(
        "cam01", "ch02", "entrance-object", 1'000'000);
    database::EntranceVisionAnalysis analysis;
    analysis.is_ev = true;
    analysis.decision = "EV_CANDIDATE";
    analysis.reason = "test-policy";
    analysis.model_version = "test-v1";
    analysis.processing_ms = 5.0;
    require(database.updateEntranceVisionAnalysis(entranceId, analysis),
            "entrance EV analysis was not stored for correlation");
    require(database.finishEntranceRecognition(
                entranceId, "294마3087", 0.99, 1, {}) == "EV",
            "entrance recognition was not completed for correlation");

    executeSql(
        temporary.path,
        "INSERT INTO PARKING_SESSION(session_id,slot_id,status,entry_time,"
        "entry_time_epoch_ms) VALUES(500,'EV02','ACTIVE',CURRENT_TIMESTAMP,"
        "1100000);"
        "INSERT INTO IMAGE_LOG(session_id,original_image_path) "
        "VALUES(500,'parking-observed.jpg');");

    const auto resolved = database.applyPlateOcrWithEntrance(
        500, "EV02", "parking-observed.jpg", "294미3087", 0.72,
        30 * 60 * 1000, 0.85);
    require(resolved.persisted && resolved.classification == "EV" &&
                resolved.source == "ENTRANCE_FUZZY" &&
                resolved.observed_plate == "294미3087" &&
                resolved.canonical_plate == "294마3087" &&
                resolved.entrance_event_id == entranceId,
            "parking OCR did not prefer the recent entrance recognition");
    require(scalarInt(
                temporary.path,
                "SELECT COUNT(*) FROM PARKING_SESSION WHERE session_id=500 "
                "AND parking_ocr_plate='294미3087' "
                "AND plate_number='294마3087' "
                "AND plate_resolution_source='ENTRANCE_FUZZY' "
                "AND entrance_event_id=" + std::to_string(entranceId) +
                " AND vehicle_id IS NOT NULL;") == 1,
            "parking session did not retain observed and canonical plates");
    require(scalarText(temporary.path,
                       "SELECT ocr_result FROM IMAGE_LOG WHERE session_id=500;") ==
                "294미3087",
            "parking IMAGE_LOG did not retain the raw OCR observation");

    const auto repeated = database.applyPlateOcrWithEntrance(
        500, "EV02", {}, "999가9999", 0.61, 30 * 60 * 1000, 0.85);
    require(repeated.persisted && repeated.canonical_plate == "294마3087" &&
                repeated.source == "ENTRANCE_FUZZY" &&
                repeated.entrance_event_id == entranceId,
            "later OCR replaced an already linked entrance authority");

    // 한 입구 통과 이벤트는 하나의 주차 세션에만 연결한다. 같은 오독값으로
    // 두 번째 세션이 들어와도 이미 소비된 입구 이벤트를 재사용하면 안 된다.
    executeSql(
        temporary.path,
        "INSERT INTO PARKING_SESSION(session_id,slot_id,status,entry_time,"
        "entry_time_epoch_ms) VALUES(501,'EV03','ACTIVE',CURRENT_TIMESTAMP,"
        "1200000);");
    const auto unlinked = database.applyPlateOcrWithEntrance(
        501, "EV03", {}, "294미3087", 0.70, 30 * 60 * 1000, 0.85);
    require(unlinked.persisted && unlinked.classification == "UNKNOWN" &&
                unlinked.source == "UNRESOLVED" &&
                unlinked.entrance_event_id < 0,
            "one entrance event was incorrectly reused by another session");
}

void ambiguousEntranceCandidatesStayUnknown() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");

    const auto completeEntrance = [&database](const std::string& objectId,
                                              const std::string& plate,
                                              const bool isEv,
                                              const std::int64_t seenAt,
                                              const double confidence) {
        const auto id = database.createEntranceRecognition(
            "cam01", "ch02", objectId, seenAt);
        database::EntranceVisionAnalysis analysis;
        analysis.is_ev = isEv;
        analysis.decision = isEv ? "EV_CANDIDATE" : "NON_EV_CANDIDATE";
        analysis.reason = "ambiguity-test";
        analysis.model_version = "test-v1";
        analysis.processing_ms = 5.0;
        require(database.updateEntranceVisionAnalysis(id, analysis),
                "ambiguous entrance analysis was not stored");
        require(database.finishEntranceRecognition(
                    id, plate, confidence, 1, {}) ==
                    (isEv ? "EV" : "NON_EV"),
                "ambiguous entrance candidate was not completed");
    };
    completeEntrance("candidate-a", "294마3087", true, 2'000'000, 0.99);
    completeEntrance("candidate-b", "294바3087", false, 2'010'000, 0.97);
    executeSql(
        temporary.path,
        "INSERT INTO PARKING_SESSION(session_id,slot_id,status,entry_time,"
        "entry_time_epoch_ms) VALUES(600,'EV04','ACTIVE',CURRENT_TIMESTAMP,"
        "2100000);");

    const auto result = database.applyPlateOcrWithEntrance(
        600, "EV04", {}, "294미3087", 0.70, 30 * 60 * 1000, 0.85);
    require(result.persisted && result.classification == "UNKNOWN" &&
                result.source == "UNRESOLVED" && result.vehicle_id < 0,
            "ambiguous entrance candidates produced an automatic decision");
}

void staleWorkingArtifactsBecomeFailedAndDeleted() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
    const auto eventId = database.createEntranceRecognition(
        "cam01", "ch02", "stale-object", 1000);
    require(database.updateEntranceImage(eventId, true, "stale/plate.jpg"),
            "stale entrance path was not stored");
    executeSql(temporary.path,
               "UPDATE ENTRANCE_RECOGNITION SET updated_at_epoch_ms=1000 "
               "WHERE entrance_event_id=" + std::to_string(eventId) + ";");

    const auto stale = database.listEntranceArtifactsForCleanup(2000);
    require(stale.size() == 1 && stale.front().event_id == eventId,
            "stale WORKING entrance event was not selected for cleanup");
    require(database.markEntranceArtifactsDeleted(eventId),
            "stale WORKING entrance event was not cleaned");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM ENTRANCE_RECOGNITION WHERE "
                      "entrance_event_id=" + std::to_string(eventId) +
                      " AND state='FAILED' AND artifact_state='DELETED' "
                      "AND plate_image_path IS NULL "
                      "AND last_error<>'' AND completed_at_epoch_ms IS NOT NULL;") == 1,
            "stale WORKING event did not retain a terminal audit record");
}

void iconDecisionUpsertsAndUpdatesVehicle() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
    const auto eventId = database.createEntranceRecognition(
        "cam01", "ch02", "new-non-ev", 2000);
    database::EntranceVisionAnalysis analysis;
    analysis.is_ev = false;
    analysis.decision = "NON_EV_CANDIDATE";
    analysis.reason = "test-policy";
    analysis.model_version = "test-v1";
    analysis.processing_ms = 8.0;
    require(database.updateEntranceVisionAnalysis(eventId, analysis),
            "non-EV icon analysis was not stored");
    require(database.finishEntranceRecognition(
                eventId, "88가8888", 0.8, 1, {}) == "NON_EV",
            "non-EV icon result did not create a vehicle");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM VEHICLE WHERE "
                      "plate_number='88가8888' AND is_ev=0;") == 1,
            "new non-EV vehicle was not inserted");

    const auto updatedEvent = database.createEntranceRecognition(
        "cam01", "ch02", "same-plate-new-object", 3000);
    analysis.is_ev = true;
    analysis.decision = "EV_CANDIDATE";
    require(database.updateEntranceVisionAnalysis(updatedEvent, analysis),
            "updated EV icon analysis was not stored");
    require(database.finishEntranceRecognition(
                updatedEvent, "88가8888", 0.9, 1, {}) == "EV",
            "latest icon decision did not update the vehicle");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM VEHICLE WHERE "
                      "plate_number='88가8888' AND is_ev=1;") == 1,
            "vehicle master was not updated by the latest icon decision");
}

void reviewDoesNotCreateVehicle() {
    TemporaryDatabase temporary;
    database::EventDatabase database(temporary.path);
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");

    const auto reviewId = database.createEntranceRecognition(
        "cam01", "ch02", "review", 4000);
    database::EntranceVisionAnalysis review;
    review.decision = "REVIEW";
    review.reason = "low-observability";
    review.model_version = "test-v1";
    review.processing_ms = 6.0;
    require(database.updateEntranceVisionAnalysis(reviewId, review),
            "REVIEW analysis was not stored");
    require(database.finishEntranceRecognition(
                reviewId, "77가7777", 0.8, 1, {}) == "EV_FAILED",
            "REVIEW result incorrectly finalized a binary vehicle");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM ENTRANCE_RECOGNITION WHERE "
                      "entrance_event_id=" + std::to_string(reviewId) +
                      " AND vision_is_ev IS NULL AND resolved_is_ev IS NULL "
                      "AND vision_decision='REVIEW' AND state='FAILED';") == 1,
            "REVIEW was converted into a binary EV value");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM VEHICLE WHERE "
                      "plate_number='77가7777';") == 0,
            "REVIEW result created a vehicle master row");
}

void legacyPhevMigrationPreservesVehicleAndForeignKey() {
    TemporaryDatabase temporary;
    const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
    {
        database::EventDatabase database(temporary.path);
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
    }
    executeSql(
        temporary.path,
        "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
        "INSERT INTO PARKING_SESSION(vehicle_id,slot_id,plate_number,entry_time,status) "
        "VALUES(2,'EV01','234나5678',CURRENT_TIMESTAMP,'ENDED');"
        "CREATE TABLE VEHICLE_LEGACY("
        "vehicle_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "plate_number TEXT UNIQUE NOT NULL,is_ev INTEGER NOT NULL DEFAULT 0,"
        "is_phev INTEGER NOT NULL DEFAULT 0,registered_at TEXT);"
        "INSERT INTO VEHICLE_LEGACY(vehicle_id,plate_number,is_ev,is_phev,registered_at) "
        "SELECT vehicle_id,plate_number,CASE WHEN vehicle_id=2 THEN 0 ELSE is_ev END,"
        "CASE WHEN vehicle_id=2 THEN 1 ELSE 0 END,registered_at FROM VEHICLE;"
        "DROP TABLE VEHICLE;"
        "ALTER TABLE VEHICLE_LEGACY RENAME TO VEHICLE;COMMIT;"
        "PRAGMA foreign_keys=ON;");
    {
        database::EventDatabase database(temporary.path);
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
        require(database.classifyVehicle("234나5678") ==
                    parking_timer::VehicleCategory::Ev,
                "legacy PHEV was not folded into EV");
    }
    require(scalarInt(
                temporary.path,
                "SELECT COUNT(*) FROM pragma_table_info('VEHICLE') "
                "WHERE name='is_phev';") == 0,
            "is_phev column remained after migration");
    require(scalarInt(temporary.path,
                      "SELECT is_ev FROM VEHICLE WHERE vehicle_id=2;") == 1,
            "legacy PHEV row did not retain its vehicle ID as EV");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM pragma_foreign_key_check;") == 0,
            "vehicle migration broke a foreign key");
    require(scalarInt(temporary.path,
                      "SELECT COUNT(*) FROM PARKING_SESSION WHERE vehicle_id=2;") == 1,
            "vehicle migration lost the referencing parking session");
}

void migrateExistingDatabaseCopy(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(path),
            "migration copy does not exist");
    const int sessionsBefore =
        scalarInt(path, "SELECT COUNT(*) FROM PARKING_SESSION;");
    const int imagesBefore = scalarInt(path, "SELECT COUNT(*) FROM IMAGE_LOG;");
    const int eventsBefore = scalarInt(path, "SELECT COUNT(*) FROM EVENT_LOG;");
    {
        database::EventDatabase database(path);
        const std::filesystem::path sqlDir{PARKING_TIMER_TEST_SQL_DIR};
        database.initialize(sqlDir / "schema.sql", sqlDir / "seed.sql");
    }
    require(scalarInt(path,
                      "SELECT COUNT(*) FROM pragma_table_info('VEHICLE') "
                      "WHERE name='is_phev';") == 0,
            "existing copy retained is_phev");
    require(scalarInt(path,
                      "SELECT COUNT(*) FROM pragma_foreign_key_check;") == 0,
            "existing copy migration broke a foreign key");
    require(scalarInt(path, "SELECT COUNT(*) FROM PARKING_SESSION;") ==
                sessionsBefore &&
                scalarInt(path, "SELECT COUNT(*) FROM IMAGE_LOG;") ==
                imagesBefore &&
                scalarInt(path, "SELECT COUNT(*) FROM EVENT_LOG;") ==
                eventsBefore,
            "existing copy migration changed operational row counts");
    require(scalarInt(path,
                      "SELECT COUNT(*) FROM pragma_table_info("
                      "'ENTRANCE_RECOGNITION') WHERE name IN ("
                      "'registered_is_ev','vision_is_ev','resolved_is_ev',"
                      "'decision_source','vision_decision','vision_reason',"
                      "'vision_model_version','vision_processing_ms',"
                      "'vision_result_path','vision_error','vehicle_id',"
                      "'duplicate_count','artifact_state',"
                      "'artifacts_deleted_at_epoch_ms');") == 14,
            "existing copy did not receive all entrance vision columns");
    require(scalarInt(path,
                      "SELECT COUNT(*) FROM pragma_table_info("
                      "'PARKING_SESSION') WHERE name IN ("
                      "'parking_ocr_plate','parking_ocr_confidence',"
                      "'entrance_event_id','plate_match_score',"
                      "'plate_resolution_source');") == 5,
            "existing copy did not receive entrance correlation columns");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--migrate-copy") {
            migrateExistingDatabaseCopy(argv[2]);
            std::cout << "EntranceDatabase copy migration passed\n";
            return EXIT_SUCCESS;
        }
        schemaAndLifecyclePersist();
        parkingOcrUsesRecentEntranceAsAuthoritativeSource();
        ambiguousEntranceCandidatesStayUnknown();
        staleWorkingArtifactsBecomeFailedAndDeleted();
        iconDecisionUpsertsAndUpdatesVehicle();
        reviewDoesNotCreateVehicle();
        legacyPhevMigrationPreservesVehicleAndForeignKey();
        std::cout << "EntranceDatabaseTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceDatabaseTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

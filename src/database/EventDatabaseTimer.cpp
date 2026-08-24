#include "database/EventDatabase.hpp"

#include "database/db_manager.h"

#include <sqlite3.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace database {
using parking_timer::LogRecord;
using parking_timer::VehicleCategory;
namespace {

/**
 * @brief `sqlite3_stmt`의 prepare/finalize 수명을 RAII로 관리하는 내부 래퍼.
 */
class Statement {
public:
    /**
     * @brief SQL 문자열을 prepared statement로 컴파일한다.
     *
     * @param[in] database statement가 사용할 열린 SQLite 연결.
     * @param[in] sql 컴파일할 NUL 종료 SQL 문자열.
     * @throws std::runtime_error SQL prepare가 실패한 경우.
     */
    Statement(sqlite3* database, const std::string_view sql) : database_(database) {
        const int result = sqlite3_prepare_v2(database_, sql.data(),
                                              static_cast<int>(sql.size()),
                                              &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error("SQLite prepare failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    /**
     * @brief 보유한 prepared statement를 `sqlite3_finalize()`로 해제한다.
     */
    ~Statement() { sqlite3_finalize(statement_); }

    /** @brief SQLite statement의 이중 finalize를 막기 위해 복사를 금지한다. */
    Statement(const Statement&) = delete;

    /** @brief SQLite statement의 이중 finalize를 막기 위해 복사 대입을 금지한다. */
    Statement& operator=(const Statement&) = delete;

    /**
     * @brief SQLite C API에 전달할 원시 statement 포인터를 반환한다.
     *
     * @return 이 RAII 객체가 소유하며 객체 수명 동안 유효한 `sqlite3_stmt*`.
     */
    sqlite3_stmt* get() const noexcept { return statement_; }

    /**
     * @brief UTF-8 텍스트 값을 지정한 SQL 파라미터에 바인딩한다.
     *
     * @param[in] index 1부터 시작하는 SQLite 파라미터 인덱스.
     * @param[in] value 바인딩할 문자열.
     * @throws std::runtime_error 바인딩에 실패한 경우.
     * @note `SQLITE_TRANSIENT`를 사용하므로 호출 후 원본 문자열 수명에 의존하지 않는다.
     */
    void bindText(const int index, const std::string_view value) {
        const int result = sqlite3_bind_text(statement_, index, value.data(),
                                             static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (result != SQLITE_OK) {
            throw std::runtime_error("SQLite text bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    /**
     * @brief 32비트 정수 값을 지정한 SQL 파라미터에 바인딩한다.
     *
     * @param[in] index 1부터 시작하는 SQLite 파라미터 인덱스.
     * @param[in] value 바인딩할 정수 값.
     * @throws std::runtime_error 바인딩에 실패한 경우.
     */
    void bindInt(const int index, const int value) {
        if (sqlite3_bind_int(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("SQLite integer bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

    /**
     * @brief 64비트 정수 값을 지정한 SQL 파라미터에 바인딩한다.
     *
     * @param[in] index 1부터 시작하는 SQLite 파라미터 인덱스.
     * @param[in] value 바인딩할 정수 값.
     * @throws std::runtime_error 바인딩에 실패한 경우.
     */
    void bindInt64(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("SQLite integer bind failed: " +
                                     std::string(sqlite3_errmsg(database_)));
        }
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

/**
 * @brief 현재 row의 SQLite TEXT 컬럼을 C++ 문자열로 복사한다.
 *
 * @param[in] statement `SQLITE_ROW`를 가리키는 prepared statement.
 * @param[in] column 0부터 시작하는 결과 컬럼 인덱스.
 * @return 컬럼 문자열. SQL NULL이면 빈 문자열.
 */
std::string columnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} :
                              std::string{reinterpret_cast<const char*>(value)};
}

/**
 * @brief SQL NULL과 실제 문자열을 구분해 optional 문자열로 읽는다.
 *
 * @param[in] statement `SQLITE_ROW`를 가리키는 prepared statement.
 * @param[in] column 0부터 시작하는 결과 컬럼 인덱스.
 * @return SQL NULL이면 `std::nullopt`, 아니면 컬럼 문자열.
 */
std::optional<std::string> optionalColumnText(sqlite3_stmt* statement,
                                               const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(statement, column);
}

/**
 * @brief 고정된 SELECT 컬럼 순서의 현재 row를 `LogRecord`로 변환한다.
 *
 * @param[in] statement `timer_log` 조회 결과의 현재 row.
 * @return 모든 로그 컬럼이 채워진 값 객체.
 * @warning 호출 SQL의 컬럼 순서는 이 함수의 0~9 인덱스 순서와 일치해야 한다.
 */
LogRecord readLogRecord(sqlite3_stmt* statement) {
    LogRecord record;
    record.id = sqlite3_column_int64(statement, 0);
    record.car_number = columnText(statement, 1);
    record.slot_id = columnText(statement, 2);
    record.status = columnText(statement, 3);
    record.parked_at = columnText(statement, 4);
    record.violation_at = optionalColumnText(statement, 5);
    record.departed_at = optionalColumnText(statement, 6);
    record.image_path_1 = optionalColumnText(statement, 7);
    record.image_path_2 = optionalColumnText(statement, 8);
    record.is_canceled = sqlite3_column_int(statement, 9) != 0;
    return record;
}

/**
 * @brief 변경 SQL을 한 번 실행하고 정상 완료 여부를 검사한다.
 *
 * @param[in] database 오류 메시지를 얻을 SQLite 연결.
 * @param[in] statement 실행할 prepared statement.
 * @throws std::runtime_error 실행 결과가 `SQLITE_DONE`이 아닌 경우.
 */
void requireDone(sqlite3* database, sqlite3_stmt* statement) {
    const int result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
        throw std::runtime_error("SQLite statement failed: " +
                                 std::string(sqlite3_errmsg(database)));
    }
}

constexpr std::string_view kLogSelect =
    "SELECT s.session_id, COALESCE(s.plate_number, ''), s.slot_id, "
    "CASE s.status WHEN 'ACTIVE' THEN 'PARKED' WHEN 'ENDED' THEN 'DEPARTS' "
    "ELSE s.status END, s.entry_time, s.violation_at, s.exit_time, "
    "(SELECT i.original_image_path FROM IMAGE_LOG i WHERE i.session_id=s.session_id "
    "AND (i.enhancement_type IN ('TIMER_ENTRY','HALL_ENTRY','BESTSHOT_VEHICLE') "
    "OR i.evidence_reason='OCCUPANCY_START_EVIDENCE') "
    "ORDER BY i.image_id LIMIT 1), "
    "(SELECT i.original_image_path FROM IMAGE_LOG i WHERE i.session_id=s.session_id "
    "AND (i.enhancement_type='TIMER_VIOLATION' "
    "OR i.evidence_reason='OVERSTAY_EVIDENCE') "
    "ORDER BY i.image_id DESC LIMIT 1), "
    "CASE WHEN s.exit_time IS NULL THEN 0 ELSE 1 END FROM PARKING_SESSION s ";

bool tableHasColumn(sqlite3* database, const std::string_view table,
                    const std::string_view column) {
    Statement statement(database, "PRAGMA table_info(" + std::string{table} + ");");
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (columnText(statement.get(), 1) == column) return true;
    }
    return false;
}

}  // namespace

/**
 * @brief SQLite 이벤트 DB를 열고 프로토타입에 필요한 연결 옵션을 설정한다.
 *
 * @param[in] database_path 열거나 새로 만들 SQLite 파일 경로. `:memory:`도 가능하다.
 * @throws std::filesystem::filesystem_error 상위 디렉터리 생성에 실패한 경우.
 * @throws std::runtime_error DB open 또는 PRAGMA 설정에 실패한 경우.
 * @note foreign key, 3초 busy timeout, WAL 모드를 연결 생성 직후 활성화한다.
 */
EventDatabase::EventDatabase(const std::filesystem::path& database_path) {
    const auto parent = database_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (!open(database_path.string())) {
        throw std::runtime_error("cannot open SQLite database: " +
                                 database_path.string());
    }

    // 메인 서버와 타이머가 동일한 연결 정책을 공유한다.
    try {
        executeSqlUnlocked("PRAGMA busy_timeout = 3000;");
        executeSqlUnlocked("PRAGMA journal_mode = WAL;");
    } catch (...) {
        close();
        throw;
    }
}

/**
 * @brief 스키마를 생성하고 차량 마스터 테스트 데이터를 멱등하게 삽입한다.
 *
 * @param[in] schema_file 실행할 DDL 파일 경로.
 * @param[in] seed_file 실행할 seed SQL 파일 경로.
 * @throws std::runtime_error 파일 읽기 또는 SQL 실행에 실패한 경우.
 * @note seed는 `BEGIN IMMEDIATE` 트랜잭션으로 묶고 오류 시 전부 rollback한다.
 */
void EventDatabase::initialize(const std::filesystem::path& schema_file,
                               const std::filesystem::path& seed_file) {
    const auto schema = readTextFile(schema_file);
    const auto seed = readTextFile(seed_file);
    {
        std::lock_guard lock(db_mutex_);
        if (!opened_ || db_ == nullptr)
            throw std::runtime_error("cannot initialize a closed database");
        // SQLite 3.34는 DROP COLUMN을 지원하지 않는다. 기존 PHEV를 EV로
        // 흡수하면서 vehicle_id와 PARKING_SESSION 외래키를 보존해 재작성한다.
        if (tableHasColumn(db_, "VEHICLE", "vehicle_id") &&
            tableHasColumn(db_, "VEHICLE", "is_phev")) {
            executeSqlUnlocked("PRAGMA foreign_keys = OFF;");
            try {
                executeSqlUnlocked("BEGIN IMMEDIATE;");
                executeSqlUnlocked("DROP TABLE IF EXISTS VEHICLE_BINARY_MIGRATION;");
                executeSqlUnlocked(
                    "CREATE TABLE VEHICLE_BINARY_MIGRATION ("
                    "vehicle_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "plate_number TEXT UNIQUE NOT NULL,"
                    "is_ev INTEGER NOT NULL DEFAULT 0 CHECK(is_ev IN (0,1)),"
                    "registered_at TEXT DEFAULT CURRENT_TIMESTAMP);");
                executeSqlUnlocked(
                    "INSERT INTO VEHICLE_BINARY_MIGRATION("
                    "vehicle_id,plate_number,is_ev,registered_at) "
                    "SELECT vehicle_id,plate_number,"
                    "CASE WHEN is_ev=1 OR is_phev=1 THEN 1 ELSE 0 END,"
                    "registered_at FROM VEHICLE;");
                executeSqlUnlocked("DROP TABLE VEHICLE;");
                executeSqlUnlocked(
                    "ALTER TABLE VEHICLE_BINARY_MIGRATION RENAME TO VEHICLE;");
                executeSqlUnlocked("COMMIT;");
            } catch (...) {
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr,
                             nullptr);
                throw;
            }
            executeSqlUnlocked("PRAGMA foreign_keys = ON;");
            Statement foreignKeyCheck(db_, "PRAGMA foreign_key_check;");
            if (sqlite3_step(foreignKeyCheck.get()) == SQLITE_ROW) {
                throw std::runtime_error(
                    "VEHICLE binary migration violated a foreign key");
            }
        }
        // Add columns referenced by current schema indexes/triggers before
        // executing CREATE ... IF NOT EXISTS against a representative older
        // database. Fresh databases skip these guards and are created by the
        // same schema below.
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id")) {
            if (!tableHasColumn(db_, "PARKING_SESSION", "violation_at"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN violation_at TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "occupancy_attempt_id"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "occupancy_attempt_id TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION", "entry_command_id"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "entry_command_id TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION", "exit_command_id"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "exit_command_id TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "entry_time_epoch_ms"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "entry_time_epoch_ms INTEGER;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "exit_time_epoch_ms"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "exit_time_epoch_ms INTEGER;");
            if (!tableHasColumn(db_, "PARKING_SESSION", "hall_confirmed"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN hall_confirmed "
                    "INTEGER NOT NULL DEFAULT 0 CHECK (hall_confirmed IN (0,1));");
            if (!tableHasColumn(db_, "PARKING_SESSION", "iva_confirmed"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN iva_confirmed "
                    "INTEGER NOT NULL DEFAULT 0 CHECK (iva_confirmed IN (0,1));");
            if (!tableHasColumn(db_, "PARKING_SESSION", "hall_occupied"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN hall_occupied "
                    "INTEGER NOT NULL DEFAULT 0 CHECK (hall_occupied IN (0,1));");
            if (!tableHasColumn(db_, "PARKING_SESSION", "iva_occupied"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN iva_occupied "
                    "INTEGER NOT NULL DEFAULT 0 CHECK (iva_occupied IN (0,1));");
            if (!tableHasColumn(db_, "PARKING_SESSION", "parking_ocr_plate"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN parking_ocr_plate TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "parking_ocr_confidence"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "parking_ocr_confidence REAL;");
            if (!tableHasColumn(db_, "PARKING_SESSION", "entrance_event_id"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN entrance_event_id "
                    "INTEGER REFERENCES ENTRANCE_RECOGNITION(entrance_event_id);");
            if (!tableHasColumn(db_, "PARKING_SESSION", "plate_match_score"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN plate_match_score REAL;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "plate_resolution_source"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "plate_resolution_source TEXT NOT NULL DEFAULT 'UNRESOLVED' "
                    "CHECK(plate_resolution_source IN ('UNRESOLVED',"
                    "'ENTRANCE_EXACT','ENTRANCE_FUZZY','VEHICLE_EXACT'));"
                );
        }
        if (tableHasColumn(db_, "IMAGE_LOG", "image_id")) {
            if (!tableHasColumn(db_, "IMAGE_LOG", "evidence_reason"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN evidence_reason TEXT;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "correlation_id"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN correlation_id TEXT;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "camera_object_id"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN camera_object_id TEXT;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "image_ref"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN image_ref TEXT;");
            if (!tableHasColumn(
                    db_, "IMAGE_LOG", "correlation_binding_revision"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN "
                    "correlation_binding_revision INTEGER;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_x"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_x REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_y"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_y REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_width"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_width REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_height"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_height REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_revision"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN roi_revision INTEGER;");
        }
        if (tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX", "command_id")) {
            if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                                "effect_state"))
                executeSqlUnlocked(
                    "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                    "effect_state TEXT NOT NULL DEFAULT 'NONE' CHECK "
                    "(effect_state IN ('NONE','PENDING','APPLIED'));");
            if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                                "effect_attempt_count"))
                executeSqlUnlocked(
                    "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                    "effect_attempt_count INTEGER NOT NULL DEFAULT 0;");
            if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                                "effect_next_attempt_at_epoch_ms"))
                executeSqlUnlocked(
                    "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                    "effect_next_attempt_at_epoch_ms INTEGER NOT NULL "
                    "DEFAULT 0;");
            if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                                "effect_last_error"))
                executeSqlUnlocked(
                    "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                    "effect_last_error TEXT NOT NULL DEFAULT '';");
        }
        // schema.sql은 기존 입구 테이블에도 인덱스를 생성한다. 인덱스가 참조하는
        // 신규 컬럼은 CREATE INDEX보다 먼저 추가해야 구버전 DB migration이 멈추지 않는다.
        if (tableHasColumn(db_, "ENTRANCE_RECOGNITION", "entrance_event_id")) {
            if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vehicle_id"))
                executeSqlUnlocked(
                    "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN vehicle_id "
                    "INTEGER REFERENCES VEHICLE(vehicle_id);");
            if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "artifact_state"))
                executeSqlUnlocked(
                    "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN artifact_state "
                    "TEXT NOT NULL DEFAULT 'WORKING' CHECK(artifact_state IN ("
                    "'WORKING','DELETE_PENDING','RETAINED_FAILURE','DELETED'));");
        }
        executeSqlUnlocked(schema);
        executeSqlUnlocked("BEGIN IMMEDIATE;");
        try {
            executeSqlUnlocked(seed);
            executeSqlUnlocked("COMMIT;");
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw;
        }
    }
    // All entrypoints finish through the same additive migration and
    // invariant checks. Calling initialize repeatedly is intentionally safe.
    migrateRuntimeSchema();
}

void EventDatabase::migrateRuntimeSchema() {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        throw std::runtime_error("cannot migrate a closed SQLite database");
    }
    runtime_schema_ready_ = false;
    occupancy_schema_ready_ = false;
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS SYSTEM_SETTINGS ("
            "key TEXT PRIMARY KEY, value TEXT NOT NULL, "
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS app_users ("
            "user_id INTEGER PRIMARY KEY,"
            "account_id TEXT NOT NULL UNIQUE,"
            "password_hash TEXT NOT NULL,display_name TEXT,"
            "enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),"
            "created_at_utc INTEGER NOT NULL,updated_at_utc INTEGER NOT NULL);");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS app_sessions ("
            "session_id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL,"
            "token_hash BLOB NOT NULL UNIQUE,created_at_utc INTEGER NOT NULL,"
            "expires_at_utc INTEGER NOT NULL,revoked_at_utc INTEGER,"
            "FOREIGN KEY(user_id) REFERENCES app_users(user_id));");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_app_sessions_user_active "
            "ON app_sessions(user_id,expires_at_utc,revoked_at_utc);");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS ENTRANCE_RECOGNITION ("
            "entrance_event_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "camera_id TEXT NOT NULL,channel_id TEXT NOT NULL,"
            "object_id TEXT NOT NULL,state TEXT NOT NULL CHECK(state IN ("
            "'COLLECTING','OCR_QUEUED','OCR_PROCESSING','COMPLETED','FAILED')),"
            "vehicle_image_path TEXT,plate_image_path TEXT,vehicle_id INTEGER,"
            "plate_number TEXT,"
            "classification TEXT,"
            "registered_is_ev INTEGER CHECK(registered_is_ev IN (0,1)),"
            "vision_is_ev INTEGER CHECK(vision_is_ev IN (0,1)),"
            "resolved_is_ev INTEGER CHECK(resolved_is_ev IN (0,1)),"
            "decision_source TEXT NOT NULL DEFAULT 'PENDING' CHECK("
            "decision_source IN ('PENDING','VEHICLE_DB','VISION','CONSENSUS',"
            "'SHADOW','CONFLICT')),"
            "vision_decision TEXT NOT NULL DEFAULT 'NOT_RUN' CHECK("
            "vision_decision IN ('NOT_RUN','EV_CANDIDATE',"
            "'NON_EV_CANDIDATE','REVIEW')),"
            "vision_reason TEXT NOT NULL DEFAULT '',"
            "vision_model_version TEXT NOT NULL DEFAULT '',"
            "vision_processing_ms REAL,vision_result_path TEXT,"
            "vision_error TEXT NOT NULL DEFAULT '',"
            "duplicate_count INTEGER NOT NULL DEFAULT 0,"
            "artifact_state TEXT NOT NULL DEFAULT 'WORKING' CHECK("
            "artifact_state IN ('WORKING','DELETE_PENDING',"
            "'RETAINED_FAILURE','DELETED')),"
            "artifacts_deleted_at_epoch_ms INTEGER,confidence REAL,"
            "ocr_attempts INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT '',"
            "first_seen_epoch_ms INTEGER NOT NULL,updated_at_epoch_ms INTEGER NOT NULL,"
            "completed_at_epoch_ms INTEGER,"
            "FOREIGN KEY(vehicle_id) REFERENCES VEHICLE(vehicle_id));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vehicle_id"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN vehicle_id "
                "INTEGER REFERENCES VEHICLE(vehicle_id);");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "registered_is_ev"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "registered_is_ev INTEGER CHECK(registered_is_ev IN (0,1));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_is_ev"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_is_ev INTEGER CHECK(vision_is_ev IN (0,1));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "resolved_is_ev"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "resolved_is_ev INTEGER CHECK(resolved_is_ev IN (0,1));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "decision_source"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN decision_source "
                "TEXT NOT NULL DEFAULT 'PENDING' CHECK(decision_source IN ("
                "'PENDING','VEHICLE_DB','VISION','CONSENSUS','SHADOW','CONFLICT'));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_decision"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN vision_decision "
                "TEXT NOT NULL DEFAULT 'NOT_RUN' CHECK(vision_decision IN ("
                "'NOT_RUN','EV_CANDIDATE','NON_EV_CANDIDATE','REVIEW'));");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_reason"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_reason TEXT NOT NULL DEFAULT '';");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_model_version"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_model_version TEXT NOT NULL DEFAULT '';");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_processing_ms"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_processing_ms REAL;");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_result_path"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_result_path TEXT;");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "vision_error"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "vision_error TEXT NOT NULL DEFAULT '';");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "duplicate_count"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "duplicate_count INTEGER NOT NULL DEFAULT 0;");
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION", "artifact_state"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN artifact_state "
                "TEXT NOT NULL DEFAULT 'WORKING' CHECK(artifact_state IN ("
                "'WORKING','DELETE_PENDING','RETAINED_FAILURE','DELETED'));"
            );
        if (!tableHasColumn(db_, "ENTRANCE_RECOGNITION",
                            "artifacts_deleted_at_epoch_ms"))
            executeSqlUnlocked(
                "ALTER TABLE ENTRANCE_RECOGNITION ADD COLUMN "
                "artifacts_deleted_at_epoch_ms INTEGER;");
        executeSqlUnlocked(
            "UPDATE ENTRANCE_RECOGNITION SET artifact_state='DELETE_PENDING' "
            "WHERE state='COMPLETED' AND artifact_state='WORKING';");
        executeSqlUnlocked(
            "UPDATE ENTRANCE_RECOGNITION SET artifact_state='RETAINED_FAILURE' "
            "WHERE state='FAILED' AND artifact_state='WORKING';");
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id")) {
            if (!tableHasColumn(db_, "PARKING_SESSION", "parking_ocr_plate"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN parking_ocr_plate TEXT;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "parking_ocr_confidence"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "parking_ocr_confidence REAL;");
            if (!tableHasColumn(db_, "PARKING_SESSION", "entrance_event_id"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN entrance_event_id "
                    "INTEGER REFERENCES ENTRANCE_RECOGNITION(entrance_event_id);");
            if (!tableHasColumn(db_, "PARKING_SESSION", "plate_match_score"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN plate_match_score REAL;");
            if (!tableHasColumn(db_, "PARKING_SESSION",
                                "plate_resolution_source"))
                executeSqlUnlocked(
                    "ALTER TABLE PARKING_SESSION ADD COLUMN "
                    "plate_resolution_source TEXT NOT NULL DEFAULT 'UNRESOLVED' "
                    "CHECK(plate_resolution_source IN ('UNRESOLVED',"
                    "'ENTRANCE_EXACT','ENTRANCE_FUZZY','VEHICLE_EXACT'));"
                );
        }
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_entrance_recognition_object ON "
            "ENTRANCE_RECOGNITION(camera_id,channel_id,object_id,first_seen_epoch_ms);");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_entrance_recognition_vehicle ON "
            "ENTRANCE_RECOGNITION(vehicle_id);");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_entrance_recognition_cleanup ON "
            "ENTRANCE_RECOGNITION(artifact_state,completed_at_epoch_ms);");
        executeSqlUnlocked(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_parking_session_entrance_event "
            "ON PARKING_SESSION(entrance_event_id) "
            "WHERE entrance_event_id IS NOT NULL;");
        executeSqlUnlocked(
        "CREATE TABLE IF NOT EXISTS FIRE_ALARM_STATE ("
        "channel_id TEXT PRIMARY KEY,"
        "sensor_id TEXT UNIQUE NOT NULL,"
        "retained_topic TEXT UNIQUE NOT NULL,"
        "desired_lifecycle TEXT NOT NULL CHECK (desired_lifecycle IN "
        "('OPEN','ACKNOWLEDGED','RESOLVED')),"
        "active_alarm_id TEXT,"
        "last_event_id TEXT NOT NULL,"
        "fire_revision INTEGER NOT NULL CHECK (fire_revision > 0),"
        "protocol_mode TEXT NOT NULL CHECK (protocol_mode IN "
        "('UNSEEN','LEGACY','VERSIONED')),"
        "active_boot_id TEXT,"
        "last_source_sequence TEXT,"
        "last_signal_json TEXT NOT NULL,"
        "updated_at TEXT NOT NULL);");
        executeSqlUnlocked(
        "CREATE TABLE IF NOT EXISTS FIRE_MQTT_OUTBOX ("
        "delivery_key TEXT PRIMARY KEY,"
        "sink_kind TEXT NOT NULL CHECK (sink_kind IN "
        "('RETAINED_STATE','LIFECYCLE_EVENT')),"
        "logical_key TEXT NOT NULL,"
        "sensor_id TEXT NOT NULL,"
        "channel_id TEXT NOT NULL,"
        "event_id TEXT NOT NULL,"
        "alarm_id TEXT NOT NULL,"
        "fire_revision INTEGER NOT NULL CHECK (fire_revision > 0),"
        "topic TEXT NOT NULL,"
        "payload_json TEXT NOT NULL,"
        "qos INTEGER NOT NULL DEFAULT 1 CHECK (qos = 1),"
        "retain INTEGER NOT NULL CHECK (retain IN (0,1)),"
        "attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),"
        "next_attempt_at TEXT NOT NULL,"
        "last_error TEXT NOT NULL DEFAULT '',"
        "delivery_state TEXT NOT NULL CHECK (delivery_state IN "
        "('PENDING','IN_FLIGHT','ACKED')),"
        "acked_revision INTEGER,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,"
        "FOREIGN KEY(channel_id) REFERENCES FIRE_ALARM_STATE(channel_id));");
        executeSqlUnlocked(
        "CREATE UNIQUE INDEX IF NOT EXISTS ux_fire_outbox_sink_logical "
        "ON FIRE_MQTT_OUTBOX(sink_kind, logical_key);");
        executeSqlUnlocked(
        "CREATE UNIQUE INDEX IF NOT EXISTS ux_fire_lifecycle_event "
        "ON FIRE_MQTT_OUTBOX(sink_kind, event_id) "
        "WHERE sink_kind='LIFECYCLE_EVENT';");
        executeSqlUnlocked(
        "CREATE INDEX IF NOT EXISTS idx_fire_outbox_pending "
        "ON FIRE_MQTT_OUTBOX(sink_kind, delivery_state, channel_id, "
        "fire_revision);");
        executeSqlUnlocked(
        "CREATE TABLE IF NOT EXISTS SENSOR_RETIRED_BOOT_ID ("
        "source_kind TEXT NOT NULL CHECK (source_kind IN ('HALL','FIRE')),"
        "sensor_id TEXT NOT NULL,boot_id TEXT NOT NULL,"
        "retired_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY(source_kind,sensor_id,boot_id));");
        const bool has_parking_slot =
            tableHasColumn(db_, "PARKING_SLOT", "slot_id");
        const bool has_parking_session =
            tableHasColumn(db_, "PARKING_SESSION", "session_id");
        if (has_parking_slot != has_parking_session) {
            throw std::runtime_error(
                "parking runtime schema is incomplete: PARKING_SLOT and "
                "PARKING_SESSION must be present together");
        }
        if (has_parking_slot) {
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "occupancy_attempt_id")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION "
                "ADD COLUMN occupancy_attempt_id TEXT;");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "entry_command_id")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN entry_command_id TEXT;");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "exit_command_id")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN exit_command_id TEXT;");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "entry_time_epoch_ms")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION "
                "ADD COLUMN entry_time_epoch_ms INTEGER;");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "exit_time_epoch_ms")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION "
                "ADD COLUMN exit_time_epoch_ms INTEGER;");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "hall_confirmed")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN hall_confirmed "
                "INTEGER NOT NULL DEFAULT 0 CHECK (hall_confirmed IN (0,1));");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "iva_confirmed")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN iva_confirmed "
                "INTEGER NOT NULL DEFAULT 0 CHECK (iva_confirmed IN (0,1));");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "hall_occupied")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN hall_occupied "
                "INTEGER NOT NULL DEFAULT 0 CHECK (hall_occupied IN (0,1));");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "session_id") &&
            !tableHasColumn(db_, "PARKING_SESSION", "iva_occupied")) {
            executeSqlUnlocked(
                "ALTER TABLE PARKING_SESSION ADD COLUMN iva_occupied "
                "INTEGER NOT NULL DEFAULT 0 CHECK (iva_occupied IN (0,1));");
        }
        if (tableHasColumn(db_, "PARKING_SESSION", "occupancy_attempt_id")) {
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET "
                "occupancy_attempt_id='legacy-attempt:' || session_id "
                "WHERE occupancy_attempt_id IS NULL OR occupancy_attempt_id='';");
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET "
                "entry_command_id='legacy-entry:' || session_id "
                "WHERE entry_command_id IS NULL OR entry_command_id='';");
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET entry_time_epoch_ms="
                "CAST((julianday(entry_time)-2440587.5)*86400000 AS INTEGER) "
                "WHERE entry_time_epoch_ms IS NULL;");
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET exit_time_epoch_ms="
                "CAST((julianday(exit_time)-2440587.5)*86400000 AS INTEGER) "
                "WHERE exit_time IS NOT NULL AND exit_time_epoch_ms IS NULL;");
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET hall_confirmed=1,"
                "hall_occupied=CASE WHEN exit_time IS NULL AND status IN "
                "('ACTIVE','VIOLATION') THEN 1 ELSE 0 END WHERE "
                "entry_command_id LIKE 'hall:%' AND hall_confirmed=0 AND "
                "iva_confirmed=0 AND hall_occupied=0 AND iva_occupied=0;");
            executeSqlUnlocked(
                "UPDATE PARKING_SESSION SET iva_confirmed=1,"
                "iva_occupied=CASE WHEN exit_time IS NULL AND status IN "
                "('ACTIVE','VIOLATION') THEN 1 ELSE 0 END WHERE "
                "entry_command_id LIKE 'camera:%' AND hall_confirmed=0 AND "
                "iva_confirmed=0 AND hall_occupied=0 AND iva_occupied=0;");
            Statement invalid_timestamp(db_,
                "SELECT 1 FROM PARKING_SESSION WHERE entry_time_epoch_ms IS NULL "
                "OR (exit_time IS NOT NULL AND exit_time_epoch_ms IS NULL) "
                "LIMIT 1;");
            if (sqlite3_step(invalid_timestamp.get()) == SQLITE_ROW) {
                throw std::runtime_error(
                    "parking session timestamp migration is ambiguous");
            }
            Statement duplicate_active(db_,
                "SELECT slot_id FROM PARKING_SESSION WHERE exit_time IS NULL "
                "AND status IN ('ACTIVE','VIOLATION') GROUP BY slot_id "
                "HAVING COUNT(*)>1 LIMIT 1;");
            if (sqlite3_step(duplicate_active.get()) == SQLITE_ROW) {
                throw std::runtime_error(
                    "multiple active parking sessions exist for slot=" +
                    columnText(duplicate_active.get(), 0));
            }
            executeSqlUnlocked(
                "UPDATE PARKING_SLOT SET status=CASE WHEN EXISTS("
                "SELECT 1 FROM PARKING_SESSION s WHERE "
                "s.slot_id=PARKING_SLOT.slot_id AND s.exit_time IS NULL AND "
                "s.status IN ('ACTIVE','VIOLATION')) THEN 'OCCUPIED' "
                "ELSE 'VACANT' END,updated_at=CURRENT_TIMESTAMP;");
            executeSqlUnlocked(
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "ux_parking_session_entry_command "
                "ON PARKING_SESSION(entry_command_id) "
                "WHERE entry_command_id IS NOT NULL;");
            executeSqlUnlocked(
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "ux_parking_session_exit_command "
                "ON PARKING_SESSION(exit_command_id) "
                "WHERE exit_command_id IS NOT NULL;");
            executeSqlUnlocked(
                "CREATE UNIQUE INDEX IF NOT EXISTS ux_parking_active_slot "
                "ON PARKING_SESSION(slot_id) WHERE exit_time IS NULL "
                "AND status IN ('ACTIVE','VIOLATION');");
        }
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS OCCUPANCY_COMMAND_INBOX ("
            "command_id TEXT PRIMARY KEY,slot_id TEXT NOT NULL,"
            "source_kind TEXT NOT NULL CHECK (source_kind IN "
            "('HALL_OBSERVATION','CAMERA_OBSERVATION','EXIT_DEADLINE')),"
            "sensor_id TEXT NOT NULL DEFAULT '',source_identity TEXT NOT NULL,"
            "source_sequence TEXT,occurred_at TEXT NOT NULL,"
            "payload_json TEXT NOT NULL,due_at_epoch_ms INTEGER NOT NULL DEFAULT 0,"
            "admission_ordinal INTEGER NOT NULL UNIQUE,status TEXT NOT NULL "
            "CHECK (status IN ('PENDING_UNPREPARED','PENDING_PREPARED',"
            "'APPLIED','REJECTED_INVALID')),"
            "occupancy_attempt_id TEXT NOT NULL DEFAULT '',"
            "correlation_id TEXT NOT NULL DEFAULT '',"
            "observation_generation INTEGER NOT NULL DEFAULT 0,"
            "deadline_id TEXT NOT NULL DEFAULT '',expected_session_id INTEGER,"
            "attempt_count INTEGER NOT NULL DEFAULT 0,"
            "next_attempt_at_epoch_ms INTEGER NOT NULL DEFAULT 0,"
            "last_error TEXT NOT NULL DEFAULT '',"
            "result_code TEXT NOT NULL DEFAULT '',result_session_id INTEGER,"
            "effect_state TEXT NOT NULL DEFAULT 'NONE' CHECK (effect_state IN "
            "('NONE','PENDING','APPLIED'))"
            ","
            "effect_attempt_count INTEGER NOT NULL DEFAULT 0,"
            "effect_next_attempt_at_epoch_ms INTEGER NOT NULL DEFAULT 0,"
            "effect_last_error TEXT NOT NULL DEFAULT '',"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "FOREIGN KEY(slot_id) REFERENCES PARKING_SLOT(slot_id));");
        if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX", "effect_state")) {
            executeSqlUnlocked(
                "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                "effect_state TEXT NOT NULL DEFAULT 'NONE' CHECK "
                "(effect_state IN ('NONE','PENDING','APPLIED'));");
        }
        if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                            "effect_attempt_count")) {
            executeSqlUnlocked(
                "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                "effect_attempt_count INTEGER NOT NULL DEFAULT 0;");
        }
        if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                            "effect_next_attempt_at_epoch_ms")) {
            executeSqlUnlocked(
                "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                "effect_next_attempt_at_epoch_ms INTEGER NOT NULL DEFAULT 0;");
        }
        if (!tableHasColumn(db_, "OCCUPANCY_COMMAND_INBOX",
                            "effect_last_error")) {
            executeSqlUnlocked(
                "ALTER TABLE OCCUPANCY_COMMAND_INBOX ADD COLUMN "
                "effect_last_error TEXT NOT NULL DEFAULT '';");
        }
        // PENDING_PREPARED is a legacy crash state.  It is only safe to
        // promote it when the authoritative session row proves that this
        // exact command committed.  Everything else must be replayed; treating
        // an ambiguous prepared row as successful can permanently discard the
        // observation that was interrupted before the session transaction.
        if (tableHasColumn(db_, "PARKING_SESSION", "entry_command_id") &&
            tableHasColumn(db_, "PARKING_SESSION", "exit_command_id")) {
            executeSqlUnlocked(
                "UPDATE OCCUPANCY_COMMAND_INBOX AS c SET status='APPLIED',"
                "result_code='SESSION_STARTED',"
                "result_session_id=(SELECT s.session_id FROM PARKING_SESSION s "
                "WHERE s.entry_command_id=c.command_id LIMIT 1),"
                "effect_state='PENDING',effect_next_attempt_at_epoch_ms=0,"
                "effect_last_error='' WHERE c.status='PENDING_PREPARED' AND "
                "EXISTS (SELECT 1 FROM PARKING_SESSION s WHERE "
                "s.entry_command_id=c.command_id);");
            executeSqlUnlocked(
                "UPDATE OCCUPANCY_COMMAND_INBOX AS c SET status='APPLIED',"
                "result_code='SESSION_ENDED',"
                "result_session_id=(SELECT s.session_id FROM PARKING_SESSION s "
                "WHERE s.exit_command_id=c.command_id LIMIT 1),"
                "effect_state='PENDING',effect_next_attempt_at_epoch_ms=0,"
                "effect_last_error='' WHERE c.status='PENDING_PREPARED' AND "
                "EXISTS (SELECT 1 FROM PARKING_SESSION s WHERE "
                "s.exit_command_id=c.command_id);");
        }
        executeSqlUnlocked(
            "UPDATE OCCUPANCY_COMMAND_INBOX SET status='PENDING_UNPREPARED',"
            "result_code='',result_session_id=NULL,effect_state='NONE',"
            "effect_attempt_count=0,effect_next_attempt_at_epoch_ms=0,"
            "effect_last_error='',next_attempt_at_epoch_ms=0,"
            "last_error='legacy prepared outcome requires replay' "
            "WHERE status='PENDING_PREPARED';");
        executeSqlUnlocked(
            "CREATE UNIQUE INDEX IF NOT EXISTS ux_occupancy_source_identity "
            "ON OCCUPANCY_COMMAND_INBOX(source_kind,source_identity);");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_occupancy_inbox_runnable "
            "ON OCCUPANCY_COMMAND_INBOX(status,slot_id,admission_ordinal,"
            "next_attempt_at_epoch_ms);");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_occupancy_effect_pending "
            "ON OCCUPANCY_COMMAND_INBOX(effect_state,slot_id,"
            "admission_ordinal,effect_next_attempt_at_epoch_ms);");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS OCCUPANCY_SENSOR_SEQUENCE_STATE ("
            "sensor_id TEXT PRIMARY KEY,protocol_mode TEXT NOT NULL CHECK "
            "(protocol_mode IN ('LEGACY','VERSIONED')),active_boot_id TEXT,"
            "last_sequence TEXT NOT NULL,last_command_id TEXT NOT NULL,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS IVA_SLOT_OBSERVATION_STATE ("
            "slot_id TEXT PRIMARY KEY,occupancy_attempt_id TEXT NOT NULL DEFAULT '',"
            "active_session_id INTEGER,observation_generation INTEGER NOT NULL DEFAULT 0,"
            "observed_state TEXT NOT NULL CHECK (observed_state IN "
            "('UNKNOWN','OCCUPIED','VACANT_PENDING','VACANT')),"
            "configured_areas_json TEXT NOT NULL DEFAULT '[]',"
            "area_states_json TEXT NOT NULL DEFAULT '{}',"
            "last_source_command_id TEXT NOT NULL DEFAULT '',"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "FOREIGN KEY(slot_id) REFERENCES PARKING_SLOT(slot_id));");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS OCCUPANCY_EXIT_DEADLINE ("
            "deadline_id TEXT PRIMARY KEY,slot_id TEXT NOT NULL,"
            "occupancy_attempt_id TEXT NOT NULL,expected_session_id INTEGER NOT NULL,"
            "observation_generation INTEGER NOT NULL,due_at_epoch_ms INTEGER NOT NULL,"
            "occupancy_policy TEXT NOT NULL DEFAULT 'CAMERA_IVA' CHECK "
            "(occupancy_policy IN ('CAMERA_IVA','HYBRID_OR')),"
            "state TEXT NOT NULL CHECK (state IN "
            "('SCHEDULED','ADMITTED','SUPERSEDED','APPLIED')),"
            "admitted_command_id TEXT UNIQUE,"
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "FOREIGN KEY(slot_id) REFERENCES PARKING_SLOT(slot_id),"
            "FOREIGN KEY(expected_session_id) REFERENCES PARKING_SESSION(session_id));");
        if (!tableHasColumn(db_, "OCCUPANCY_EXIT_DEADLINE",
                            "occupancy_policy")) {
            executeSqlUnlocked(
                "ALTER TABLE OCCUPANCY_EXIT_DEADLINE ADD COLUMN "
                "occupancy_policy TEXT NOT NULL DEFAULT 'CAMERA_IVA' CHECK "
                "(occupancy_policy IN ('CAMERA_IVA','HYBRID_OR'));");
        }
        executeSqlUnlocked(
            "CREATE UNIQUE INDEX IF NOT EXISTS ux_occupancy_live_deadline_slot "
            "ON OCCUPANCY_EXIT_DEADLINE(slot_id) "
            "WHERE state IN ('SCHEDULED','ADMITTED');");
        executeSqlUnlocked(
            "CREATE TABLE IF NOT EXISTS PARKING_CORRELATION_BINDING ("
            "correlation_id TEXT PRIMARY KEY,"
            "source_command_id TEXT NOT NULL UNIQUE,"
            "occupancy_attempt_id TEXT NOT NULL,"
            "session_id INTEGER,slot_id TEXT NOT NULL,"
            "camera_id TEXT NOT NULL,video_source_token TEXT NOT NULL,"
            "rule_name TEXT NOT NULL,object_id TEXT NOT NULL,"
            "channel_id TEXT NOT NULL,"
            "binding_revision INTEGER NOT NULL CHECK(binding_revision>0),"
            "state TEXT NOT NULL CHECK(state IN "
            "('PENDING','COMMITTED','ENDED','FAILED','EXPIRED','QUARANTINED')),"
            "created_at_epoch_ms INTEGER NOT NULL,"
            "expires_at_epoch_ms INTEGER NOT NULL,"
            "updated_at_epoch_ms INTEGER NOT NULL,ended_at_epoch_ms INTEGER,"
            "CHECK(state!='COMMITTED' OR (session_id IS NOT NULL AND "
            "occupancy_attempt_id!='')),"
            "UNIQUE(occupancy_attempt_id,camera_id,video_source_token,"
            "rule_name,object_id),"
            "FOREIGN KEY(session_id) REFERENCES PARKING_SESSION(session_id),"
            "FOREIGN KEY(slot_id) REFERENCES PARKING_SLOT(slot_id));");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_parking_correlation_bestshot_lookup "
            "ON PARKING_CORRELATION_BINDING(camera_id,channel_id,object_id,"
            "state,expires_at_epoch_ms,correlation_id);");
        executeSqlUnlocked(
            "CREATE INDEX IF NOT EXISTS idx_parking_correlation_session "
            "ON PARKING_CORRELATION_BINDING(session_id,state,correlation_id);");
        if (tableHasColumn(db_, "IMAGE_LOG", "image_id")) {
            if (!tableHasColumn(db_, "IMAGE_LOG", "correlation_id"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN correlation_id TEXT;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "camera_object_id"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN camera_object_id TEXT;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "image_ref"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN image_ref TEXT;");
            if (!tableHasColumn(
                    db_, "IMAGE_LOG", "correlation_binding_revision"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN "
                    "correlation_binding_revision INTEGER;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_x"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_x REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_y"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_y REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_width"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_width REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_height"))
                executeSqlUnlocked("ALTER TABLE IMAGE_LOG ADD COLUMN roi_height REAL;");
            if (!tableHasColumn(db_, "IMAGE_LOG", "roi_revision"))
                executeSqlUnlocked(
                    "ALTER TABLE IMAGE_LOG ADD COLUMN roi_revision INTEGER;");
            executeSqlUnlocked(
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "ux_image_bestshot_correlation ON IMAGE_LOG("
                "correlation_id,enhancement_type,image_ref) WHERE "
                "correlation_id IS NOT NULL AND enhancement_type IS NOT NULL "
                "AND image_ref IS NOT NULL;");
        }
        // Install the actor identity invariant only after the additive column
        // migration has completed.
        if (tableHasColumn(db_, "PARKING_SESSION", "entry_command_id") &&
            tableHasColumn(db_, "PARKING_SESSION", "occupancy_attempt_id") &&
            tableHasColumn(db_, "PARKING_SESSION", "entry_time_epoch_ms")) {
            executeSqlUnlocked(
                "CREATE TRIGGER IF NOT EXISTS "
                "tr_actor_session_identity_insert BEFORE INSERT ON "
                "PARKING_SESSION WHEN NEW.entry_command_id IS NOT NULL AND "
                "(NEW.occupancy_attempt_id IS NULL OR "
                "NEW.occupancy_attempt_id='' OR NEW.entry_time_epoch_ms IS "
                "NULL) BEGIN SELECT RAISE(ABORT,'actor session identity "
                "missing'); END;");
        }
        }
        if (tableHasColumn(db_, "IMAGE_LOG", "image_id") &&
            !tableHasColumn(db_, "IMAGE_LOG", "evidence_reason")) {
            executeSqlUnlocked(
                "ALTER TABLE IMAGE_LOG ADD COLUMN evidence_reason TEXT;");
        }
        if (tableHasColumn(db_, "IMAGE_LOG", "evidence_reason")) {
            executeSqlUnlocked(
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "ux_image_evidence_session_reason "
                "ON IMAGE_LOG(session_id, evidence_reason) "
                "WHERE session_id IS NOT NULL AND evidence_reason IS NOT NULL;");
        }
        Statement foreign_key_check(db_, "PRAGMA foreign_key_check;");
        if (sqlite3_step(foreign_key_check.get()) == SQLITE_ROW) {
            throw std::runtime_error(
                "runtime schema migration found a foreign-key violation");
        }
        executeSqlUnlocked("COMMIT;");
        runtime_schema_ready_ = true;
        occupancy_schema_ready_ = has_parking_slot;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        runtime_schema_ready_ = false;
        occupancy_schema_ready_ = false;
        throw;
    }
}

std::optional<std::string> EventDatabase::getSystemSetting(
    const std::string& key) const {
    if (key.empty()) return std::nullopt;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;
    Statement statement(db_,
        "SELECT value FROM SYSTEM_SETTINGS WHERE key=? LIMIT 1;");
    statement.bindText(1, key);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return columnText(statement.get(), 0);
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error("SQLite setting read failed: " +
                             std::string(sqlite3_errmsg(db_)));
}

bool EventDatabase::upsertSystemSetting(const std::string& key,
                                        const std::string& value) {
    if (key.empty()) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;
    try {
        Statement statement(db_,
            "INSERT INTO SYSTEM_SETTINGS(key,value,updated_at) "
            "VALUES(?,?,CURRENT_TIMESTAMP) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value, "
            "updated_at=CURRENT_TIMESTAMP;");
        statement.bindText(1, key);
        statement.bindText(2, value);
        requireDone(db_, statement.get());
        return true;
    } catch (...) {
        return false;
    }
}

std::int64_t EventDatabase::createHallSession(
    const std::string& slot_id,
    const std::string& source_id,
    const std::string& entry_time) {
    if (slot_id.empty() || entry_time.empty()) {
        throw std::invalid_argument("hall session fields must not be empty");
    }
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        throw std::runtime_error("cannot create session in a closed database");
    }
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement slot(db_,
            "UPDATE PARKING_SLOT SET status='OCCUPIED', updated_at=? "
            "WHERE slot_id=?;");
        slot.bindText(1, entry_time);
        slot.bindText(2, slot_id);
        requireDone(db_, slot.get());
        if (sqlite3_changes(db_) != 1) {
            throw std::runtime_error("hall slot does not exist: " + slot_id);
        }

        Statement session(db_,
            "INSERT INTO PARKING_SESSION(vehicle_id,slot_id,plate_number,"
            "entry_time,status) VALUES(NULL,?,NULL,?,'ACTIVE');");
        session.bindText(1, slot_id);
        session.bindText(2, entry_time);
        requireDone(db_, session.get());
        const auto session_id = sqlite3_last_insert_rowid(db_);

        Statement event(db_,
            "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
            "VALUES(?,?,'HALL_OCCUPIED',?);");
        event.bindInt64(1, session_id);
        event.bindText(2, slot_id);
        event.bindText(3, "hall sensor=" + source_id);
        requireDone(db_, event.get());
        executeSqlUnlocked("COMMIT;");
        return session_id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

EvidenceInsertResult EventDatabase::insertEvidenceImage(
    const std::int64_t session_id,
    const std::string& original_path,
    const std::string& evidence_reason,
    const std::string& captured_at,
    const std::string& enhanced_path,
    const snapshot::NormalizedRoi applied_roi,
    const std::uint64_t roi_revision) {
    if (session_id < 0 || original_path.empty() || captured_at.empty() ||
        (evidence_reason != "OCCUPANCY_START_EVIDENCE" &&
         evidence_reason != "OVERSTAY_EVIDENCE")) {
        throw std::invalid_argument("invalid evidence image fields");
    }
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) {
        throw std::runtime_error("cannot insert evidence in a closed database");
    }
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement existing(db_,
            "SELECT 1 FROM IMAGE_LOG WHERE session_id=? AND evidence_reason=? "
            "LIMIT 1;");
        existing.bindInt64(1, session_id);
        existing.bindText(2, evidence_reason);
        if (sqlite3_step(existing.get()) == SQLITE_ROW) {
            executeSqlUnlocked("COMMIT;");
            return EvidenceInsertResult::Duplicate;
        }

        Statement active(db_,
            "SELECT 1 FROM PARKING_SESSION WHERE session_id=? "
            "AND status IN ('ACTIVE','VIOLATION') AND exit_time IS NULL;");
        active.bindInt64(1, session_id);
        if (sqlite3_step(active.get()) != SQLITE_ROW) {
            executeSqlUnlocked("COMMIT;");
            return EvidenceInsertResult::InactiveSession;
        }

        Statement image(db_,
            "INSERT INTO IMAGE_LOG(session_id,original_image_path,"
            "enhanced_image_path,enhancement_type,evidence_reason,captured_at,"
            "roi_x,roi_y,roi_width,roi_height,roi_revision) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?);");
        image.bindInt64(1, session_id);
        image.bindText(2, original_path);
        if (enhanced_path.empty()) {
            if (sqlite3_bind_null(image.get(), 3) != SQLITE_OK)
                throw std::runtime_error("SQLite evidence enhanced NULL bind failed");
        } else {
            image.bindText(3, enhanced_path);
        }
        image.bindText(4, enhanced_path.empty() ? "NONE" : "CAMERA_AUTO");
        image.bindText(5, evidence_reason);
        image.bindText(6, captured_at);
        if (roi_revision > 0) {
            if (sqlite3_bind_double(image.get(), 7, applied_roi.x) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 8, applied_roi.y) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 9, applied_roi.width) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 10, applied_roi.height) != SQLITE_OK ||
                sqlite3_bind_int64(image.get(), 11,
                                   static_cast<sqlite3_int64>(roi_revision)) != SQLITE_OK)
                throw std::runtime_error("SQLite evidence ROI bind failed");
        } else {
            for (int index = 7; index <= 11; ++index)
                if (sqlite3_bind_null(image.get(), index) != SQLITE_OK)
                    throw std::runtime_error("SQLite evidence ROI NULL bind failed");
        }
        requireDone(db_, image.get());
        executeSqlUnlocked("COMMIT;");
        return EvidenceInsertResult::Inserted;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

std::optional<std::string> EventDatabase::findEvidenceImagePath(
    const std::int64_t session_id,
    const std::string& evidence_reason) const {
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;
    Statement statement(db_,
        "SELECT original_image_path FROM IMAGE_LOG WHERE session_id=? "
        "AND evidence_reason=? ORDER BY image_id LIMIT 1;");
    statement.bindInt64(1, session_id);
    statement.bindText(2, evidence_reason);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) {
        throw std::runtime_error("SQLite evidence lookup failed: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    return columnText(statement.get(), 0);
}

EvidenceInsertResult EventDatabase::insertHallCaptureImage(
    const std::int64_t session_id,
    const std::string& original_path,
    const std::string& enhanced_path,
    const std::string& enhancement_type,
    const std::string& captured_at,
    const snapshot::NormalizedRoi applied_roi,
    const std::uint64_t roi_revision) {
    if (session_id < 0 || original_path.empty() || captured_at.empty() ||
        (enhancement_type != "HALL_30S" &&
         enhancement_type != "HALL_60S")) {
        throw std::invalid_argument("invalid hall capture image fields");
    }
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr)
        throw std::runtime_error("cannot insert hall capture in a closed database");

    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement existing(db_,
            "SELECT 1 FROM IMAGE_LOG WHERE session_id=? "
            "AND enhancement_type=? LIMIT 1;");
        existing.bindInt64(1, session_id);
        existing.bindText(2, enhancement_type);
        if (sqlite3_step(existing.get()) == SQLITE_ROW) {
            executeSqlUnlocked("COMMIT;");
            return EvidenceInsertResult::Duplicate;
        }

        Statement active(db_,
            "SELECT 1 FROM PARKING_SESSION WHERE session_id=? "
            "AND status IN ('ACTIVE','VIOLATION') AND exit_time IS NULL;");
        active.bindInt64(1, session_id);
        if (sqlite3_step(active.get()) != SQLITE_ROW) {
            executeSqlUnlocked("COMMIT;");
            return EvidenceInsertResult::InactiveSession;
        }

        Statement image(db_,
            "INSERT INTO IMAGE_LOG(session_id,original_image_path,"
            "enhanced_image_path,enhancement_type,captured_at,roi_x,roi_y,"
            "roi_width,roi_height,roi_revision) VALUES(?,?,?,?,?,?,?,?,?,?);");
        image.bindInt64(1, session_id);
        image.bindText(2, original_path);
        if (enhanced_path.empty()) {
            if (sqlite3_bind_null(image.get(), 3) != SQLITE_OK)
                throw std::runtime_error("SQLite hall enhanced NULL bind failed");
        } else {
            image.bindText(3, enhanced_path);
        }
        image.bindText(4, enhancement_type);
        image.bindText(5, captured_at);
        if (roi_revision > 0) {
            if (sqlite3_bind_double(image.get(), 6, applied_roi.x) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 7, applied_roi.y) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 8, applied_roi.width) != SQLITE_OK ||
                sqlite3_bind_double(image.get(), 9, applied_roi.height) != SQLITE_OK ||
                sqlite3_bind_int64(image.get(), 10,
                                   static_cast<sqlite3_int64>(roi_revision)) != SQLITE_OK)
                throw std::runtime_error("SQLite hall ROI bind failed");
        } else {
            for (int index = 6; index <= 10; ++index)
                if (sqlite3_bind_null(image.get(), index) != SQLITE_OK)
                    throw std::runtime_error("SQLite hall ROI NULL bind failed");
        }
        requireDone(db_, image.get());

        Statement event(db_,
            "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
            "SELECT ?,slot_id,'HALL_CAPTURE_STORED',? "
            "FROM PARKING_SESSION WHERE session_id=?;");
        event.bindInt64(1, session_id);
        event.bindText(2, "type=" + enhancement_type +
                               " image=" + original_path);
        event.bindInt64(3, session_id);
        requireDone(db_, event.get());
        executeSqlUnlocked("COMMIT;");
        return EvidenceInsertResult::Inserted;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

bool EventDatabase::markPlateOcrUnresolved(
    const std::int64_t session_id,
    const std::string& slot_id,
    const int attempts) {
    if (session_id < 0 || attempts < 1) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    Statement existing(db_,
        "SELECT 1 FROM EVENT_LOG WHERE session_id=? "
        "AND event_type='PLATE_OCR_UNRESOLVED' LIMIT 1;");
    existing.bindInt64(1, session_id);
    if (sqlite3_step(existing.get()) == SQLITE_ROW) return true;

    Statement event(db_,
        "INSERT INTO EVENT_LOG(session_id,slot_id,event_type,message) "
        "VALUES(?,?,'PLATE_OCR_UNRESOLVED',?);");
    event.bindInt64(1, session_id);
    if (slot_id.empty()) {
        if (sqlite3_bind_null(event.get(), 2) != SQLITE_OK) return false;
    } else {
        event.bindText(2, slot_id);
    }
    event.bindText(3, "ocr_status=FAILED ev_status=UNKNOWN attempts=" +
                           std::to_string(attempts));
    requireDone(db_, event.get());
    return true;
}

/**
 * @brief 차량번호를 마스터 테이블에서 조회해 EV 분류를 반환한다.
 *
 * @param[in] car_number 조회할 차량번호.
 * @return `EV`, 일반차 또는 미등록(`Unknown`) 분류.
 * @throws std::runtime_error SQLite 조회가 실패한 경우.
 */
VehicleCategory EventDatabase::classifyVehicle(const std::string_view car_number) const {
    std::lock_guard lock(db_mutex_);
    Statement statement(db_, "SELECT is_ev FROM VEHICLE WHERE plate_number = ?;");
    statement.bindText(1, car_number);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return VehicleCategory::Unknown;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error("SQLite vehicle lookup failed: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    if (sqlite3_column_int(statement.get(), 0) == 1) {
        return VehicleCategory::Ev;
    }
    return VehicleCategory::NonEv;
}

/**
 * @brief EV 입차 세션을 `PARKED` 상태로 INSERT한다.
 *
 * @param[in] car_number 차량번호.
 * @param[in] slot_id 주차면 ID.
 * @param[in] parked_at 최초 주차 판정 UTC 시각.
 * @param[in] image_path_1 최초 증거 이미지 경로.
 * @return 새 `timer_log` 행의 64비트 ID.
 * @throws std::runtime_error SQL 제약조건 또는 실행 오류가 발생한 경우.
 */
std::int64_t EventDatabase::insertParked(const std::string& car_number,
                                         const std::string& slot_id,
                                         const std::string& parked_at,
                                         const std::string& image_path_1) {
    std::lock_guard lock(db_mutex_);
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement statement(
            db_,
            "INSERT INTO PARKING_SESSION(vehicle_id, slot_id, plate_number, entry_time, status) "
            "VALUES ((SELECT vehicle_id FROM VEHICLE WHERE plate_number = ?), ?, ?, ?, 'ACTIVE');");
        statement.bindText(1, car_number);
        statement.bindText(2, slot_id);
        statement.bindText(3, car_number);
        statement.bindText(4, parked_at);
        requireDone(db_, statement.get());
        const auto session_id = sqlite3_last_insert_rowid(db_);
        Statement image(db_, "INSERT INTO IMAGE_LOG(session_id, original_image_path, "
                             "enhancement_type) VALUES (?, ?, 'TIMER_ENTRY');");
        image.bindInt64(1, session_id);
        image.bindText(2, image_path_1);
        requireDone(db_, image.get());
        executeSqlUnlocked("COMMIT;");
        return session_id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

/**
 * @brief 아직 활성인 주차 세션을 위반 상태로 조건부 갱신한다.
 *
 * @param[in] log_id 갱신할 불변 주차 세션 ID.
 * @param[in] violation_at 최초 deadline 도달 UTC 시각.
 * @param[in] image_path_2 위반 증거 이미지 경로.
 * @return 정확히 한 행이 갱신됐으면 `true`. 이미 출차/취소됐으면 `false`.
 * @throws std::runtime_error SQLite UPDATE가 실패한 경우.
 * @note WHERE 조건 자체가 출차와 만료의 경합을 해결하는 최종 안전장치다.
 */
bool EventDatabase::markViolation(const std::int64_t log_id,
                                  const std::string& violation_at,
                                  const std::string& image_path_2) {
    std::lock_guard lock(db_mutex_);
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        Statement statement(db_, "UPDATE PARKING_SESSION SET status = 'VIOLATION', "
                                 "violation_at = ? WHERE session_id = ? "
                                 "AND status = 'ACTIVE' AND exit_time IS NULL;");
        statement.bindText(1, violation_at);
        statement.bindInt64(2, log_id);
        requireDone(db_, statement.get());
        const bool changed = sqlite3_changes(db_) == 1;
        if (changed && !image_path_2.empty()) {
            Statement existing(db_,
                "SELECT 1 FROM IMAGE_LOG WHERE session_id=? "
                "AND original_image_path=? LIMIT 1;");
            existing.bindInt64(1, log_id);
            existing.bindText(2, image_path_2);
            if (sqlite3_step(existing.get()) != SQLITE_ROW) {
                Statement image(db_,
                    "INSERT INTO IMAGE_LOG(session_id, original_image_path, "
                    "enhancement_type) VALUES (?, ?, 'TIMER_VIOLATION');");
                image.bindInt64(1, log_id);
                image.bindText(2, image_path_2);
                requireDone(db_, image.get());
            }
        }
        executeSqlUnlocked("COMMIT;");
        return changed;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

/**
 * @brief DB INSERT 후 큐 등록에 실패한 세션을 보상 종료한다.
 *
 * @param[in] log_id 종료할 주차 세션 ID.
 * @param[in] canceled_at 보상 종료 UTC 시각.
 * @return 활성 PARKED 행 한 개가 종료됐으면 `true`.
 * @throws std::runtime_error SQLite UPDATE가 실패한 경우.
 */
bool EventDatabase::cancelUnscheduled(const std::int64_t log_id,
                                      const std::string& canceled_at) {
    std::lock_guard lock(db_mutex_);
    Statement statement(
        db_,
        "UPDATE PARKING_SESSION SET status = 'ENDED', exit_time = ? "
        "WHERE session_id = ? AND status = 'ACTIVE' AND exit_time IS NULL;");
    statement.bindText(1, canceled_at);
    statement.bindInt64(2, log_id);
    requireDone(db_, statement.get());
    return sqlite3_changes(db_) == 1;
}

/**
 * @brief 지정 구역의 활성 세션을 출차 완료 상태로 원자적으로 갱신한다.
 *
 * @param[in] slot_id 출차가 감지된 주차면 ID.
 * @param[in] departed_at 출차 UTC 시각.
 * @return 갱신된 로그. 활성 세션이 없으면 `std::nullopt`.
 * @throws std::runtime_error 조회·UPDATE·재조회 중 하나라도 실패한 경우.
 * @note 위반 후 출차도 최종 status는 `DEPARTS`가 되지만 기존 `violation_at`은 보존한다.
 */
std::optional<LogRecord> EventDatabase::departActiveBySlot(
    const std::string& slot_id,
    const std::string& departed_at) {
    std::lock_guard lock(db_mutex_);
    // 조회와 UPDATE 사이에 다른 DB 쓰기가 끼어들지 않도록 write lock을 먼저 확보한다.
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        // partial unique index상 활성 행은 최대 하나지만, 최신 행을 명시적으로 선택한다.
        Statement find_statement(db_, std::string{kLogSelect} +
            "WHERE s.slot_id = ? AND s.exit_time IS NULL "
            "ORDER BY s.session_id DESC LIMIT 1;");
        find_statement.bindText(1, slot_id);
        const int find_result = sqlite3_step(find_statement.get());
        if (find_result == SQLITE_DONE) {
            executeSqlUnlocked("COMMIT;");
            return std::nullopt;
        }
        if (find_result != SQLITE_ROW) {
            throw std::runtime_error("SQLite active log lookup failed: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        const auto log_id = sqlite3_column_int64(find_statement.get(), 0);

        // is_canceled는 우선순위 큐 노드를 즉시 찾지 않고 나중에 버리기 위한 표시다.
        Statement update_statement(
            db_,
            "UPDATE PARKING_SESSION SET status = 'ENDED', exit_time = ?, "
            "duration_sec = MAX(0, CAST(strftime('%s', ?) AS INTEGER) - "
            "CAST(strftime('%s', entry_time) AS INTEGER)) "
            "WHERE session_id = ? AND exit_time IS NULL;");
        update_statement.bindText(1, departed_at);
        update_statement.bindText(2, departed_at);
        update_statement.bindInt64(3, log_id);
        requireDone(db_, update_statement.get());
        if (sqlite3_changes(db_) != 1) {
            throw std::runtime_error("active parking log changed concurrently");
        }

        Statement slot_statement(
            db_, "UPDATE PARKING_SLOT SET status='VACANT', updated_at=? "
                 "WHERE slot_id=?;");
        slot_statement.bindText(1, departed_at);
        slot_statement.bindText(2, slot_id);
        requireDone(db_, slot_statement.get());

        // 호출자와 이벤트 발행부가 최종 상태를 그대로 사용할 수 있도록 같은 트랜잭션에서 읽는다.
        Statement result_statement(db_, std::string{kLogSelect} +
            "WHERE s.session_id = ?;");
        result_statement.bindInt64(1, log_id);
        if (sqlite3_step(result_statement.get()) != SQLITE_ROW) {
            throw std::runtime_error("updated parking log disappeared");
        }
        auto record = readLogRecord(result_statement.get());
        executeSqlUnlocked("COMMIT;");
        return record;
    } catch (...) {
        // 원래 예외를 보존하기 위해 rollback 결과는 여기서 별도 예외로 바꾸지 않는다.
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

/**
 * @brief 지정 구역에서 아직 출차하지 않은 세션을 조회한다.
 *
 * @param[in] slot_id 조회할 주차면 ID.
 * @return 활성 로그 또는 활성 행이 없을 때 `std::nullopt`.
 * @throws std::runtime_error SQLite 조회가 실패한 경우.
 */
std::optional<LogRecord> EventDatabase::findActiveBySlot(
    const std::string& slot_id) const {
    std::lock_guard lock(db_mutex_);
    Statement statement(db_, std::string{kLogSelect} +
        "WHERE s.slot_id = ? AND s.exit_time IS NULL "
        "ORDER BY s.session_id DESC LIMIT 1;");
    statement.bindText(1, slot_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error("SQLite active log lookup failed: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    return readLogRecord(statement.get());
}

/**
 * @brief 고유 로그 ID로 주차 세션 한 건을 조회한다.
 *
 * @param[in] log_id 조회할 `timer_log.id`.
 * @return 해당 로그 또는 존재하지 않을 때 `std::nullopt`.
 * @throws std::runtime_error SQLite 조회가 실패한 경우.
 */
std::optional<LogRecord> EventDatabase::findLogById(const std::int64_t log_id) const {
    std::lock_guard lock(db_mutex_);
    Statement statement(db_, std::string{kLogSelect} +
        "WHERE s.session_id = ?;");
    statement.bindInt64(1, log_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error("SQLite log lookup failed: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    return readLogRecord(statement.get());
}

/**
 * @brief 모든 타이머 로그를 생성 순서대로 조회한다.
 *
 * @return `id` 오름차순의 로그 목록.
 * @throws std::runtime_error 조회 도중 SQLite 오류가 발생한 경우.
 */
std::vector<LogRecord> EventDatabase::listLogs() const {
    std::lock_guard lock(db_mutex_);
    Statement statement(db_, std::string{kLogSelect} +
        "ORDER BY s.session_id;");
    std::vector<LogRecord> records;
    while (true) {
        // sqlite3_step을 반복해 row를 모두 소비하고 SQLITE_DONE에서 정상 종료한다.
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return records;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error("SQLite log listing failed: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        records.push_back(readLogRecord(statement.get()));
    }
}

/**
 * @brief 차량 마스터의 차량번호와 차량 종류를 모두 조회한다.
 *
 * @return 차량번호 오름차순의 `(car_number, vehicle_type)` 목록.
 * @throws std::runtime_error 조회 도중 SQLite 오류가 발생한 경우.
 */
std::vector<std::pair<std::string, std::string>> EventDatabase::listVehicles() const {
    std::lock_guard lock(db_mutex_);
    Statement statement(
        db_, "SELECT plate_number, CASE WHEN is_ev=1 THEN 'EV' "
             "ELSE 'NON_EV' END "
             "FROM VEHICLE ORDER BY plate_number;");
    std::vector<std::pair<std::string, std::string>> vehicles;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return vehicles;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error("SQLite vehicle listing failed: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        vehicles.emplace_back(columnText(statement.get(), 0),
                              columnText(statement.get(), 1));
    }
}

/**
 * @brief 데모를 새로 시작할 수 있도록 타이머 로그와 자동 증가 번호를 초기화한다.
 *
 * @throws std::runtime_error DELETE SQL 실행이 실패한 경우.
 * @warning 운영 데이터에는 사용하지 말고 `--reset-logs` 데모 옵션에서만 사용한다.
 */
void EventDatabase::clearTimerLogs() {
    std::lock_guard lock(db_mutex_);
    executeSqlUnlocked(
        "BEGIN IMMEDIATE;"
        "CREATE TEMP TABLE IF NOT EXISTS timer_session_ids(session_id INTEGER PRIMARY KEY);"
        "DELETE FROM timer_session_ids;"
        "INSERT INTO timer_session_ids SELECT DISTINCT session_id FROM IMAGE_LOG "
        "WHERE enhancement_type='TIMER_ENTRY' AND session_id IS NOT NULL;"
        "DELETE FROM EVENT_LOG WHERE session_id IN (SELECT session_id FROM timer_session_ids);"
        "DELETE FROM IMAGE_LOG WHERE session_id IN (SELECT session_id FROM timer_session_ids);"
        "DELETE FROM PARKING_SESSION WHERE session_id IN (SELECT session_id FROM timer_session_ids);"
        "UPDATE PARKING_SLOT SET status='VACANT' WHERE slot_id NOT IN "
        "(SELECT slot_id FROM PARKING_SESSION WHERE exit_time IS NULL);"
        "DELETE FROM timer_session_ids;"
        "COMMIT;");
}

/**
 * @brief 준비 과정이 필요 없는 SQL 문자열을 SQLite 연결에서 직접 실행한다.
 *
 * @param[in] sql 실행할 하나 이상의 SQL 문장.
 * @throws std::runtime_error SQLite 실행이 실패한 경우.
 * @warning 이 함수는 mutex를 잡지 않는다. 호출자는 필요할 경우 `mutex_`를 이미
 *          보유해야 하며, 생성자처럼 객체가 아직 공유되지 않은 시점에만 예외가 가능하다.
 */
void EventDatabase::executeSqlUnlocked(const std::string& sql) {
    char* raw_error{};
    const int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &raw_error);
    if (result == SQLITE_OK) {
        return;
    }
    // sqlite3_exec이 할당한 오류 문자열은 메시지를 복사한 뒤 sqlite3_free로 해제한다.
    const std::string error = raw_error == nullptr ? sqlite3_errmsg(db_) : raw_error;
    sqlite3_free(raw_error);
    throw std::runtime_error("SQLite execution failed: " + error);
}

/**
 * @brief SQL/config 보조 파일 전체를 문자열로 읽는다.
 *
 * @param[in] path 읽을 파일 경로.
 * @return 파일의 전체 바이트 내용.
 * @throws std::runtime_error 파일을 열 수 없는 경우.
 */
std::string EventDatabase::readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open SQL file: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

}  // namespace database

#include "parking_timer/EventDatabase.hpp"

#include <sqlite3.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace parking_timer {
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
    Statement(sqlite3* database, const char* sql) : database_(database) {
        const int result = sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr);
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
    record.zone_id = sqlite3_column_int(statement, 2);
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

    // 한 연결을 CLI와 타이머 worker가 공유하므로 SQLite 자체도 serialized 모드로 연다.
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int result = sqlite3_open_v2(database_path.string().c_str(), &db_, flags, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = db_ == nullptr ? "unknown SQLite error" :
                                                    sqlite3_errmsg(db_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("cannot open SQLite database: " + message);
    }

    // PRAGMA 중 하나라도 실패하면 아직 완성되지 않은 연결을 즉시 닫는다.
    try {
        executeSqlUnlocked("PRAGMA foreign_keys = ON;");
        executeSqlUnlocked("PRAGMA busy_timeout = 3000;");
        executeSqlUnlocked("PRAGMA journal_mode = WAL;");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

/**
 * @brief 보유한 SQLite 연결을 닫는다.
 */
EventDatabase::~EventDatabase() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
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
    std::lock_guard lock(mutex_);
    // 스키마는 IF NOT EXISTS로 멱등하며, seed 전체만 별도 원자적 단위로 처리한다.
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

/**
 * @brief 차량번호를 마스터 테이블에서 조회해 EV 분류를 반환한다.
 *
 * @param[in] car_number 조회할 차량번호.
 * @return `EV`, `PHEV`, 일반차 또는 미등록(`Unknown`) 분류.
 * @throws std::runtime_error SQLite 조회가 실패한 경우.
 */
VehicleCategory EventDatabase::classifyVehicle(const std::string_view car_number) const {
    std::lock_guard lock(mutex_);
    Statement statement(db_,
                        "SELECT vehicle_type FROM mock_vehicle_master "
                        "WHERE car_number = ?;");
    statement.bindText(1, car_number);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return VehicleCategory::Unknown;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error("SQLite vehicle lookup failed: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    const auto type = columnText(statement.get(), 0);
    if (type == "EV") {
        return VehicleCategory::Ev;
    }
    if (type == "PHEV") {
        return VehicleCategory::Phev;
    }
    return VehicleCategory::NonEv;
}

/**
 * @brief EV/PHEV 입차 세션을 `PARKED` 상태로 INSERT한다.
 *
 * @param[in] car_number 차량번호.
 * @param[in] zone_id 충전구역 ID.
 * @param[in] parked_at 최초 주차 판정 UTC 시각.
 * @param[in] image_path_1 최초 증거 이미지 경로.
 * @return 새 `timer_log` 행의 64비트 ID.
 * @throws std::runtime_error SQL 제약조건 또는 실행 오류가 발생한 경우.
 */
std::int64_t EventDatabase::insertParked(const std::string& car_number,
                                         const int zone_id,
                                         const std::string& parked_at,
                                         const std::string& image_path_1) {
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        "INSERT INTO timer_log(car_number, zone_id, status, parked_at, image_path_1) "
        "VALUES (?, ?, 'PARKED', ?, ?);");
    statement.bindText(1, car_number);
    statement.bindInt(2, zone_id);
    statement.bindText(3, parked_at);
    statement.bindText(4, image_path_1);
    requireDone(db_, statement.get());
    return sqlite3_last_insert_rowid(db_);
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
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        // 출차가 먼저 처리됐다면 조건이 맞지 않아 0행이 되고 위반 이벤트도 발생하지 않는다.
        "UPDATE timer_log "
        "SET status = 'VIOLATION', violation_at = ?, image_path_2 = ? "
        "WHERE id = ? AND status = 'PARKED' AND departed_at IS NULL "
        "AND is_canceled = 0;");
    statement.bindText(1, violation_at);
    statement.bindText(2, image_path_2);
    statement.bindInt64(3, log_id);
    requireDone(db_, statement.get());
    return sqlite3_changes(db_) == 1;
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
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        "UPDATE timer_log SET status = 'DEPARTS', departed_at = ?, is_canceled = 1 "
        "WHERE id = ? AND status = 'PARKED' AND departed_at IS NULL;");
    statement.bindText(1, canceled_at);
    statement.bindInt64(2, log_id);
    requireDone(db_, statement.get());
    return sqlite3_changes(db_) == 1;
}

/**
 * @brief 지정 구역의 활성 세션을 출차 완료 상태로 원자적으로 갱신한다.
 *
 * @param[in] zone_id 출차가 감지된 충전구역 ID.
 * @param[in] departed_at 출차 UTC 시각.
 * @return 갱신된 로그. 활성 세션이 없으면 `std::nullopt`.
 * @throws std::runtime_error 조회·UPDATE·재조회 중 하나라도 실패한 경우.
 * @note 위반 후 출차도 최종 status는 `DEPARTS`가 되지만 기존 `violation_at`은 보존한다.
 */
std::optional<LogRecord> EventDatabase::departActiveByZone(
    const int zone_id,
    const std::string& departed_at) {
    std::lock_guard lock(mutex_);
    // 조회와 UPDATE 사이에 다른 DB 쓰기가 끼어들지 않도록 write lock을 먼저 확보한다.
    executeSqlUnlocked("BEGIN IMMEDIATE;");
    try {
        // partial unique index상 활성 행은 최대 하나지만, 최신 행을 명시적으로 선택한다.
        Statement find_statement(
            db_,
            "SELECT id, car_number, zone_id, status, parked_at, violation_at, "
            "departed_at, image_path_1, image_path_2, is_canceled "
            "FROM timer_log WHERE zone_id = ? AND departed_at IS NULL "
            "ORDER BY id DESC LIMIT 1;");
        find_statement.bindInt(1, zone_id);
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
            "UPDATE timer_log SET status = 'DEPARTS', departed_at = ?, "
            "is_canceled = 1 WHERE id = ? AND departed_at IS NULL;");
        update_statement.bindText(1, departed_at);
        update_statement.bindInt64(2, log_id);
        requireDone(db_, update_statement.get());
        if (sqlite3_changes(db_) != 1) {
            throw std::runtime_error("active parking log changed concurrently");
        }

        // 호출자와 이벤트 발행부가 최종 상태를 그대로 사용할 수 있도록 같은 트랜잭션에서 읽는다.
        Statement result_statement(
            db_,
            "SELECT id, car_number, zone_id, status, parked_at, violation_at, "
            "departed_at, image_path_1, image_path_2, is_canceled "
            "FROM timer_log WHERE id = ?;");
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
 * @param[in] zone_id 조회할 충전구역 ID.
 * @return 활성 로그 또는 활성 행이 없을 때 `std::nullopt`.
 * @throws std::runtime_error SQLite 조회가 실패한 경우.
 */
std::optional<LogRecord> EventDatabase::findActiveByZone(const int zone_id) const {
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        "SELECT id, car_number, zone_id, status, parked_at, violation_at, "
        "departed_at, image_path_1, image_path_2, is_canceled "
        "FROM timer_log WHERE zone_id = ? AND departed_at IS NULL "
        "ORDER BY id DESC LIMIT 1;");
    statement.bindInt(1, zone_id);
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
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        "SELECT id, car_number, zone_id, status, parked_at, violation_at, "
        "departed_at, image_path_1, image_path_2, is_canceled "
        "FROM timer_log WHERE id = ?;");
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
    std::lock_guard lock(mutex_);
    Statement statement(
        db_,
        "SELECT id, car_number, zone_id, status, parked_at, violation_at, "
        "departed_at, image_path_1, image_path_2, is_canceled "
        "FROM timer_log ORDER BY id;");
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
    std::lock_guard lock(mutex_);
    Statement statement(
        db_, "SELECT car_number, vehicle_type FROM mock_vehicle_master ORDER BY car_number;");
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
    std::lock_guard lock(mutex_);
    executeSqlUnlocked("DELETE FROM timer_log;");
    executeSqlUnlocked("DELETE FROM sqlite_sequence WHERE name = 'timer_log';");
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

}  // namespace parking_timer

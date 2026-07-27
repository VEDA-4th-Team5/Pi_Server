#include "database/db_manager.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static sqlite3 *g_db = NULL;

sqlite3 *db_native_handle(void)
{
    return g_db;
}

/* 공개 DB 함수가 연결 없이 호출되는 실수를 공통으로 검사한다. */
static int require_db(const char *context)
{
    if (g_db != NULL) return 0;
    fprintf(stderr, "[DB] %s 실패: DB가 열려 있지 않습니다.\n", context);
    return -1;
}

/* 선택 입력 문자열은 NULL을 SQLite NULL로 그대로 보존한다. */
static int bind_text_or_null(sqlite3_stmt *stmt, int index, const char *value)
{
    return value != NULL
        ? sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT)
        : sqlite3_bind_null(stmt, index);
}

/* 음수 ID는 연결 대상이 없는 이벤트/이미지를 의미하므로 NULL로 저장한다. */
static int bind_id_or_null(sqlite3_stmt *stmt, int index, int value)
{
    return value >= 0
        ? sqlite3_bind_int(stmt, index, value)
        : sqlite3_bind_null(stmt, index);
}

static void copy_column_text(sqlite3_stmt *stmt, int column,
                             char *destination, size_t capacity)
{
    const unsigned char *value = sqlite3_column_text(stmt, column);
    if (capacity == 0) return;
    if (value == NULL) {
        destination[0] = '\0';
        return;
    }
    snprintf(destination, capacity, "%s", (const char *)value);
}

/* SQL prepare 실패 로그 형식을 모든 CRUD 함수에서 통일한다. */
static int prepare(sqlite3_stmt **stmt, const char *sql, const char *context)
{
    int rc;
    if (require_db(context) < 0) return -1;
    rc = sqlite3_prepare_v2(g_db, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] %s prepare 실패: %s\n", context, sqlite3_errmsg(g_db));
        return -2;
    }
    return 0;
}

/* UPDATE/INSERT 실행, statement 해제 및 필요 시 변경 행 존재 여부를 검사한다. */
static int finish_update(sqlite3_stmt *stmt, const char *context, int require_change)
{
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[DB] %s 실행 실패: %s\n", context, sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    sqlite3_finalize(stmt);
    if (require_change && sqlite3_changes(g_db) == 0) {
        fprintf(stderr, "[DB] %s 실패: 대상 행이 없습니다.\n", context);
        return -4;
    }
    return 0;
}

int db_open(const char *path)
{
    char *error = NULL;
    int rc;
    if (path == NULL) {
        fprintf(stderr, "[DB] DB 경로가 NULL입니다.\n");
        return -1;
    }
    /* 중복 open 시 이전 연결이 남지 않게 먼저 닫는다. */
    db_close();
    rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] DB 열기 실패 (%s): %s\n", path,
                g_db != NULL ? sqlite3_errmsg(g_db) : "unknown error");
        db_close();
        return -2;
    }
    /* SQLite는 연결마다 foreign key 검사를 켜야 한다. */
    rc = sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] foreign key 활성화 실패: %s\n",
                error != NULL ? error : sqlite3_errmsg(g_db));
        sqlite3_free(error);
        db_close();
        return -3;
    }
    return 0;
}

void db_close(void)
{
    if (g_db == NULL) return;
    if (sqlite3_close(g_db) != SQLITE_OK) {
        fprintf(stderr, "[DB] DB 닫기 실패: %s\n", sqlite3_errmsg(g_db));
        return;
    }
    g_db = NULL;
}

int db_get_vehicle_by_plate(const char *plate_number, int *vehicle_id,
                            int *is_ev, int *is_phev)
{
    static const char *sql =
        "SELECT vehicle_id, is_ev, is_phev FROM VEHICLE WHERE plate_number = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (plate_number == NULL || vehicle_id == NULL || is_ev == NULL ||
        is_phev == NULL) return -1;
    if (prepare(&stmt, sql, "차량 조회") < 0) return -2;
    if (sqlite3_bind_text(stmt, 1, plate_number, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        fprintf(stderr, "[DB] 차량 조회 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        fprintf(stderr, "[DB] 등록 차량을 찾을 수 없습니다: %s\n", plate_number);
        sqlite3_finalize(stmt);
        return -4;
    }
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "[DB] 차량 조회 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -5;
    }
    *vehicle_id = sqlite3_column_int(stmt, 0);
    *is_ev = sqlite3_column_int(stmt, 1);
    *is_phev = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);
    return 0;
}

int db_update_slot_status(const char *slot_id, const char *status)
{
    static const char *sql =
        "UPDATE PARKING_SLOT SET status = ?, updated_at = CURRENT_TIMESTAMP "
        "WHERE slot_id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (slot_id == NULL || status == NULL) return -1;
    if (prepare(&stmt, sql, "주차면 상태 갱신") < 0) return -2;
    if (sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, slot_id, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        fprintf(stderr, "[DB] 주차면 상태 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "주차면 상태 갱신", 1);
}

int db_create_parking_session(int vehicle_id, const char *slot_id,
                              const char *plate_number, int *session_id)
{
    static const char *sql =
        "INSERT INTO PARKING_SESSION(vehicle_id, slot_id, plate_number, status) "
        "VALUES(?, ?, ?, 'ACTIVE');";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (slot_id == NULL || session_id == NULL) return -1;
    if (prepare(&stmt, sql, "주차 세션 생성") < 0) return -2;
    rc = bind_id_or_null(stmt, 1, vehicle_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, slot_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 3, plate_number);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] 주차 세션 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    rc = finish_update(stmt, "주차 세션 생성", 0);
    if (rc < 0) return rc;
    *session_id = (int)sqlite3_last_insert_rowid(g_db);
    return 0;
}

int db_end_parking_session(int session_id)
{
    static const char *sql =
        "UPDATE PARKING_SESSION SET "
        "exit_time = CURRENT_TIMESTAMP, "
        "duration_sec = MAX(0, CAST(strftime('%s', CURRENT_TIMESTAMP) AS INTEGER) "
        "- CAST(strftime('%s', entry_time) AS INTEGER)), "
        "status = 'ENDED' WHERE session_id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (session_id < 0) return -1;
    if (prepare(&stmt, sql, "주차 세션 종료") < 0) return -2;
    if (sqlite3_bind_int(stmt, 1, session_id) != SQLITE_OK) {
        fprintf(stderr, "[DB] 주차 세션 종료 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "주차 세션 종료", 1);
}

int db_insert_image_log(int session_id, const char *original_path,
                        const char *enhanced_path, const char *enhancement_type,
                        const char *ocr_result)
{
    static const char *sql =
        "INSERT INTO IMAGE_LOG(session_id, original_image_path, enhanced_image_path, "
        "enhancement_type, ocr_result) VALUES(?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (prepare(&stmt, sql, "이미지 로그 입력") < 0) return -1;
    rc = bind_id_or_null(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 2, original_path);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 3, enhanced_path);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 4, enhancement_type);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 5, ocr_result);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] 이미지 로그 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -2;
    }
    return finish_update(stmt, "이미지 로그 입력", 0);
}

int db_insert_event_log(int session_id, const char *slot_id,
                        const char *event_type, const char *message)
{
    static const char *sql =
        "INSERT INTO EVENT_LOG(session_id, slot_id, event_type, message) "
        "VALUES(?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (event_type == NULL) return -1;
    if (prepare(&stmt, sql, "이벤트 로그 입력") < 0) return -2;
    rc = bind_id_or_null(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 2, slot_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, event_type, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 4, message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] 이벤트 로그 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "이벤트 로그 입력", 0);
}

int db_mark_event_handled(int event_id)
{
    static const char *sql = "UPDATE EVENT_LOG SET handled = 1 WHERE event_id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (event_id < 0) return -1;
    if (prepare(&stmt, sql, "이벤트 처리 완료") < 0) return -2;
    if (sqlite3_bind_int(stmt, 1, event_id) != SQLITE_OK) {
        fprintf(stderr, "[DB] 이벤트 처리 bind 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "이벤트 처리 완료", 1);
}

int db_update_image_ocr_by_path(const char *original_path, const char *ocr_result)
{
    static const char *sql =
        "UPDATE IMAGE_LOG SET ocr_result = ? WHERE original_image_path = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (original_path == NULL) return -1;
    if (prepare(&stmt, sql, "이미지 OCR 결과 갱신") < 0) return -2;
    rc = bind_text_or_null(stmt, 1, ocr_result);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 2, original_path, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "이미지 OCR 결과 갱신", 1);
}

int db_update_image_enhanced_by_path(const char *original_path,
                                     const char *enhanced_path)
{
    static const char *sql =
        "UPDATE IMAGE_LOG SET enhanced_image_path = ?, enhancement_type = ? "
        "WHERE original_image_path = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (original_path == NULL || enhanced_path == NULL) return -1;
    if (prepare(&stmt, sql, "전처리 이미지 경로 갱신") < 0) return -2;
    rc = sqlite3_bind_text(stmt, 1, enhanced_path, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 2, "OPENCV_CLAHE_UNSHARP", -1,
                               SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 3, original_path, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "전처리 이미지 경로 갱신", 1);
}

int db_assign_vehicle_to_session(int session_id, int vehicle_id,
                                 const char *plate_number)
{
    static const char *sql =
        "UPDATE PARKING_SESSION SET vehicle_id = ?, plate_number = ? "
        "WHERE session_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (session_id < 0) return -1;
    if (prepare(&stmt, sql, "주차 세션 OCR 차량 연결") < 0) return -2;
    rc = bind_id_or_null(stmt, 1, vehicle_id);
    if (rc == SQLITE_OK) rc = bind_text_or_null(stmt, 2, plate_number);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, session_id);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "주차 세션 OCR 차량 연결", 1);
}

int db_visit_parking_slots(const char *slot_id, DbParkingSlotVisitor visitor,
                           void *context)
{
    static const char *sql_all =
        "SELECT p.slot_id,p.slot_type,p.status,COALESCE(p.sensor_type,''),"
        "COALESCE(p.updated_at,''),s.session_id,COALESCE(s.plate_number,''),"
        "COALESCE(s.entry_time,''),v.is_ev FROM PARKING_SLOT p "
        "LEFT JOIN PARKING_SESSION s ON s.session_id=(SELECT session_id FROM "
        "PARKING_SESSION WHERE slot_id=p.slot_id "
        "AND status IN ('ACTIVE','VIOLATION') "
        "ORDER BY entry_time DESC,session_id DESC LIMIT 1) "
        "LEFT JOIN VEHICLE v ON v.vehicle_id=s.vehicle_id ORDER BY p.slot_id;";
    static const char *sql_one =
        "SELECT p.slot_id,p.slot_type,p.status,COALESCE(p.sensor_type,''),"
        "COALESCE(p.updated_at,''),s.session_id,COALESCE(s.plate_number,''),"
        "COALESCE(s.entry_time,''),v.is_ev FROM PARKING_SLOT p "
        "LEFT JOIN PARKING_SESSION s ON s.session_id=(SELECT session_id FROM "
        "PARKING_SESSION WHERE slot_id=p.slot_id "
        "AND status IN ('ACTIVE','VIOLATION') "
        "ORDER BY entry_time DESC,session_id DESC LIMIT 1) "
        "LEFT JOIN VEHICLE v ON v.vehicle_id=s.vehicle_id WHERE p.slot_id=?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;
    if (visitor == NULL) return -1;
    if (prepare(&stmt, slot_id == NULL ? sql_all : sql_one,
                "주차면 API 조회") < 0) return -2;
    if (slot_id != NULL &&
        sqlite3_bind_text(stmt, 1, slot_id, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbParkingSlotRow row;
        memset(&row, 0, sizeof(row));
        copy_column_text(stmt, 0, row.slot_id, sizeof(row.slot_id));
        copy_column_text(stmt, 1, row.slot_type, sizeof(row.slot_type));
        copy_column_text(stmt, 2, row.parking_status, sizeof(row.parking_status));
        copy_column_text(stmt, 3, row.sensor_type, sizeof(row.sensor_type));
        copy_column_text(stmt, 4, row.updated_at, sizeof(row.updated_at));
        row.has_active_session = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
        row.session_id = row.has_active_session ? sqlite3_column_int(stmt, 5) : -1;
        copy_column_text(stmt, 6, row.plate_number, sizeof(row.plate_number));
        copy_column_text(stmt, 7, row.entry_time, sizeof(row.entry_time));
        row.has_vehicle_classification = sqlite3_column_type(stmt, 8) != SQLITE_NULL;
        row.is_ev = row.has_vehicle_classification ? sqlite3_column_int(stmt, 8) : -1;
        ++count;
        if (visitor(&row, context) != 0) break;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "[DB] 주차면 API 조회 실패: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -3;
    }
    sqlite3_finalize(stmt);
    return count;
}

static void fill_image_row(sqlite3_stmt *stmt, DbImageRow *row)
{
    memset(row, 0, sizeof(*row));
    row->image_id = sqlite3_column_int(stmt, 0);
    row->session_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL
        ? -1 : sqlite3_column_int(stmt, 1);
    copy_column_text(stmt, 2, row->original_path, sizeof(row->original_path));
    copy_column_text(stmt, 3, row->enhanced_path, sizeof(row->enhanced_path));
    copy_column_text(stmt, 4, row->enhancement_type, sizeof(row->enhancement_type));
    copy_column_text(stmt, 5, row->ocr_result, sizeof(row->ocr_result));
    copy_column_text(stmt, 6, row->captured_at, sizeof(row->captured_at));
}

int db_visit_session_images(int session_id, DbImageVisitor visitor, void *context)
{
    static const char *sql =
        "SELECT image_id,session_id,COALESCE(original_image_path,''),"
        "COALESCE(enhanced_image_path,''),COALESCE(enhancement_type,''),"
        "COALESCE(ocr_result,''),COALESCE(captured_at,'') FROM IMAGE_LOG "
        "WHERE session_id=? ORDER BY captured_at,image_id;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;
    if (session_id < 0 || visitor == NULL) return -1;
    if (prepare(&stmt, sql, "세션 이미지 API 조회") < 0) return -2;
    if (sqlite3_bind_int(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbImageRow row;
        fill_image_row(stmt, &row);
        ++count;
        if (visitor(&row, context) != 0) break;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -3;
    }
    sqlite3_finalize(stmt);
    return count;
}

int db_delete_session_images(int session_id)
{
    static const char *sql = "DELETE FROM IMAGE_LOG WHERE session_id=?;";
    sqlite3_stmt *stmt = NULL;
    if (session_id < 0) return -1;
    if (prepare(&stmt, sql, "세션 이미지 로그 삭제") < 0) return -2;
    if (sqlite3_bind_int(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    return finish_update(stmt, "세션 이미지 로그 삭제", 0);
}

int db_get_image_by_id(int image_id, DbImageRow *row)
{
    static const char *sql =
        "SELECT image_id,session_id,COALESCE(original_image_path,''),"
        "COALESCE(enhanced_image_path,''),COALESCE(enhancement_type,''),"
        "COALESCE(ocr_result,''),COALESCE(captured_at,'') FROM IMAGE_LOG "
        "WHERE image_id=?;";
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (image_id < 0 || row == NULL) return -1;
    if (prepare(&stmt, sql, "이미지 API 단건 조회") < 0) return -2;
    if (sqlite3_bind_int(stmt, 1, image_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -3;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -4;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -3;
    }
    fill_image_row(stmt, row);
    sqlite3_finalize(stmt);
    return 0;
}

#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

struct sqlite3;

#define DB_TEXT_SMALL 64
#define DB_TEXT_PATH 1024

typedef struct DbParkingSlotRow {
    char slot_id[DB_TEXT_SMALL];
    char slot_type[DB_TEXT_SMALL];
    char parking_status[DB_TEXT_SMALL];
    char sensor_type[DB_TEXT_SMALL];
    char updated_at[DB_TEXT_SMALL];
    int has_active_session;
    int session_id;
    char plate_number[DB_TEXT_SMALL];
    char entry_time[DB_TEXT_SMALL];
    int has_vehicle_classification;
    int is_ev;
} DbParkingSlotRow;

typedef struct DbImageRow {
    int image_id;
    int session_id;
    char original_path[DB_TEXT_PATH];
    char enhanced_path[DB_TEXT_PATH];
    char enhancement_type[DB_TEXT_SMALL];
    char ocr_result[DB_TEXT_SMALL];
    char captured_at[DB_TEXT_SMALL];
} DbImageRow;

typedef int (*DbParkingSlotVisitor)(const DbParkingSlotRow *row, void *context);
typedef int (*DbImageVisitor)(const DbImageRow *row, void *context);

/* 프로세스에서 사용할 SQLite DB 하나를 열고 foreign key 검사를 활성화한다. */
int db_open(const char *path);
/* 열려 있는 전역 DB 연결을 안전하게 닫는다. */
void db_close(void);
/* 번호판으로 등록 차량 ID와 전기차 여부를 조회한다. */
int db_get_vehicle_by_plate(const char *plate_number, int *vehicle_id,
                            int *is_ev, int *is_phev);
/* 지정 주차면의 VACANT/OCCUPIED/ERROR 상태를 변경한다. */
int db_update_slot_status(const char *slot_id, const char *status);
/* 입차 세션을 만들고 생성된 session_id를 호출자에게 돌려준다. */
int db_create_parking_session(int vehicle_id, const char *slot_id,
                              const char *plate_number, int *session_id);
/* 출차 시각과 주차 시간을 계산하고 세션을 ENDED로 바꾼다. */
int db_end_parking_session(int session_id);
/* 원본/보정 이미지 및 OCR 결과를 IMAGE_LOG에 기록한다. */
int db_insert_image_log(int session_id, const char *original_path,
                        const char *enhanced_path, const char *enhancement_type,
                        const char *ocr_result);
/* 카메라·센서·주차 상태 이벤트를 EVENT_LOG에 기록한다. */
int db_insert_event_log(int session_id, const char *slot_id,
                        const char *event_type, const char *message);
/* 관제에서 확인한 이벤트의 handled 값을 1로 바꾼다. */
int db_mark_event_handled(int event_id);
/* 이미지 경로로 해당 IMAGE_LOG의 OCR 결과를 갱신한다. */
int db_update_image_ocr_by_path(const char *original_path, const char *ocr_result);
/* 원본 이미지 행에 OpenCV 전처리 이미지 경로를 연결한다. */
int db_update_image_enhanced_by_path(const char *original_path,
                                     const char *enhanced_path);
/* OCR 번호판과 등록 차량을 기존 주차 세션에 연결한다. */
int db_assign_vehicle_to_session(int session_id, int vehicle_id,
                                 const char *plate_number);
int db_visit_parking_slots(const char *slot_id, DbParkingSlotVisitor visitor,
                           void *context);
int db_visit_session_images(int session_id, DbImageVisitor visitor, void *context);
int db_delete_session_images(int session_id);
int db_get_image_by_id(int image_id, DbImageRow *row);
/* 통합 C++ 저장 계층에서 같은 연결로 transaction/query를 수행할 때 사용한다. */
struct sqlite3 *db_native_handle(void);

#ifdef __cplusplus
}
#endif

#endif

#include "database/db_manager.h"

#include <stdio.h>

static int check(const char *name, int result)
{
    printf("%-34s %s (%d)\n", name, result == 0 ? "성공" : "실패", result);
    return result;
}

int main(int argc, char **argv)
{
    /* DB 공개 API를 실제 주차 흐름 순서대로 호출하는 통합형 smoke test다.
     * 전달받은 DB를 변경하므로 운영 DB가 아닌 초기화된 테스트 DB에서 실행해야 한다. */
    int vehicle_id = -1;
    int is_ev = -1;
    int is_phev = -1;
    int session_id = -1;
    int failed = 0;

    const char *db_path = argc > 1 ? argv[1] : "data/db/test_parking.db";
    if (check("db_open", db_open(db_path)) < 0) return 1;
    if (check("db_get_vehicle_by_plate",
              db_get_vehicle_by_plate("12가3456", &vehicle_id, &is_ev,
                                      &is_phev)) < 0) {
        failed = 1;
    } else {
        printf("차량 조회: vehicle_id=%d, is_ev=%d, is_phev=%d\n",
               vehicle_id, is_ev, is_phev);
    }
    if (check("db_update_slot_status",
              db_update_slot_status("P01", "OCCUPIED")) < 0) failed = 1;
    if (check("db_create_parking_session",
              db_create_parking_session(vehicle_id, "P01", "12가3456", &session_id)) < 0) {
        failed = 1;
    } else {
        printf("생성된 session_id=%d\n", session_id);
    }
    if (check("db_insert_image_log",
              db_insert_image_log(session_id, "snapshots/original.jpg",
                                  "enhanced/enhanced.jpg", "BRIGHTNESS",
                                  "12가3456")) < 0) failed = 1;
    if (check("db_insert_event_log",
              db_insert_event_log(session_id, "P01", "GENERAL_SLOT_OCCUPIED",
                                  "P01 주차중 상태 변경")) < 0) failed = 1;
    if (check("db_end_parking_session", db_end_parking_session(session_id)) < 0)
        failed = 1;
    if (check("db_mark_event_handled", db_mark_event_handled(1)) < 0)
        failed = 1;

    db_close();
    printf("MVP DB Manager 테스트 %s\n", failed ? "실패" : "완료");
    return failed;
}

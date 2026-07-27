#pragma once

#include "parking_timer/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct sqlite3;

namespace database {

/** @brief 카메라/MQTT 이벤트와 증거 경로를 DB에 전달하는 입력 레코드다. */
struct EventRecord {
    std::string camera_id;
    std::string channel_id;
    std::string slot_id;
    std::string source_type;
    std::string source_id;
    std::string event_type;
    std::string severity;
    double confidence;
    std::string snapshot_path;
    std::string clip_path;
    std::string raw_topic;
    std::string raw_payload;
    std::string payload_json;
    std::string created_at;
};

/** @brief HTTP API가 사용하는 주차면과 현재 활성 세션의 읽기 모델이다. */
struct ParkingSlotView {
    std::string slot_id;
    std::string slot_type;
    std::string parking_status;
    std::string sensor_type;
    std::string updated_at;
    int session_id{-1};
    std::string plate_number;
    std::string entry_time;
    int is_ev{-1};
};

/** @brief IMAGE_LOG 한 행을 파일 API가 사용하기 좋은 문자열로 표현한다. */
struct ImageView {
    int image_id{-1};
    int session_id{-1};
    std::string original_path;
    std::string enhanced_path;
    std::string enhancement_type;
    std::string ocr_result;
    std::string captured_at;
};

/**
 * @brief 카메라·OCR·HTTP·타이머가 공유하는 단일 SQLite 저장 서비스다.
 *
 * C db_manager와 타이머용 prepared statement가 같은 SQLite 연결을 사용한다.
 * 공개 메서드는 db_mutex_로 직렬화되며 객체 복사 대신 프로세스당 한 인스턴스를 사용한다.
 */
class EventDatabase {
public:
    EventDatabase();
    explicit EventDatabase(const std::filesystem::path& database_path);
    ~EventDatabase();

    /** @brief SQLite 파일을 열고 FK 검사를 활성화한다. */
    bool open(const std::string& db_path);
    /** @brief 열린 DB 연결을 닫는다. */
    void close();
    /** @brief 정규화된 카메라 이벤트와 선택적 Snapshot을 IMAGE_LOG/EVENT_LOG에 기록한다. */
    bool insertEvent(const EventRecord& record);
    /** @brief 센서·통신 운영 이벤트를 기존 EVENT_LOG schema에 저장한다. */
    bool insertSystemEvent(const std::string& event_type,
                           const std::string& slot_id,
                           const std::string& message);
    /** @brief Vehicle BestShot을 근거로 OCCUPIED 슬롯과 ACTIVE 세션을 원자적으로 연결한다. */
    bool createEntryWithBestShot(const std::string& slot_id,
                                 const std::string& image_path,
                                 const std::string& object_id,
                                 int* session_id);
    /** @brief 홀센서 입차 Snapshot으로 ACTIVE 세션과 IMAGE/EVENT 로그를 만든다. */
    bool createEntryWithSnapshot(const std::string& slot_id,
                                 const std::string& image_path,
                                 const std::string& source_id,
                                 int* session_id);
    /** @brief 번호판 BestShot과 선택적 카메라 plate text를 기존 세션에 연결한다. */
    bool attachPlateBestShot(int session_id,
                             const std::string& image_path,
                             const std::string& plate_text);
    /** @brief 원본 IMAGE_LOG 행에 OpenCV 전처리 파일 경로를 연결한다. */
    bool attachEnhancedPlateImage(const std::string& image_path,
                                  const std::string& enhanced_image_path);
    /** @brief OCR 결과를 저장하고 VEHICLE 조회 결과(EV/NON_EV/UNKNOWN)를 반환한다. */
    std::string applyPlateOcr(int session_id,
                              const std::string& slot_id,
                              const std::string& image_path,
                              const std::string& plate_number,
                              double confidence);
    /** @brief 전체 주차면과 활성 세션을 조회한다. */
    bool listParkingSlots(std::vector<ParkingSlotView>& rows);
    /** @brief slot_id 한 건의 상태를 조회한다. */
    bool getParkingSlot(const std::string& slot_id, ParkingSlotView& row);
    /** @brief 세션에 연결된 모든 이미지 메타데이터를 조회한다. */
    bool listSessionImages(int session_id, std::vector<ImageView>& rows);
    /** @brief image_id로 이미지 경로와 OCR 메타데이터를 조회한다. */
    bool getImage(int image_id, ImageView& row);
    /** @brief 파일 삭제가 끝난 조기 출차 세션의 IMAGE_LOG 행을 모두 제거한다. */
    bool deleteSessionImageRecords(int session_id);

    /** @brief schema와 seed SQL을 적용하며 구형 컬럼을 먼저 호환 마이그레이션한다. */
    void initialize(const std::filesystem::path& schema_file,
                    const std::filesystem::path& seed_file);
    /** @brief VEHICLE의 is_ev/is_phev로 차량 종류를 분류한다. */
    parking_timer::VehicleCategory classifyVehicle(std::string_view car_number) const;
    /** @brief EV/PHEV 장기 점유용 ACTIVE 세션과 최초 이미지를 트랜잭션으로 생성한다. */
    std::int64_t insertParked(const std::string& car_number,
                              const std::string& slot_id,
                              const std::string& parked_at,
                              const std::string& image_path_1);
    /** @brief 아직 ACTIVE인 세션만 VIOLATION으로 조건부 갱신한다. */
    bool markViolation(std::int64_t log_id, const std::string& violation_at,
                       const std::string& image_path_2);
    /** @brief DB 생성 뒤 timer enqueue 실패 시 세션을 보상 종료한다. */
    bool cancelUnscheduled(std::int64_t log_id, const std::string& canceled_at);
    /** @brief slot_id의 활성 세션을 ENDED로 바꾸고 점유시간을 계산한다. */
    std::optional<parking_timer::LogRecord> departActiveBySlot(
        const std::string& slot_id, const std::string& departed_at);
    /** @brief 주차면의 출차되지 않은 최신 세션을 조회한다. */
    std::optional<parking_timer::LogRecord> findActiveBySlot(
        const std::string& slot_id) const;
    /** @brief 불변 session ID로 타이머 읽기 모델을 조회한다. */
    std::optional<parking_timer::LogRecord> findLogById(std::int64_t log_id) const;
    /** @brief 타이머 CLI 표시용 전체 세션을 생성 순서로 반환한다. */
    std::vector<parking_timer::LogRecord> listLogs() const;
    /** @brief 차량번호와 EV/PHEV/NON_EV 문자열 목록을 반환한다. */
    std::vector<std::pair<std::string, std::string>> listVehicles() const;
    /** @brief TIMER_ENTRY로 식별되는 데모 타이머 세션만 정리한다. */
    void clearTimerLogs();

private:
    void executeSqlUnlocked(const std::string& sql);
    static std::string readTextFile(const std::filesystem::path& path);

    bool opened_;
    std::string db_path_;
    mutable std::mutex db_mutex_;
    sqlite3* db_{};
};

}

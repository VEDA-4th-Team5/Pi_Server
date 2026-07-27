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

struct ImageView {
    int image_id{-1};
    int session_id{-1};
    std::string original_path;
    std::string enhanced_path;
    std::string enhancement_type;
    std::string ocr_result;
    std::string captured_at;
};

class EventDatabase {
public:
    EventDatabase();
    explicit EventDatabase(const std::filesystem::path& database_path);
    ~EventDatabase();

    bool open(const std::string& db_path);
    void close();
    bool insertEvent(const EventRecord& record);
    bool createEntryWithBestShot(const std::string& slot_id,
                                 const std::string& image_path,
                                 const std::string& object_id,
                                 int* session_id);
    bool attachPlateBestShot(int session_id,
                             const std::string& image_path,
                             const std::string& plate_text);
    bool attachEnhancedPlateImage(const std::string& image_path,
                                  const std::string& enhanced_image_path);
    std::string applyPlateOcr(int session_id,
                              const std::string& slot_id,
                              const std::string& image_path,
                              const std::string& plate_number,
                              double confidence);

    // --- 홀센서 점유 경로 (EVDA-136) ---
    // 촬영/OCR/이미지/이벤트가 모두 여기서 만든 하나의 session_id에 매달린다.

    // OCCUPIED 확정 시 주차면을 점유로 바꾸고 PARKING_SESSION 행을 연다.
    // 같은 주차면에 이미 활성 세션이 있으면 unique index가 막으므로 중복 세션은
    // 생기지 않는다.
    bool openHallSession(const std::string& slot_id, int* session_id);
    // VACANT 시 세션을 종료하고 주차면을 비움으로 되돌린다.
    bool closeHallSession(int session_id, const std::string& slot_id);
    // 30초/60초 촬영본을 세션의 증거로 남긴다. enhancement_type은
    // HALL_30S / HALL_60S (촬영 규약 §7). OCR을 돌리지 않는 증거 전용
    // 이미지도 반드시 기록한다.
    bool attachCaptureImage(int session_id,
                            const std::string& slot_id,
                            const std::string& original_path,
                            const std::string& enhanced_path,
                            const std::string& enhancement_type);
    // 재시도까지 실패해 번호판을 읽지 못한 세션을 UNKNOWN으로 확정한다.
    // vehicle_id/plate_number를 건드리지 않으므로 조회 API는 NON_EV가 아니라
    // UNKNOWN을 보고한다 (촬영 규약 §8). 차량은 아직 서 있으므로 세션 상태는
    // ACTIVE로 두어 1시간 타이머를 끊지 않는다.
    bool markPlateOcrUnresolved(int session_id, const std::string& slot_id,
                                int attempts);
    bool listParkingSlots(std::vector<ParkingSlotView>& rows);
    bool getParkingSlot(const std::string& slot_id, ParkingSlotView& row);
    bool listSessionImages(int session_id, std::vector<ImageView>& rows);
    bool getImage(int image_id, ImageView& row);

    void initialize(const std::filesystem::path& schema_file,
                    const std::filesystem::path& seed_file);
    parking_timer::VehicleCategory classifyVehicle(std::string_view car_number) const;
    std::int64_t insertParked(const std::string& car_number,
                              const std::string& slot_id,
                              const std::string& parked_at,
                              const std::string& image_path_1);
    bool markViolation(std::int64_t log_id, const std::string& violation_at,
                       const std::string& image_path_2);
    bool cancelUnscheduled(std::int64_t log_id, const std::string& canceled_at);
    std::optional<parking_timer::LogRecord> departActiveBySlot(
        const std::string& slot_id, const std::string& departed_at);
    std::optional<parking_timer::LogRecord> findActiveBySlot(
        const std::string& slot_id) const;
    std::optional<parking_timer::LogRecord> findLogById(std::int64_t log_id) const;
    std::vector<parking_timer::LogRecord> listLogs() const;
    std::vector<std::pair<std::string, std::string>> listVehicles() const;
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

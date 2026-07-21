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

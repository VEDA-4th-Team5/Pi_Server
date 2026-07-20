#pragma once

#include <mutex>
#include <string>
#include <vector>

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

private:
    bool opened_;
    std::string db_path_;
    std::mutex db_mutex_;
};

}

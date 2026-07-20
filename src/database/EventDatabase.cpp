#include "database/EventDatabase.hpp"

#include "database/db_manager.h"
#include "util/Logger.hpp"

#include <sstream>

namespace database {

EventDatabase::EventDatabase()
    : opened_(false) {
}

EventDatabase::~EventDatabase() {
    close();
}

bool EventDatabase::open(const std::string& db_path) {
    // C DB manager가 전역 연결 하나를 사용하므로 모든 접근을 같은 mutex로 직렬화한다.
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (opened_) {
        db_close();
        opened_ = false;
    }

    db_path_ = db_path;
    if (db_open(db_path_.c_str()) < 0) {
        util::logError("MVP parking DB open failed: " + db_path_);
        return false;
    }

    opened_ = true;
    util::logInfo("MVP parking DB opened: " + db_path_);
    return true;
}

void EventDatabase::close() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_) return;
    db_close();
    opened_ = false;
}

bool EventDatabase::insertEvent(const EventRecord& record) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_) {
        util::logError("MVP parking DB insert failed: DB is not open");
        return false;
    }

    const char* slot_id = record.slot_id.empty() ? nullptr : record.slot_id.c_str();
    bool success = true;

    // MQTT 이벤트는 아직 주차 세션 판정 전이므로 session_id 없이 증거 이미지를 기록한다.
    if (!record.snapshot_path.empty()) {
        if (db_insert_image_log(
                -1,
                record.snapshot_path.c_str(),
                nullptr,
                "NONE",
                nullptr) < 0) {
            util::logError("MVP IMAGE_LOG insert failed: " + record.snapshot_path);
            success = false;
        }
    }

    // MVP EVENT_LOG schema에 없는 카메라 상세 필드는 사람이 읽을 수 있는 message에 담는다.
    std::string message =
        "camera_id=" + record.camera_id +
        " channel_id=" + record.channel_id +
        " source=" + record.source_type +
        " snapshot=" + record.snapshot_path;

    if (db_insert_event_log(
            -1,
            slot_id,
            record.event_type.c_str(),
            message.c_str()) < 0) {
        util::logError("MVP EVENT_LOG insert failed: " + record.event_type);
        success = false;
    }

    return success;
}

bool EventDatabase::createEntryWithBestShot(const std::string& slot_id,
                                            const std::string& image_path,
                                            const std::string& object_id,
                                            int* session_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_ || slot_id.empty() || image_path.empty() || session_id == nullptr)
        return false;

    // 차량 BestShot을 입차 근거로 사용해 주차면, 세션, 이미지, 이벤트를 차례로 만든다.
    if (db_update_slot_status(slot_id.c_str(), "OCCUPIED") < 0 ||
        db_create_parking_session(-1, slot_id.c_str(), nullptr, session_id) < 0 ||
        db_insert_image_log(*session_id, image_path.c_str(), nullptr,
                            "BESTSHOT_VEHICLE", nullptr) < 0) {
        util::logError("BestShot entry DB update failed: slot=" + slot_id);
        return false;
    }

    std::string message = "vehicle BestShot object_id=" + object_id +
                          " image=" + image_path;
    if (db_insert_event_log(*session_id, slot_id.c_str(), "VEHICLE_ENTERED",
                            message.c_str()) < 0) {
        util::logError("BestShot entry event insert failed: slot=" + slot_id);
        return false;
    }
    return true;
}

bool EventDatabase::attachPlateBestShot(int session_id,
                                        const std::string& image_path,
                                        const std::string& plate_text) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_ || session_id < 0 || image_path.empty()) return false;
    // 번호판 문자열이 없더라도 이미지 자체는 해당 입차 세션의 증거로 보존한다.
    const char* ocr = plate_text.empty() ? nullptr : plate_text.c_str();
    if (db_insert_image_log(session_id, image_path.c_str(), nullptr,
                            "BESTSHOT_PLATE", ocr) < 0) {
        util::logError("Plate BestShot DB insert failed: session=" +
                       std::to_string(session_id));
        return false;
    }
    return true;
}

bool EventDatabase::attachEnhancedPlateImage(
    const std::string& image_path,
    const std::string& enhanced_image_path) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_ || image_path.empty() || enhanced_image_path.empty()) return false;
    if (db_update_image_enhanced_by_path(image_path.c_str(),
                                         enhanced_image_path.c_str()) < 0) {
        util::logError("Enhanced IMAGE_LOG update failed: " + image_path);
        return false;
    }
    return true;
}

std::string EventDatabase::applyPlateOcr(int session_id,
                                         const std::string& slot_id,
                                         const std::string& image_path,
                                         const std::string& plate_number,
                                         double confidence) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!opened_) return "DB_ERROR";

    const char* ocr = plate_number.empty() ? nullptr : plate_number.c_str();
    if (db_update_image_ocr_by_path(image_path.c_str(), ocr) < 0)
        util::logError("OCR IMAGE_LOG update failed: " + image_path);

    std::string classification = "OCR_FAILED";
    int vehicle_id = -1;
    int is_ev = -1;
    if (!plate_number.empty()) {
        int lookup = db_get_vehicle_by_plate(plate_number.c_str(), &vehicle_id, &is_ev);
        classification = lookup == 0 ? (is_ev ? "EV" : "NON_EV") : "UNKNOWN";
        if (session_id >= 0 &&
            db_assign_vehicle_to_session(session_id,
                                         lookup == 0 ? vehicle_id : -1,
                                         plate_number.c_str()) < 0)
            util::logError("OCR parking session update failed: session=" +
                           std::to_string(session_id));
    }

    std::ostringstream message;
    message << "plate=" << plate_number << " classification=" << classification
            << " confidence=" << confidence << " image=" << image_path;
    db_insert_event_log(session_id, slot_id.empty() ? nullptr : slot_id.c_str(),
                        ("PLATE_OCR_" + classification).c_str(),
                        message.str().c_str());
    return classification;
}

namespace {
int collectSlot(const DbParkingSlotRow* source, void* context) {
    auto* rows = static_cast<std::vector<ParkingSlotView>*>(context);
    ParkingSlotView row;
    row.slot_id = source->slot_id;
    row.slot_type = source->slot_type;
    row.parking_status = source->parking_status;
    row.sensor_type = source->sensor_type;
    row.updated_at = source->updated_at;
    row.session_id = source->has_active_session ? source->session_id : -1;
    row.plate_number = source->plate_number;
    row.entry_time = source->entry_time;
    row.is_ev = source->has_vehicle_classification ? source->is_ev : -1;
    rows->push_back(std::move(row));
    return 0;
}
int collectImage(const DbImageRow* source, void* context) {
    auto* rows = static_cast<std::vector<ImageView>*>(context);
    rows->push_back({source->image_id, source->session_id, source->original_path,
                     source->enhanced_path, source->enhancement_type,
                     source->ocr_result, source->captured_at});
    return 0;
}
}

bool EventDatabase::listParkingSlots(std::vector<ParkingSlotView>& rows) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    rows.clear();
    return opened_ && db_visit_parking_slots(nullptr, collectSlot, &rows) >= 0;
}

bool EventDatabase::getParkingSlot(const std::string& slot_id, ParkingSlotView& row) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<ParkingSlotView> rows;
    const int count = opened_ ? db_visit_parking_slots(slot_id.c_str(), collectSlot, &rows) : -1;
    if (count != 1 || rows.empty()) return false;
    row = std::move(rows.front());
    return true;
}

bool EventDatabase::listSessionImages(int session_id, std::vector<ImageView>& rows) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    rows.clear();
    return opened_ && db_visit_session_images(session_id, collectImage, &rows) >= 0;
}

bool EventDatabase::getImage(int image_id, ImageView& row) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    DbImageRow source;
    if (!opened_ || db_get_image_by_id(image_id, &source) < 0) return false;
    row = {source.image_id, source.session_id, source.original_path,
           source.enhanced_path, source.enhancement_type, source.ocr_result,
           source.captured_at};
    return true;
}

}

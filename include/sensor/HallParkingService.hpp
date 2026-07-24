#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/SystemEventReporter.hpp"
#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotManager.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sensor {

/** @brief MQTT test/UART 공통 홀센서 메시지를 Snapshot·OCR·타이머 흐름으로 연결한다. */
class HallParkingService {
public:
    using OcrEnqueue = std::function<void(
        int, const std::string&, const std::string&)>;
    using OcrCancel = std::function<void(int)>;

    HallParkingService(
        std::vector<parking::ParkingSlotConfig> slot_configs,
        const app::AppConfig& app_config,
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        snapshot::SnapshotStorage& snapshot_storage,
        database::EventDatabase& database,
        OcrEnqueue ocr_enqueue,
        OcrCancel ocr_cancel,
        parking_timer::ParkingSlotManager& timer_manager,
        parking_timer::EventManager& event_manager,
        event::SystemEventReporter* system_event_reporter = nullptr);

    /** @brief SENSOR:HALLxx:OCCUPIED/VACANT 메시지 한 줄을 처리한다. */
    bool handleLine(const std::string& line,
                    const std::string& transport = "mqtt-test");

private:
    bool handleOccupied(const parking::ParkingSensorEvent& event);
    bool handleVacant(const parking::ParkingSensorEvent& event);
    bool removeEarlyDepartureImages(std::int64_t session_id);
    void report(event::SystemEventCode code,
                event::SystemEventSeverity severity,
                const std::string& message,
                const std::string& transport,
                const std::string& slot_id = {}) noexcept;

    std::vector<parking::ParkingSlotConfig> slot_configs_;
    parking::SensorSlotIndex slot_index_;
    SensorProtocolParser parser_;
    ParkingSensorEventAdapter adapter_;
    parking::ParkingSensorSequenceGuard sequence_guard_;
    parking::ParkingSlotManager occupancy_manager_;
    const app::AppConfig& app_config_;
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    snapshot::SnapshotStorage& snapshot_storage_;
    database::EventDatabase& database_;
    OcrEnqueue ocr_enqueue_;
    OcrCancel ocr_cancel_;
    parking_timer::ParkingSlotManager& timer_manager_;
    parking_timer::EventManager& event_manager_;
    event::SystemEventReporter* system_event_reporter_{};
    std::mutex mutex_;
};

}  // namespace sensor

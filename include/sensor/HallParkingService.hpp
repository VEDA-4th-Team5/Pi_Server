#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/SystemEventReporter.hpp"
#include "parking/ParkingOccupancyConfirmationGate.hpp"
#include "parking/EvidenceCaptureWorker.hpp"
#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotManager.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sensor {

struct HallParkingWorkItem {
    parking::ParkingSensorEvent event;
    parking::ParkingTransitionResult transition;
};

/** @brief 슬롯별 최신 상태를 병합하는 비동기 작업용 bounded queue다. */
class HallParkingWorkQueue {
public:
    enum class PushResult { Added, Coalesced, Full };

    explicit HallParkingWorkQueue(std::size_t capacity);
    [[nodiscard]] bool canAccept(const std::string& slot_id) const;
    [[nodiscard]] PushResult push(HallParkingWorkItem item);
    [[nodiscard]] std::optional<HallParkingWorkItem> pop();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    std::size_t capacity_;
    std::deque<HallParkingWorkItem> items_;
};

/** @brief MQTT test/UART 공통 홀센서 메시지를 Snapshot·OCR·타이머 흐름으로 연결한다. */
class HallParkingService {
public:
    using OcrCancel = std::function<void(int)>;
    using TransitionSink =
        std::function<void(const parking::ParkingTransitionResult&)>;

    HallParkingService(
        std::vector<parking::ParkingSlotConfig> slot_configs,
        const app::AppConfig& app_config,
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        database::EventDatabase& database,
        OcrCancel ocr_cancel,
        parking_timer::ParkingSlotManager& timer_manager,
        parking_timer::EventManager& event_manager,
        parking::EvidenceCaptureWorker& evidence_worker,
        event::SystemEventReporter* system_event_reporter = nullptr,
        TransitionSink transition_sink = {});

    ~HallParkingService();

    HallParkingService(const HallParkingService&) = delete;
    HallParkingService& operator=(const HallParkingService&) = delete;

    /** @brief SENSOR:HALLxx:OCCUPIED/VACANT 메시지 한 줄을 처리한다. */
    bool handleLine(const std::string& line,
                    const std::string& transport = "mqtt-test");

private:
    bool processEventLocked(const parking::ParkingSensorEvent& event,
                            bool apply_confirmation_gate);
    bool handleOccupied(const parking::ParkingSensorEvent& event,
                        const parking::ParkingTransitionResult& transition);
    bool handleVacant(const parking::ParkingSensorEvent& event,
                      const parking::ParkingTransitionResult& transition);
    void confirmationLoop();
    void workLoop();
    [[nodiscard]] bool canEnqueueWorkLocked(
        const parking::ParkingSensorEvent& event);
    bool enqueueWorkLocked(HallParkingWorkItem item);
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
    database::EventDatabase& database_;
    OcrCancel ocr_cancel_;
    parking_timer::ParkingSlotManager& timer_manager_;
    parking_timer::EventManager& event_manager_;
    parking::EvidenceCaptureWorker& evidence_worker_;
    event::SystemEventReporter* system_event_reporter_{};
    TransitionSink transition_sink_;
    std::optional<parking::ParkingOccupancyConfirmationGate>
        confirmation_gate_;
    std::mutex mutex_;
    std::condition_variable confirmation_condition_;
    std::thread confirmation_worker_;
    std::condition_variable work_condition_;
    HallParkingWorkQueue work_queue_;
    std::thread work_worker_;
    bool stopping_{false};
};

}  // namespace sensor

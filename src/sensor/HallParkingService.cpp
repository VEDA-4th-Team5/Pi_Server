#include "sensor/HallParkingService.hpp"

#include "util/Logger.hpp"

#include <filesystem>
#include <utility>

namespace sensor {
namespace {

const app::IvaAreaConfig* findArea(const app::AppConfig& config,
                                   const std::string& slot_id) {
    for (const auto& area : config.iva_areas)
        if (area.slot_id == slot_id) return &area;
    return nullptr;
}

std::shared_ptr<camera::CameraChannel> findChannel(
    const std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    const std::string& channel_id) {
    for (const auto& channel : channels)
        if (channel && channel->channel_id == channel_id) return channel;
    return nullptr;
}

}  // namespace

HallParkingService::HallParkingService(
    std::vector<parking::ParkingSlotConfig> slot_configs,
    const app::AppConfig& app_config,
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    database::EventDatabase& database,
    OcrCancel ocr_cancel,
    parking_timer::ParkingSlotManager& timer_manager,
    parking_timer::EventManager& event_manager,
    parking::EvidenceCaptureWorker& evidence_worker,
    ::event::SystemEventReporter* system_event_reporter,
    TransitionSink transition_sink)
    : slot_configs_(std::move(slot_configs)),
      slot_index_(slot_configs_),
      adapter_(slot_index_),
      occupancy_manager_(slot_configs_),
      app_config_(app_config),
      channels_(channels),
      database_(database),
      ocr_cancel_(std::move(ocr_cancel)),
      timer_manager_(timer_manager),
      event_manager_(event_manager),
      evidence_worker_(evidence_worker),
      system_event_reporter_(system_event_reporter),
      transition_sink_(std::move(transition_sink)) {
    work_worker_ = std::thread(&HallParkingService::workLoop, this);
    if (app_config_.parking_occupancy_confirm_ms > 0) {
        confirmation_gate_.emplace(std::chrono::milliseconds(
            app_config_.parking_occupancy_confirm_ms));
        confirmation_worker_ =
            std::thread(&HallParkingService::confirmationLoop, this);
        util::logInfo(
            "parking occupancy confirm gate enabled: threshold_ms=" +
            std::to_string(app_config_.parking_occupancy_confirm_ms));
    }
}

HallParkingService::~HallParkingService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    confirmation_condition_.notify_all();
    work_condition_.notify_all();
    if (confirmation_worker_.joinable()) confirmation_worker_.join();
    if (work_worker_.joinable()) work_worker_.join();
}

bool HallParkingService::handleLine(const std::string& line,
                                    const std::string& transport) {
    std::lock_guard lock(mutex_);
    std::string error;
    auto message = parser_.parse(line, std::chrono::system_clock::now(), &error);
    if (!message) {
        util::logWarn("Hall sensor message rejected: " + error + " | " + line);
        report(::event::SystemEventCode::SensorMessageInvalid,
               ::event::SystemEventSeverity::Warning, error, transport);
        return false;
    }
    message->transport = transport;
    auto event = adapter_.adapt(*message, &error);
    if (!event) {
        util::logWarn("Hall sensor event rejected: " + error + " | " + line);
        report(::event::SystemEventCode::SensorNotMapped,
               ::event::SystemEventSeverity::Warning,
               error + "; sensor_id=" + message->sensorId, transport);
        return false;
    }
    if (!sequence_guard_.accept(*event, &error)) {
        util::logWarn("Hall sensor event rejected: " + error + " | " + line);
        report(::event::SystemEventCode::SensorSequenceRejected,
               ::event::SystemEventSeverity::Warning, error, transport,
               event->slotId);
        return false;
    }

    return processEventLocked(*event, true);
}

bool HallParkingService::processEventLocked(
    const parking::ParkingSensorEvent& event,
    const bool apply_confirmation_gate) {
    if (apply_confirmation_gate && confirmation_gate_) {
        const auto* slot = occupancy_manager_.findSlot(event.slotId);
        const bool already_occupied = slot != nullptr && slot->occupied();
        if (confirmation_gate_->evaluate(event, already_occupied) ==
            parking::ParkingOccupancyConfirmationGate::Decision::Suppress) {
            if (event.state == parking::ParkingSensorState::Vacant) {
                parking::ParkingTransitionResult recovery_transition;
                recovery_transition.slotId = event.slotId;
                recovery_transition.sessionId.clear();
                work_queue_.push_back({event, recovery_transition});
                work_condition_.notify_one();
            }
            confirmation_condition_.notify_all();
            // 센서가 1초마다 같은 상태를 보내므로 이 두 줄이 로그의 대부분을
            // 차지한다. LOG_HALL_SENSOR=false 로 따로 끌 수 있도록 태그를 붙인다.
            util::logLine("HALL_SENSOR",
                          "event awaiting confirmation: slot=" +
                          event.slotId + " state=" +
                          (event.state == parking::ParkingSensorState::Occupied
                               ? "OCCUPIED"
                               : "VACANT"));
            return true;
        }
    }

    const auto transition = occupancy_manager_.handle(event);
    if (!transition.changed()) {
        // 재시작 직후 메모리 상태는 VACANT지만 DB에는 이전 ACTIVE가 남을 수
        // 있다. VACANT는 비동기 worker에서 DB와 대조해 stale 세션을 닫는다.
        if (event.state == parking::ParkingSensorState::Vacant) {
            work_queue_.push_back({event, transition});
            work_condition_.notify_one();
        }
        util::logLine("HALL_SENSOR", "event ignored: slot=" + event.slotId +
                      " reason=" + transition.message);
        return true;
    }
    work_queue_.push_back({event, transition});
    work_condition_.notify_one();
    return true;
}

bool HallParkingService::handleOccupied(
    const parking::ParkingSensorEvent& event,
    const parking::ParkingTransitionResult& transition) {
    const auto* area = findArea(app_config_, event.slotId);
    if (!area) {
        util::logError("No ROI mapping for hall slot=" + event.slotId);
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "no ROI mapping for occupied slot", event.sourceTransport,
               event.slotId);
        return false;
    }
    auto channel = findChannel(channels_, area->channel_id);
    if (!channel) {
        util::logError("No RTSP channel for hall slot=" + event.slotId +
                       " channel=" + area->channel_id);
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "no RTSP channel for occupied slot", event.sourceTransport,
               event.slotId);
        return false;
    }
    std::int64_t session_id = -1;
    bool adopted_existing = false;
    try {
        if (const auto existing = database_.findActiveBySlot(event.slotId)) {
            session_id = existing->id;
            adopted_existing = true;
        } else {
            session_id = database_.createHallSession(
                event.slotId, event.sensorId,
                parking_timer::utcString(event.occurredAt));
        }
    } catch (const std::exception& error) {
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "entry session creation failed: " + std::string(error.what()),
               event.sourceTransport, event.slotId);
        return false;
    }
    event_manager_.publish("SLOT_OCCUPIED", event.slotId, "",
                           parking_timer::utcNow(),
                           adopted_existing ? "existing session adopted"
                                            : "evidence capture scheduled",
                           session_id);
    if (adopted_existing) {
        // 이미 시작 증거/타이머가 존재할 수 있으므로 T0를 현재 시각으로 다시
        // 잡아 촬영을 중복 예약하지 않는다. 이후 VACANT는 같은 ID를 종료한다.
        util::logLine("HALL_RECOVERY", "active session adopted slot=" +
                      event.slotId + " session=" +
                      std::to_string(session_id));
        return true;
    }
    if (transition_sink_) {
        auto database_transition = transition;
        database_transition.sessionId = std::to_string(session_id);
        transition_sink_(database_transition);
    }
    const auto started_monotonic = transition.session
        ? transition.session->startedAtMonotonic()
        : event.receivedMonotonic;
    if (!evidence_worker_.scheduleSession({
            session_id, event.slotId, channel,
            {area->roi_x, area->roi_y, area->roi_width, area->roi_height},
            started_monotonic})) {
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "evidence capture scheduling failed", event.sourceTransport,
               event.slotId);
        try {
            timer_manager_.handleExit(event.slotId);
        } catch (...) {
            util::logError("Failed to compensate unscheduled evidence session=" +
                           std::to_string(session_id));
        }
        return false;
    }
    util::logLine("HALL_OCCUPIED", "slot=" + event.slotId +
                  " session=" + std::to_string(session_id) +
                  " evidence=scheduled");
    return true;
}

bool HallParkingService::handleVacant(
    const parking::ParkingSensorEvent& event,
    const parking::ParkingTransitionResult& transition) {
    auto departed = timer_manager_.handleExit(event.slotId);
    if (!departed) return true;

    evidence_worker_.cancelSession(departed->id);
    util::logLine("EVIDENCE_CAPTURE",
                  "overstay capture canceled by VACANT session=" +
                  std::to_string(departed->id) + " slot=" + event.slotId);

    if (transition_sink_) {
        auto database_transition = transition;
        database_transition.code =
            parking::ParkingTransitionCode::SessionCompleted;
        database_transition.sessionId = std::to_string(departed->id);
        transition_sink_(database_transition);
    }

    if (ocr_cancel_) ocr_cancel_(static_cast<int>(departed->id));
    if (!departed->violation_at.has_value()) {
        if (!removeEarlyDepartureImages(departed->id)) return false;
        event_manager_.publish("EARLY_DEPARTURE_IMAGES_DELETED", event.slotId,
                               departed->car_number, parking_timer::utcNow(),
                               "temporary entry images removed", departed->id);
    }
    return true;
}

void HallParkingService::workLoop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lock(mutex_);
            work_condition_.wait(lock, [this] {
                return stopping_ || !work_queue_.empty();
            });
            if (stopping_ && work_queue_.empty()) break;
            item = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        try {
            const bool success =
                item.event.state == parking::ParkingSensorState::Occupied
                    ? handleOccupied(item.event, item.transition)
                    : handleVacant(item.event, item.transition);
            if (!success) {
                util::logError("Hall parking async work failed: slot=" +
                               item.event.slotId);
            }
        } catch (const std::exception& error) {
            util::logError("Hall parking async exception: slot=" +
                           item.event.slotId + " error=" + error.what());
            report(::event::SystemEventCode::SensorHandlerFailed,
                   ::event::SystemEventSeverity::Error, error.what(),
                   item.event.sourceTransport, item.event.slotId);
        } catch (...) {
            util::logError("Hall parking async unknown exception: slot=" +
                           item.event.slotId);
        }
    }
}

void HallParkingService::confirmationLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        const auto next = confirmation_gate_->nextDeadline();
        if (!next) {
            confirmation_condition_.wait(lock);
        } else {
            confirmation_condition_.wait_until(lock, *next);
        }
        if (stopping_) break;

        const auto monotonic_now = std::chrono::steady_clock::now();
        const auto wall_now = std::chrono::system_clock::now();
        auto due = confirmation_gate_->takeDue(monotonic_now, wall_now);
        for (const auto& event : due) {
            util::logLine("HALL_CONFIRM",
                          "occupied confirmed slot=" + event.slotId);
            if (!processEventLocked(event, false)) {
                util::logError("confirmed OCCUPIED processing failed: slot=" +
                               event.slotId);
            }
        }
    }
}

bool HallParkingService::removeEarlyDepartureImages(const std::int64_t session_id) {
    std::vector<database::ImageView> images;
    if (!database_.listSessionImages(static_cast<int>(session_id), images)) return false;

    bool removed = true;
    for (const auto& image : images) {
        for (const auto* path : {&image.original_path, &image.enhanced_path}) {
            if (path->empty()) continue;
            std::error_code error;
            const bool existed = std::filesystem::exists(*path, error);
            if (error || (existed && !std::filesystem::remove(*path, error)) || error) {
                util::logError("Early departure image removal failed: " + *path);
                removed = false;
            }
        }
    }
    if (!removed) return false;
    return database_.deleteSessionImageRecords(static_cast<int>(session_id));
}

void HallParkingService::report(const ::event::SystemEventCode code,
                                const ::event::SystemEventSeverity severity,
                                const std::string& message,
                                const std::string& transport,
                                const std::string& slot_id) noexcept {
    if (system_event_reporter_ == nullptr) return;
    system_event_reporter_->report({
        .source = ::event::SystemEventSource::HallSensor,
        .code = code,
        .severity = severity,
        .slot_id = slot_id,
        .transport = transport,
        .device = {},
        .message = message,
    });
}

}  // namespace sensor

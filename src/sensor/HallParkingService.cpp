#include "sensor/HallParkingService.hpp"

#include "util/Logger.hpp"

#include <algorithm>
#include <exception>
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

HallParkingWorkQueue::HallParkingWorkQueue(const std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

bool HallParkingWorkQueue::canAccept(
    const parking::ParkingSensorEvent& event) const {
    return items_.size() < capacity_ ||
        std::any_of(items_.begin(), items_.end(),
            [&event](const HallParkingWorkItem& item) {
                return item.event.slotId == event.slotId &&
                       item.event.state == event.state;
            });
}

HallParkingWorkQueue::PushResult HallParkingWorkQueue::push(
    HallParkingWorkItem item) {
    const auto same_slot = std::find_if(
        items_.begin(), items_.end(),
        [&item](const HallParkingWorkItem& queued) {
            return queued.event.slotId == item.event.slotId &&
                   queued.event.state == item.event.state;
        });
    if (same_slot != items_.end()) {
        *same_slot = std::move(item);
        return PushResult::Coalesced;
    }
    if (items_.size() >= capacity_) return PushResult::Full;
    items_.push_back(std::move(item));
    return PushResult::Added;
}

std::optional<HallParkingWorkItem> HallParkingWorkQueue::pop() {
    if (items_.empty()) return std::nullopt;
    HallParkingWorkItem item = std::move(items_.front());
    items_.pop_front();
    return item;
}

bool HallParkingWorkQueue::empty() const noexcept { return items_.empty(); }
std::size_t HallParkingWorkQueue::size() const noexcept { return items_.size(); }
std::size_t HallParkingWorkQueue::capacity() const noexcept { return capacity_; }

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
      transition_sink_(std::move(transition_sink)),
      work_queue_(static_cast<std::size_t>(
          std::max(1, app_config.parking_hall_work_queue_capacity))) {
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
    if (app_config_.parking_occupancy_source == "CAMERA_IVA") {
        camera_exit_worker_ =
            std::thread(&HallParkingService::cameraExitLoop, this);
        util::logInfo("camera IVA occupancy enabled: exit_confirm_ms=" +
                      std::to_string(
                          app_config_.camera_iva_exit_confirm_ms));
    }
}

HallParkingService::~HallParkingService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    confirmation_condition_.notify_all();
    camera_exit_condition_.notify_all();
    work_condition_.notify_all();
    if (confirmation_worker_.joinable()) confirmation_worker_.join();
    if (camera_exit_worker_.joinable()) camera_exit_worker_.join();
    if (work_worker_.joinable()) work_worker_.join();
}

bool HallParkingService::handleLine(const std::string& line,
                                    const std::string& transport) {
    std::lock_guard lock(mutex_);
    if (app_config_.parking_occupancy_source != "HALL") {
        util::logLine("HALL_SENSOR",
                      "input ignored because occupancy source is " +
                          app_config_.parking_occupancy_source);
        return false;
    }
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

bool HallParkingService::handleCameraOccupancy(
    const std::string& slot_id, const bool occupied) {
    std::lock_guard lock(mutex_);
    if (app_config_.parking_occupancy_source != "CAMERA_IVA") return false;

    const auto slot = std::find_if(
        slot_configs_.begin(), slot_configs_.end(),
        [&slot_id](const parking::ParkingSlotConfig& config) {
            return config.enabled && config.slotId == slot_id;
        });
    if (slot == slot_configs_.end()) {
        util::logWarn("IVA occupancy rejected: unknown/disabled slot=" +
                      slot_id);
        return false;
    }

    parking::ParkingSensorEvent event;
    event.slotId = slot->slotId;
    // 기존 상태 머신이 슬롯 설정과 일치 여부를 검증하므로 설정된 안정 ID를
    // 사용하되 transport는 camera-mqtt로 남겨 입력 출처를 구분한다.
    event.sensorId = slot->sensorId;
    event.state = occupied ? parking::ParkingSensorState::Occupied
                           : parking::ParkingSensorState::Vacant;
    event.occurredAt = std::chrono::system_clock::now();
    event.receivedMonotonic = std::chrono::steady_clock::now();
    event.sourceTransport = "camera-mqtt";

    if (occupied) {
        const std::size_t canceled = pending_camera_exits_.erase(slot_id);
        if (canceled != 0) {
            util::logLine("IVA_EXIT", "canceled by ENTER slot=" + slot_id);
            camera_exit_condition_.notify_all();
        }
        return processEventLocked(event, false);
    }

    if (pending_camera_exits_.contains(slot_id)) {
        util::logLine("IVA_EXIT", "duplicate ignored slot=" + slot_id);
        return true;
    }
    const auto delay = std::chrono::milliseconds(
        app_config_.camera_iva_exit_confirm_ms);
    pending_camera_exits_.emplace(
        slot_id, PendingCameraExit{event, event.receivedMonotonic + delay});
    util::logLine("IVA_EXIT", "pending slot=" + slot_id +
                  " confirm_ms=" + std::to_string(delay.count()));
    camera_exit_condition_.notify_all();
    return true;
}

bool HallParkingService::processEventLocked(
    const parking::ParkingSensorEvent& event,
    const bool apply_confirmation_gate,
    const std::optional<std::int64_t> expected_session_id) {
    if (apply_confirmation_gate && confirmation_gate_) {
        const auto* slot = occupancy_manager_.findSlot(event.slotId);
        const bool already_occupied = slot != nullptr && slot->occupied();
        if (confirmation_gate_->evaluate(event, already_occupied) ==
            parking::ParkingOccupancyConfirmationGate::Decision::Suppress) {
            if (event.state == parking::ParkingSensorState::Vacant) {
                parking::ParkingTransitionResult recovery_transition;
                recovery_transition.slotId = event.slotId;
                recovery_transition.sessionId.clear();
                if (!enqueueWorkLocked(
                        {event, recovery_transition, expected_session_id}))
                    return false;
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

    if (!canEnqueueWorkLocked(event)) return false;
    const auto transition = occupancy_manager_.handle(event);
    if (!transition.changed()) {
        // 재시작 직후 메모리 상태는 VACANT지만 DB에는 이전 ACTIVE가 남을 수
        // 있다. VACANT는 비동기 worker에서 DB와 대조해 stale 세션을 닫는다.
        if (event.state == parking::ParkingSensorState::Vacant) {
            if (!enqueueWorkLocked(
                    {event, transition, expected_session_id})) return false;
        }
        util::logLine("HALL_SENSOR", "event ignored: slot=" + event.slotId +
                      " reason=" + transition.message);
        return true;
    }
    return enqueueWorkLocked({event, transition, expected_session_id});
}

bool HallParkingService::canEnqueueWorkLocked(
    const parking::ParkingSensorEvent& event) {
    if (work_queue_.canAccept(event)) return true;

    const std::string message =
        "hall work queue full; newest distinct-slot event rejected; size=" +
        std::to_string(work_queue_.size()) + " capacity=" +
        std::to_string(work_queue_.capacity());
    util::logError(message + " slot=" + event.slotId);
    report(::event::SystemEventCode::HallWorkQueueOverflow,
           ::event::SystemEventSeverity::Error, message,
           event.sourceTransport, event.slotId);
    return false;
}

bool HallParkingService::enqueueWorkLocked(HallParkingWorkItem item) {
    if (!canEnqueueWorkLocked(item.event)) return false;
    const auto slot_id = item.event.slotId;
    const auto latest_state = item.event.state;
    const auto result = work_queue_.push(std::move(item));
    if (result == HallParkingWorkQueue::PushResult::Coalesced) {
        util::logLine(
            "HALL_WORK_QUEUE",
            "pending slot event coalesced slot=" + slot_id + " latest=" +
            (latest_state == parking::ParkingSensorState::Occupied
                 ? "OCCUPIED" : "VACANT"));
        work_condition_.notify_one();
        return true;
    }
    if (result == HallParkingWorkQueue::PushResult::Full) return false;
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
        // 재시작 복구 세션은 시작 시점에 evidence_worker_.restoreSession으로
        // 이미 스케줄돼 있어 아래 scheduleSession 호출이 session_id 기준
        // no-op이 된다. 반면 BestShot이 홀센서보다 먼저 세션을 만든 경우는
        // 촬영/OCR이 전혀 예약된 적이 없으므로, 여기서 return하지 않고 아래
        // 공통 경로를 그대로 태워 예약한다.
        util::logLine("HALL_RECOVERY", "active session adopted slot=" +
                      event.slotId + " session=" +
                      std::to_string(session_id));
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
            started_monotonic, area->snapshot_api_channel})) {
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
    const parking::ParkingTransitionResult& transition,
    const std::optional<std::int64_t> expected_session_id) {
    if (expected_session_id) {
        const auto active = database_.findActiveBySlot(event.slotId);
        if (!active || active->id != *expected_session_id) {
            util::logLine(
                "IVA_EXIT",
                "stale event ignored slot=" + event.slotId +
                    " expected_session=" +
                    std::to_string(*expected_session_id) +
                    " active_session=" +
                    (active ? std::to_string(active->id) : "none"));
            return true;
        }
    }
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
        HallParkingWorkItem item;
        {
            std::unique_lock lock(mutex_);
            work_condition_.wait(lock, [this] {
                return stopping_ || !work_queue_.empty();
            });
            if (stopping_ && work_queue_.empty()) break;
            auto popped = work_queue_.pop();
            if (!popped) continue;
            item = std::move(*popped);
        }
        try {
            const bool success =
                item.event.state == parking::ParkingSensorState::Occupied
                    ? handleOccupied(item.event, item.transition)
                    : handleVacant(item.event, item.transition,
                                   item.expectedSessionId);
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

void HallParkingService::cameraExitLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (pending_camera_exits_.empty()) {
            camera_exit_condition_.wait(lock);
            continue;
        }
        auto next = std::min_element(
            pending_camera_exits_.begin(), pending_camera_exits_.end(),
            [](const auto& left, const auto& right) {
                return left.second.deadline < right.second.deadline;
            });
        const auto deadline = next->second.deadline;
        if (camera_exit_condition_.wait_until(lock, deadline) !=
            std::cv_status::timeout) {
            continue;
        }
        if (stopping_) break;

        const auto now = std::chrono::steady_clock::now();
        std::vector<parking::ParkingSensorEvent> due;
        for (auto it = pending_camera_exits_.begin();
             it != pending_camera_exits_.end();) {
            if (it->second.deadline > now) {
                ++it;
                continue;
            }
            auto event = it->second.event;
            event.occurredAt = std::chrono::system_clock::now();
            event.receivedMonotonic = now;
            due.push_back(std::move(event));
            it = pending_camera_exits_.erase(it);
        }
        // DB 조회는 MQTT가 호출하는 public method와 같은 mutex를 잡지 않은
        // background 구간에서 수행한다.
        lock.unlock();
        for (const auto& event : due) {
            try {
                util::logLine("IVA_EXIT", "confirmed slot=" + event.slotId);
                const auto active = database_.findActiveBySlot(event.slotId);
                if (!active) {
                    util::logLine("IVA_EXIT",
                                  "ignored slot=" + event.slotId +
                                      " reason=no active session");
                    continue;
                }
                std::lock_guard eventLock(mutex_);
                if (stopping_) continue;
                if (!processEventLocked(event, false, active->id)) {
                    util::logError(
                        "confirmed IVA EXIT processing failed: slot=" +
                        event.slotId);
                }
            } catch (const std::exception& error) {
                util::logError("IVA EXIT confirmation failed: slot=" +
                               event.slotId + " error=" + error.what());
            } catch (...) {
                util::logError("IVA EXIT confirmation failed: slot=" +
                               event.slotId + " unknown error");
            }
        }
        lock.lock();
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
        .source = transport == "camera-mqtt"
            ? ::event::SystemEventSource::Mqtt
            : ::event::SystemEventSource::HallSensor,
        .code = code,
        .severity = severity,
        .slot_id = slot_id,
        .transport = transport,
        .device = {},
        .message = message,
    });
}

}  // namespace sensor

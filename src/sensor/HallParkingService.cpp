#include "sensor/HallParkingService.hpp"

#include "parking_timer/Types.hpp"
#include "util/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
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

std::int64_t epochMs(
    const std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string stableHash(const std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string areaKey(const event::IvaOccupancySignal& signal) {
    return signal.cameraId + "|" + signal.videoSourceToken + "|" +
           signal.ruleName;
}

const char* actionName(const event::IvaOccupancyAction action) {
    switch (action) {
    case event::IvaOccupancyAction::Enter:
        return "ENTER";
    case event::IvaOccupancyAction::Intrusion:
        return "INTRUSION";
    case event::IvaOccupancyAction::Exit:
        return "EXIT";
    case event::IvaOccupancyAction::Unsupported:
        return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
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
std::size_t HallParkingWorkQueue::capacity() const noexcept {
    return capacity_;
}

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
    TransitionSink transition_sink,
    std::function<void(parking::SlotActorCheckpoint)> actor_checkpoint)
    : slot_configs_(std::move(slot_configs)),
      slot_index_(slot_configs_),
      adapter_(slot_index_),
      app_config_(app_config),
      channels_(channels),
      database_(database),
      ocr_cancel_(std::move(ocr_cancel)),
      timer_manager_(timer_manager),
      event_manager_(event_manager),
      evidence_worker_(evidence_worker),
      system_event_reporter_(system_event_reporter),
      transition_sink_(std::move(transition_sink)),
      transition_store_(database_),
      transition_actor_(
          transition_store_,
          {.transportAdmissionCapacity = static_cast<std::size_t>(
               std::max(1, app_config.parking_hall_work_queue_capacity)),
           .durablePendingCapacity = static_cast<std::size_t>(
                std::max(1, app_config.parking_hall_work_queue_capacity)),
            .retryDelay = std::chrono::milliseconds(50),
            .checkpoint = std::move(actor_checkpoint)},
           [this](const parking::CommittedOccupancyTransition& transition) {
              return applyCommittedEffects(transition);
          }) {
    if (!transition_actor_.start()) {
        throw std::runtime_error(
            "durable slot transition actor could not be started");
    }
    util::logLine(
        "SLOT_TRANSITION_ACTOR",
        "started source=" + app_config_.parking_occupancy_source +
            " durable_capacity=" +
            std::to_string(std::max(
                1, app_config_.parking_hall_work_queue_capacity)) +
            " hall_confirm_ms=" +
            std::to_string(std::max(
                0, app_config_.parking_occupancy_confirm_ms)) +
            " camera_exit_confirm_ms=" +
            std::to_string(std::max(
                0, app_config_.camera_iva_exit_confirm_ms)));
}

HallParkingService::~HallParkingService() {
    if (!stop(std::chrono::seconds(30))) std::terminate();
}

bool HallParkingService::stop(const std::chrono::milliseconds timeout) {
    transition_actor_.closeIngress();
    return transition_actor_.stopAndDrain(timeout);
}

parking::SlotTransitionCommand HallParkingService::makeHallCommand(
    const parking::ParkingSensorEvent& event,
    const std::string& raw_line) const {
    const auto received_now = std::chrono::system_clock::now();
    const auto received_epoch_ms = epochMs(received_now);
    std::string source_identity;
    if (event.sourceProtocolVersion == SensorProtocolVersion::BootEpochV2 &&
        event.sourceBootId && event.sourceSequence) {
        source_identity = event.sensorId + ":v2:" + *event.sourceBootId +
            ':' + std::to_string(*event.sourceSequence);
    } else if (event.sourceSequence) {
        source_identity = event.sensorId + ":v1:" +
            std::to_string(*event.sourceSequence);
    } else {
        source_identity = event.sensorId + ":unsequenced:" +
            stableHash(raw_line) + ":" + std::to_string(received_epoch_ms) +
            ":" + std::to_string(unsequenced_identity_.fetch_add(1));
    }
    nlohmann::json payload{
        {"state", event.state == parking::ParkingSensorState::Occupied
                      ? "OCCUPIED" : "VACANT"},
        {"occupancy_policy", app_config_.parking_occupancy_source},
        {"source_protocol", toString(event.sourceProtocolVersion)},
        {"transport", event.sourceTransport},
        {"occurred_at_epoch_ms", epochMs(event.occurredAt)}};
    if (event.sourceBootId) payload["source_boot_id"] = *event.sourceBootId;
    parking::SlotTransitionCommand command;
    command.commandId = "hall:" + stableHash(source_identity);
    command.kind = parking::SlotCommandKind::HallObservation;
    command.slotId = event.slotId;
    command.sensorId = event.sensorId;
    command.sourceIdentity = source_identity;
    command.sourceSequence = event.sourceSequence;
    command.occurredAt = parking_timer::utcString(event.occurredAt);
    command.payloadJson = payload.dump();
    command.dueAtEpochMs = received_epoch_ms;
    if (event.state == parking::ParkingSensorState::Occupied) {
        command.dueAtEpochMs +=
            std::max(0, app_config_.parking_occupancy_confirm_ms);
    }
    return command;
}

parking::SlotTransitionCommand HallParkingService::makeCameraCommand(
    const event::IvaOccupancySignal& signal) const {
    const auto occurred_at = signal.occurredAt;
    const auto occurred_epoch_ms = epochMs(occurred_at);
    const auto received_epoch_ms =
        epochMs(std::chrono::system_clock::now());
    const auto deadline_due_epoch_ms = received_epoch_ms +
        ((signal.action == event::IvaOccupancyAction::Exit &&
          signal.authoritativeExit)
             ? std::max(0, app_config_.camera_iva_exit_confirm_ms)
             : 0);
    const std::string key = areaKey(signal);

    std::vector<std::string> configured_areas;
    const auto slot = std::find_if(
        slot_configs_.begin(), slot_configs_.end(),
        [&signal](const parking::ParkingSlotConfig& config) {
            return config.enabled && config.slotId == signal.slotId;
        });
    if (slot != slot_configs_.end()) {
        for (const auto& binding : slot->cameraBindings) {
            if (!binding.enabled) continue;
            configured_areas.push_back(
                binding.cameraId + "|" + binding.videoSourceToken + "|" +
                binding.ruleName);
        }
    }
    if (configured_areas.empty()) configured_areas.push_back(key);
    std::sort(configured_areas.begin(), configured_areas.end());
    configured_areas.erase(
        std::unique(configured_areas.begin(), configured_areas.end()),
        configured_areas.end());

    const std::string fallback_identity =
        signal.slotId + "|" + key + "|" + signal.objectId + "|" +
        actionName(signal.action) + "|" +
        std::to_string(occurred_epoch_ms) + "|" +
        std::to_string(unsequenced_identity_.fetch_add(1));
    const std::string source_identity = signal.sourceIdentity.empty()
        ? "camera-fallback:" + stableHash(fallback_identity)
        : signal.sourceIdentity;
    const std::string correlation_id =
        "correlation:" + stableHash(source_identity);
    const auto correlation_expires_at_epoch_ms = occurred_epoch_ms +
        std::max(1, app_config_.bestshot_correlation_window_ms);
    nlohmann::json payload{
        {"action", actionName(signal.action)},
        {"authoritative_exit", signal.authoritativeExit},
        {"occupancy_authority", signal.occupancyAuthority},
        {"occupancy_policy", app_config_.parking_occupancy_source},
        {"area_key", key},
        {"configured_areas", configured_areas},
        {"camera_id", signal.cameraId},
        {"channel_id", signal.channelId},
        {"video_source_token", signal.videoSourceToken},
        {"rule_name", signal.ruleName},
        {"object_id", signal.objectId},
        {"correlation_id", correlation_id},
        {"correlation_expires_at_epoch_ms",
         correlation_expires_at_epoch_ms},
        {"transport", "camera-mqtt"},
        {"occurred_at_epoch_ms", occurred_epoch_ms},
        {"timestamp_authority", signal.occurredAtFromSource
             ? "CAMERA_UTC" : "RECEIVE_FALLBACK"},
        {"deadline_due_at_epoch_ms", deadline_due_epoch_ms}};

    parking::SlotTransitionCommand command;
    command.commandId = "camera:" + stableHash(source_identity);
    command.kind = parking::SlotCommandKind::CameraObservation;
    command.slotId = signal.slotId;
    command.sensorId = slot == slot_configs_.end() ? std::string{}
                                                    : slot->sensorId;
    command.sourceIdentity = source_identity;
    command.occurredAt = parking_timer::utcString(occurred_at);
    command.payloadJson = payload.dump();
    // The observation reducer must run immediately. Only the separately
    // persisted exit deadline is delayed; otherwise a later same-slot
    // INTRUSION cannot supersede it before the confirmation interval elapses.
    command.dueAtEpochMs = received_epoch_ms;
    return command;
}

bool HallParkingService::handleLine(const std::string& line,
                                    const std::string& transport) {
    if (app_config_.parking_occupancy_source != "HALL" &&
        app_config_.parking_occupancy_source != "HYBRID_OR") {
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

    const auto submitted = transition_actor_.submit(
        makeHallCommand(*event, line));
    if (!submitted.accepted()) {
        const auto severity = submitted.code ==
                parking::SlotSubmitCode::RejectedStale
            ? ::event::SystemEventSeverity::Warning
            : ::event::SystemEventSeverity::Error;
        const auto code = submitted.code ==
                parking::SlotSubmitCode::RejectedStale
            ? ::event::SystemEventCode::SensorSequenceRejected
            : ::event::SystemEventCode::HallWorkQueueOverflow;
        report(code, severity, submitted.message, transport, event->slotId);
        util::logWarn("Hall durable admission rejected: slot=" +
                      event->slotId + " reason=" + submitted.message);
        return false;
    }
    return true;
}

bool HallParkingService::handleCameraIvaSignal(
    const event::IvaOccupancySignal& signal) {
    const bool camera_authoritative =
        app_config_.parking_occupancy_source == "CAMERA_IVA" ||
        app_config_.parking_occupancy_source == "HYBRID_OR";
    const bool hall_authoritative =
        app_config_.parking_occupancy_source == "HALL" ||
        app_config_.parking_occupancy_source == "HYBRID_OR";
    if ((!camera_authoritative && !hall_authoritative) ||
        signal.occupancyAuthority != camera_authoritative) {
        return false;
    }
    const auto slot = std::find_if(
        slot_configs_.begin(), slot_configs_.end(),
        [&signal](const parking::ParkingSlotConfig& config) {
            return config.enabled && config.slotId == signal.slotId;
        });
    if (slot == slot_configs_.end()) {
        util::logWarn("IVA occupancy rejected: unknown/disabled slot=" +
                      signal.slotId);
        return false;
    }
    if (signal.action == event::IvaOccupancyAction::Unsupported) return false;
    if (app_config_.parking_occupancy_source == "HALL" &&
        signal.action != event::IvaOccupancyAction::Intrusion) {
        return false;
    }

    const auto submitted = transition_actor_.submit(makeCameraCommand(signal));
    if (!submitted.accepted()) {
        report(::event::SystemEventCode::HallWorkQueueOverflow,
               ::event::SystemEventSeverity::Error, submitted.message,
               "camera-mqtt", signal.slotId);
        util::logWarn("IVA durable admission rejected: slot=" +
                      signal.slotId + " reason=" + submitted.message);
        return false;
    }
    return true;
}

bool HallParkingService::applyCommittedEffects(
    const parking::CommittedOccupancyTransition& committed) {
    parking::ParkingTransitionResult transition;
    transition.slotId = committed.slotId;
    transition.sessionId = std::to_string(committed.sessionId);
    transition.message = committed.message;

    if (committed.code ==
        parking::CommittedOccupancyCode::SessionStarted) {
        const auto* area = findArea(app_config_, committed.slotId);
        if (!area) {
            report(::event::SystemEventCode::SensorHandlerFailed,
                   ::event::SystemEventSeverity::Error,
                   "no ROI mapping for committed occupied slot",
                   committed.sourceTransport, committed.slotId);
            return false;
        }
        auto channel = findChannel(channels_, area->channel_id);
        if (!channel) {
            report(::event::SystemEventCode::SensorHandlerFailed,
                   ::event::SystemEventSeverity::Error,
                   "no RTSP channel for committed occupied slot",
                   committed.sourceTransport, committed.slotId);
            return false;
        }

        const auto started_system = std::chrono::system_clock::time_point{
            std::chrono::milliseconds(committed.occurredAtEpochMs)};
        const auto wall_now = std::chrono::system_clock::now();
        const auto steady_now = std::chrono::steady_clock::now();
        const auto elapsed = wall_now > started_system
            ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  wall_now - started_system)
            : std::chrono::steady_clock::duration::zero();
        const auto started_monotonic = steady_now - elapsed;

        transition.code = parking::ParkingTransitionCode::SessionStarted;
        transition.session.emplace(
            transition.sessionId, committed.slotId, committed.sensorId,
            started_system, started_monotonic);
        if (transition_sink_) transition_sink_(transition);

        if (!evidence_worker_.scheduleSession({
                committed.sessionId, committed.slotId, channel,
                {},
                started_monotonic, area->snapshot_api_channel})) {
            report(::event::SystemEventCode::SensorHandlerFailed,
                   ::event::SystemEventSeverity::Error,
                   "evidence capture scheduling failed after occupancy commit; "
                   "session retained for retry",
                   committed.sourceTransport, committed.slotId);
            return false;
        }
        return event_manager_.publish(
            "SLOT_OCCUPIED", committed.slotId, "",
            committed.occurredAt, "authoritative occupancy commit",
            committed.sessionId);
    }

    if (committed.code != parking::CommittedOccupancyCode::SessionEnded)
        return true;

    const auto departed = timer_manager_.handleCommittedExit(
        committed.sessionId, committed.slotId);
    if (!departed) {
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "committed session end could not rebuild timer projection",
               committed.sourceTransport, committed.slotId);
        return false;
    }

    evidence_worker_.cancelSession(departed->id);
    transition.code = parking::ParkingTransitionCode::SessionCompleted;
    if (transition_sink_) transition_sink_(transition);
    if (ocr_cancel_) ocr_cancel_(static_cast<int>(departed->id));
    const bool departure_published = event_manager_.publish(
        "DEPARTURE", committed.slotId, departed->car_number,
        departed->departed_at.value_or(committed.occurredAt),
        "authoritative occupancy transaction committed; timer projection "
        "lazily canceled",
        departed->id);
    bool cleanup_published = true;
    if (!departed->violation_at.has_value()) {
        if (!removeEarlyDepartureImages(departed->id)) return false;
        cleanup_published = event_manager_.publish(
            "EARLY_DEPARTURE_IMAGES_DELETED", committed.slotId,
            departed->car_number, committed.occurredAt,
            "temporary entry images removed", departed->id);
    }
    return departure_published && cleanup_published;
}

bool HallParkingService::removeEarlyDepartureImages(
    const std::int64_t session_id) {
    std::vector<database::ImageView> images;
    if (!database_.listSessionImages(static_cast<int>(session_id), images))
        return false;

    bool removed = true;
    for (const auto& image : images) {
        for (const auto* path : {&image.original_path, &image.enhanced_path}) {
            if (path->empty()) continue;
            std::error_code error;
            const bool existed = std::filesystem::exists(*path, error);
            if (error || (existed && !std::filesystem::remove(*path, error)) ||
                error) {
                util::logError("Early departure image removal failed: " +
                               *path);
                removed = false;
            }
        }
    }
    if (!removed) return false;
    return database_.deleteSessionImageRecords(static_cast<int>(session_id));
}

void HallParkingService::report(
    const ::event::SystemEventCode code,
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

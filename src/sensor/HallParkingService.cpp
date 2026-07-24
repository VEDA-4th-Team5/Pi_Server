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
    snapshot::SnapshotStorage& snapshot_storage,
    database::EventDatabase& database,
    OcrEnqueue ocr_enqueue,
    OcrCancel ocr_cancel,
    parking_timer::ParkingSlotManager& timer_manager,
    parking_timer::EventManager& event_manager,
    ::event::SystemEventReporter* system_event_reporter)
    : slot_configs_(std::move(slot_configs)),
      slot_index_(slot_configs_),
      adapter_(slot_index_),
      occupancy_manager_(slot_configs_),
      app_config_(app_config),
      channels_(channels),
      snapshot_storage_(snapshot_storage),
      database_(database),
      ocr_enqueue_(std::move(ocr_enqueue)),
      ocr_cancel_(std::move(ocr_cancel)),
      timer_manager_(timer_manager),
      event_manager_(event_manager),
      system_event_reporter_(system_event_reporter) {}

bool HallParkingService::handleLine(const std::string& line,
                                    const std::string& transport) {
    std::lock_guard lock(mutex_);
    std::string error;
    auto message = parser_.parse(line, std::chrono::system_clock::now(), &error);
    if (!message) {
        util::logWarn("Hall sensor message rejected: " + error);
        report(::event::SystemEventCode::SensorMessageInvalid,
               ::event::SystemEventSeverity::Warning, error, transport);
        return false;
    }
    message->transport = transport;
    auto event = adapter_.adapt(*message, &error);
    if (!event) {
        util::logWarn("Hall sensor event rejected: " + error);
        report(::event::SystemEventCode::SensorNotMapped,
               ::event::SystemEventSeverity::Warning,
               error + "; sensor_id=" + message->sensorId, transport);
        return false;
    }
    if (!sequence_guard_.accept(*event, &error)) {
        util::logWarn("Hall sensor event rejected: " + error);
        report(::event::SystemEventCode::SensorSequenceRejected,
               ::event::SystemEventSeverity::Warning, error, transport,
               event->slotId);
        return false;
    }

    const auto transition = occupancy_manager_.handle(*event);
    if (!transition.changed()) {
        util::logInfo("Hall sensor event ignored: slot=" + event->slotId +
                      " reason=" + transition.message);
        return true;
    }
    return event->state == parking::ParkingSensorState::Occupied
               ? handleOccupied(*event)
               : handleVacant(*event);
}

bool HallParkingService::handleOccupied(const parking::ParkingSensorEvent& event) {
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
    const snapshot::NormalizedRoi roi{
        area->roi_x, area->roi_y, area->roi_width, area->roi_height};
    const std::string snapshot_path = snapshot_storage_.saveIvaAreaSnapshot(
        channel, event.slotId, roi);
    if (snapshot_path.empty()) {
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "entry Snapshot capture failed", event.sourceTransport,
               event.slotId);
        return false;
    }

    int session_id = -1;
    if (!database_.createEntryWithSnapshot(
            event.slotId, snapshot_path, event.sensorId, &session_id)) {
        std::error_code ignored;
        std::filesystem::remove(snapshot_path, ignored);
        report(::event::SystemEventCode::SensorHandlerFailed,
               ::event::SystemEventSeverity::Error,
               "entry session or EVENT_LOG creation failed",
               event.sourceTransport, event.slotId);
        return false;
    }
    event_manager_.publish("SLOT_OCCUPIED", event.slotId, "",
                           parking_timer::utcNow(), snapshot_path, session_id);
    if (ocr_enqueue_) ocr_enqueue_(session_id, event.slotId, snapshot_path);
    util::logLine("HALL_OCCUPIED", "slot=" + event.slotId +
                  " session=" + std::to_string(session_id) +
                  " snapshot=" + snapshot_path);
    return true;
}

bool HallParkingService::handleVacant(const parking::ParkingSensorEvent& event) {
    auto departed = timer_manager_.handleExit(event.slotId);
    if (!departed) return true;

    if (ocr_cancel_) ocr_cancel_(static_cast<int>(departed->id));
    if (!departed->violation_at.has_value()) {
        if (!removeEarlyDepartureImages(departed->id)) return false;
        event_manager_.publish("EARLY_DEPARTURE_IMAGES_DELETED", event.slotId,
                               departed->car_number, parking_timer::utcNow(),
                               "temporary entry images removed", departed->id);
    }
    return true;
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

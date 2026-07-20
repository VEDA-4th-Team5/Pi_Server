#include "parking/ParkingTriggerCoordinator.hpp"

#include "util/Logger.hpp"

namespace parking {

ParkingTriggerCoordinator::ParkingTriggerCoordinator(
    int correlation_window_ms, int duplicate_suppression_ms)
    : correlation_window_(correlation_window_ms > 0 ? correlation_window_ms : 8000),
      duplicate_suppression_(duplicate_suppression_ms > 0
                                 ? duplicate_suppression_ms
                                 : 1500) {
}

bool ParkingTriggerCoordinator::recordCameraIva(const std::string& slot_id,
                                                 const std::string& channel_id) {
    if (slot_id.empty() || channel_id.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    auto last = last_iva_by_slot_.find(slot_id);
    if (last != last_iva_by_slot_.end() &&
        now - last->second < duplicate_suppression_) {
        util::logInfo("duplicate IVA trigger suppressed: slot=" + slot_id);
        return false;
    }
    last_iva_by_slot_[slot_id] = now;

    PendingTrigger& pending = pending_by_slot_[slot_id];
    pending.channel_id = channel_id;
    pending.camera_iva = true;
    pending.updated_at = now;
    util::logLine("PARKING_TRIGGER", "camera IVA pending slot=" + slot_id +
                  " channel=" + channel_id);
    return true;
}

void ParkingTriggerCoordinator::recordHallState(const std::string& slot_id,
                                                 const std::string& channel_id,
                                                 bool occupied) {
    if (slot_id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!occupied) {
        pending_by_slot_.erase(slot_id);
        return;
    }
    PendingTrigger& pending = pending_by_slot_[slot_id];
    if (!channel_id.empty()) pending.channel_id = channel_id;
    pending.hall_occupied = true;
    pending.updated_at = std::chrono::steady_clock::now();
}

std::optional<std::string> ParkingTriggerCoordinator::claimSlotForVehicle(
    const std::string& channel_id) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::string> selected;
    // 한 카메라 채널이 EV01/EV02처럼 여러 주차면을 담당할 수 있다.
    // unordered_map 순서나 가장 최근 이벤트에 기대지 않고, 먼저 발생한 유효
    // trigger를 먼저 Vehicle BestShot에 연결한다(FIFO).
    auto selected_time = std::chrono::steady_clock::time_point::max();

    for (auto it = pending_by_slot_.begin(); it != pending_by_slot_.end();) {
        if (now - it->second.updated_at > correlation_window_) {
            it = pending_by_slot_.erase(it);
            continue;
        }
        // 카메라 단독 모드는 camera_iva 하나로 충분하다. 홀센서가 연결되면 같은
        // pending 항목에 hall_occupied가 합쳐져 상위 정책을 강화할 수 있다.
        if (it->second.channel_id == channel_id &&
            (it->second.camera_iva || it->second.hall_occupied) &&
            it->second.updated_at < selected_time) {
            selected = it->first;
            selected_time = it->second.updated_at;
        }
        ++it;
    }

    if (selected) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - selected_time).count();
        util::logLine("PARKING_TRIGGER", "claimed slot=" + *selected +
                      " channel=" + channel_id + " age_ms=" +
                      std::to_string(age));
        pending_by_slot_.erase(*selected);
    }
    return selected;
}

void ParkingTriggerCoordinator::clearSlot(const std::string& slot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_by_slot_.erase(slot_id);
}

}

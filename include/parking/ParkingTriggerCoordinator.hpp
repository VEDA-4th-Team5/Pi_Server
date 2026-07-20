#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace parking {

// 카메라 IVA와 향후 홀센서 신호를 주차면 단위의 입차 트리거로 통합한다.
// 현재는 IVA만으로도 Vehicle BestShot을 주차면에 연결할 수 있다.
class ParkingTriggerCoordinator {
public:
    ParkingTriggerCoordinator(int correlation_window_ms,
                              int duplicate_suppression_ms);

    // 새 trigger면 true, suppression 시간 안의 중복이면 false를 반환한다.
    bool recordCameraIva(const std::string& slot_id,
                         const std::string& channel_id);

    // 추후 STM32 UART/MQTT 수신기가 호출할 인터페이스다. 현재 카메라 단독
    // 모드에서는 이 함수가 호출되지 않아도 IVA만으로 입차가 성립한다.
    void recordHallState(const std::string& slot_id,
                         const std::string& channel_id,
                         bool occupied);

    // 같은 채널에서 도착한 Vehicle BestShot에 연결할 최근 주차면을 한 번만 반환한다.
    std::optional<std::string> claimSlotForVehicle(const std::string& channel_id);
    void clearSlot(const std::string& slot_id);

private:
    struct PendingTrigger {
        std::string channel_id;
        bool camera_iva{false};
        bool hall_occupied{false};
        std::chrono::steady_clock::time_point updated_at;
    };

    std::chrono::milliseconds correlation_window_;
    std::chrono::milliseconds duplicate_suppression_;
    std::mutex mutex_;
    std::unordered_map<std::string, PendingTrigger> pending_by_slot_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_iva_by_slot_;
};

}

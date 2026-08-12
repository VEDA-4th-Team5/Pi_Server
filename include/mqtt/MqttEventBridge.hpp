#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/CameraEvent.hpp"
#include "event/IvaOccupancyCoordinator.hpp"
#include "mqtt/MqttEndpoint.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace notification {
class TelegramChannelNotifier;
}

namespace mqtt {

/**
 * Keeps a normal application publish within the exact Fire-ready MQTT epoch.
 * The release callback is always invoked when the permit leaves scope.
 */
class RegularEgressPermit {
public:
    RegularEgressPermit() = default;
    RegularEgressPermit(MqttConnectionEpoch epoch,
                        std::uint64_t generation,
                        std::function<bool()> validate,
                        std::function<void()> release)
        : epoch_(epoch),
          generation_(generation),
          validate_(std::move(validate)),
          release_(std::move(release)) {}
    ~RegularEgressPermit() { reset(); }

    RegularEgressPermit(const RegularEgressPermit&) = delete;
    RegularEgressPermit& operator=(const RegularEgressPermit&) = delete;
    RegularEgressPermit(RegularEgressPermit&& other) noexcept
        : epoch_(std::exchange(other.epoch_, 0)),
          generation_(std::exchange(other.generation_, 0)),
          validate_(std::exchange(other.validate_, {})),
          release_(std::exchange(other.release_, {})) {}
    RegularEgressPermit& operator=(RegularEgressPermit&& other) noexcept {
        if (this == &other) return *this;
        reset();
        epoch_ = std::exchange(other.epoch_, 0);
        generation_ = std::exchange(other.generation_, 0);
        validate_ = std::exchange(other.validate_, {});
        release_ = std::exchange(other.release_, {});
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return epoch_ != 0 && static_cast<bool>(validate_) &&
               static_cast<bool>(release_);
    }
    [[nodiscard]] MqttConnectionEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] bool isCurrent() const noexcept {
        if (!*this) return false;
        try {
            return validate_();
        } catch (...) {
            return false;
        }
    }

private:
    void reset() noexcept {
        if (!release_) return;
        auto release = std::exchange(release_, {});
        validate_ = {};
        epoch_ = 0;
        generation_ = 0;
        try {
            release();
        } catch (...) {
        }
    }

    MqttConnectionEpoch epoch_{};
    std::uint64_t generation_{};
    std::function<bool()> validate_;
    std::function<void()> release_;
};

/**
   * @brief 카메라 MQTT 이벤트를 Snapshot·OCR·DB 처리로 연결하는 어댑터다.
   *
   * Mosquitto C callback을 인스턴스 메서드로 전달하고, IVA 이벤트의
   * channel/slot/ROI를 찾은 뒤 최신 RTSP 프레임을 저장한다.
   * MQTT transport 연결과 callback quiescence 수명도 이 객체가 소유한다.
 */
class MqttEventBridge {
public:
    using SensorMessageHandler = std::function<void(const std::string&)>;
    using FireAckHandler =
        std::function<bool(const std::string& channel_id,
                           const std::string& alarm_id)>;
    using IvaOccupancyHandler =
        std::function<bool(const event::IvaOccupancySignal& signal)>;
    using TransportFactHandler =
        std::function<bool(const MqttTransportFact& fact)>;
    using RegularEgressAdmission =
        std::function<RegularEgressPermit()>;

    MqttEventBridge(
        const app::AppConfig& config,
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        database::EventDatabase& database,
        snapshot::SnapshotStorage& snapshot_storage,
        parking::ParkingTriggerCoordinator& trigger_coordinator,
        ocr::OcrWorker& ocr_worker,
        std::vector<parking::ParkingSlotConfig> parking_slot_configs,
        SensorMessageHandler sensor_message_handler = {},
        FireAckHandler fire_ack_handler = {},
        IvaOccupancyHandler iva_occupancy_handler = {},
        notification::TelegramChannelNotifier* telegram_notifier = nullptr,
        std::unique_ptr<IMqttTransport> transport = makeMosquittoTransport()
    );
    ~MqttEventBridge();
    MqttEventBridge(const MqttEventBridge&) = delete;
    MqttEventBridge& operator=(const MqttEventBridge&) = delete;

    /** Bind optional durable Fire delivery callbacks before start(). */
    bool bindFireAckHandler(FireAckHandler handler);
    bool bindTransportFactHandler(TransportFactHandler handler);
    bool bindRegularEgressAdmission(RegularEgressAdmission admission);

    /** @brief Broker 연결, topic 구독과 network loop를 시작한다. */
    bool start();

    void closeIngress() noexcept;
    [[nodiscard]] bool quiesceIngress(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    [[nodiscard]] bool quiesceTransportObservers(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    bool abortActiveEpoch(MqttConnectionEpoch expected_epoch) noexcept;

    /** @brief Mosquitto loop와 연결을 종료하고 자원을 해제한다. */
    [[nodiscard]] bool stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    [[nodiscard]] MqttEndpointState state() const noexcept;

    /** @brief 화재 알림 등 상위 계층의 일반 MQTT 메시지를 발행한다. */
    bool publish(const std::string& topic,
                 const std::string& payload,
                 int qos = 1,
                 bool retain = false);

    /** @brief Qt 관제 클라이언트용 상태·이벤트를 발행한다. */
    bool publishQtEvent(const std::string& topic,
                        const std::string& payload,
                        int qos = 1,
                        bool retain = false);

    /** @brief 카메라 촬영 요청 등 서버 application 메시지를 발행한다. */
    bool publishApplicationEvent(const std::string& topic,
                                  const std::string& payload,
                                  int qos = 1,
                                  bool retain = false);

    MqttTrackedPublishResult publishTrackedFire(
        const std::string& topic,
        const std::string& payload,
        bool retain,
        MqttPublishCorrelation correlation,
        MqttConnectionEpoch expected_epoch);

private:
    /** @brief 수신 메시지를 정규화하고 이벤트별 처리 흐름을 실행한다. */
    void onMessage(const std::string& topic, const std::string& payload);

    /** @brief 분리된 카메라 이벤트 한 건을 기존 IVA/일반 이벤트 흐름으로 처리한다. */
    void processCameraEvent(event::CameraEvent camera_event);

private:
    const app::AppConfig& config_;
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    database::EventDatabase& database_;
    snapshot::SnapshotStorage& snapshot_storage_;
    parking::ParkingTriggerCoordinator& trigger_coordinator_;
    ocr::OcrWorker& ocr_worker_;
    const std::vector<parking::ParkingSlotConfig> parking_slot_configs_;
    SensorMessageHandler sensor_message_handler_;
    FireAckHandler fire_ack_handler_;
    IvaOccupancyHandler iva_occupancy_handler_;
    TransportFactHandler transport_fact_handler_;
    RegularEgressAdmission regular_egress_admission_;

    notification::TelegramChannelNotifier* telegram_notifier_;

    MqttEndpoint endpoint_;
    mutable std::mutex lifecycle_mutex_;
};

}  // namespace mqtt

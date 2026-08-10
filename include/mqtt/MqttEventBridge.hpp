#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "event/CameraEvent.hpp"
#include "event/IvaOccupancyCoordinator.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <mosquitto.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace notification {
class TelegramChannelNotifier;
}

namespace mqtt {

/**
   * @brief 카메라 MQTT 이벤트를 Snapshot·OCR·DB 처리로 연결하는 어댑터다.
   *
   * Mosquitto C callback을 인스턴스 메서드로 전달하고, IVA 이벤트의
   * channel/slot/ROI를 찾은 뒤 최신 RTSP 프레임을 저장한다.
   * mosq_ 연결 수명도 이 객체가 소유한다.
 */
class MqttEventBridge {
public:
    using SensorMessageHandler = std::function<void(const std::string&)>;
    using FireAckHandler =
        std::function<bool(const std::string& channel_id,
                           const std::string& alarm_id)>;
    using IvaOccupancyHandler =
        std::function<bool(const event::IvaOccupancySignal& signal)>;

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
        notification::TelegramChannelNotifier* telegram_notifier = nullptr
    );

    /** @brief Broker 연결, topic 구독과 network loop를 시작한다. */
    bool start();

    /** @brief Mosquitto loop와 연결을 종료하고 자원을 해제한다. */
    void stop();

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

private:
    /** @brief Mosquitto C callback에서 객체의 메시지 처리 함수로 연결한다. */
    static void onMessageStatic(
        mosquitto* mosq,
        void* userdata,
        const mosquitto_message* message
    );

    /** @brief 수신 메시지를 정규화하고 이벤트별 처리 흐름을 실행한다. */
    void onMessage(
        mosquitto* mosq,
        const mosquitto_message* message
    );

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

    notification::TelegramChannelNotifier* telegram_notifier_;

    mosquitto* mosq_;
};

}  // namespace mqtt

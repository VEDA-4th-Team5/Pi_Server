#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <mosquitto.h>

#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace mqtt {

/**
 * @brief 카메라 MQTT 이벤트를 Snapshot·OCR·DB 처리로 연결하는 어댑터다.
 *
 * Mosquitto C callback을 인스턴스 메서드로 전달하고, IVA 이벤트의 channel/slot/ROI를
 * 찾은 뒤 최신 RTSP 프레임을 저장한다. mosq_ 연결 수명도 이 객체가 소유한다.
 */
class MqttEventBridge {
public:
    using SensorMessageHandler = std::function<void(const std::string&)>;

    MqttEventBridge(
        const app::AppConfig& config,
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        database::EventDatabase& database,
        snapshot::SnapshotStorage& snapshot_storage,
        parking::ParkingTriggerCoordinator& trigger_coordinator,
        ocr::OcrWorker& ocr_worker,
        SensorMessageHandler sensor_message_handler = {}
    );

    /** @brief broker 연결, topic 구독과 네트워크 loop 시작에 성공하면 true다. */
    bool start();
    /** @brief Mosquitto loop와 연결을 종료하고 자원을 해제한다. */
    void stop();

    /** @brief Qt용 application event를 broker에 발행한다. */
    bool publishQtEvent(const std::string& topic,
                        const std::string& payload,
                        int qos = 1,
                        bool retain = false);

private:
    /** @brief C callback의 userdata를 MqttEventBridge로 복원하는 진입점이다. */
    static void onMessageStatic(
        mosquitto* mosq,
        void* userdata,
        const mosquitto_message* message
    );

    /** @brief 메시지 한 건을 정규화하고 이벤트 종류에 맞는 처리 흐름을 실행한다. */
    void onMessage(mosquitto* mosq, const mosquitto_message* message);
    /** @brief 정리된 관제 payload를 설정된 QoS로 broker에 발행한다. */
    bool publish(const std::string& topic, const std::string& payload,
                 int qos, bool retain);

private:
    const app::AppConfig& config_;
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    database::EventDatabase& database_;
    snapshot::SnapshotStorage& snapshot_storage_;
    parking::ParkingTriggerCoordinator& trigger_coordinator_;
    ocr::OcrWorker& ocr_worker_;
    SensorMessageHandler sensor_message_handler_;

    mosquitto* mosq_;
};

}

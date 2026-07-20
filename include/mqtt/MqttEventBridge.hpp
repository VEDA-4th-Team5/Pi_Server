#pragma once

#include "app/AppConfig.hpp"
#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "ocr/OcrWorker.hpp"
#include "parking/ParkingTriggerCoordinator.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <mosquitto.h>

#include <memory>
#include <string>
#include <vector>

namespace mqtt {

class MqttEventBridge {
public:
    MqttEventBridge(
        const app::AppConfig& config,
        std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
        database::EventDatabase& database,
        snapshot::SnapshotStorage& snapshot_storage,
        parking::ParkingTriggerCoordinator& trigger_coordinator,
        ocr::OcrWorker& ocr_worker
    );

    bool start();
    void stop();

private:
    static void onMessageStatic(
        mosquitto* mosq,
        void* userdata,
        const mosquitto_message* message
    );

    void onMessage(mosquitto* mosq, const mosquitto_message* message);
    bool publish(const std::string& topic, const std::string& payload);

private:
    const app::AppConfig& config_;
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels_;
    database::EventDatabase& database_;
    snapshot::SnapshotStorage& snapshot_storage_;
    parking::ParkingTriggerCoordinator& trigger_coordinator_;
    ocr::OcrWorker& ocr_worker_;

    mosquitto* mosq_;
};

}

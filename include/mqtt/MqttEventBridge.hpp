  #pragma once

  #include "app/AppConfig.hpp"
  #include "camera/CameraChannel.hpp"
  #include "database/EventDatabase.hpp"
  #include "ocr/OcrWorker.hpp"
  #include "parking/ParkingTriggerCoordinator.hpp"
  #include "snapshot/SnapshotStorage.hpp"

  #include <mosquitto.h>

  #include <functional>
  #include <memory>
  #include <string>
  #include <vector>

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
      using SensorMessageHandler =
          std::function<void(const std::string&)>;

      MqttEventBridge(
          const app::AppConfig& config,
          std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
          database::EventDatabase& database,
          snapshot::SnapshotStorage& snapshot_storage,
          parking::ParkingTriggerCoordinator& trigger_coordinator,
          ocr::OcrWorker& ocr_worker,
          SensorMessageHandler sensor_message_handler = {}
      );

      /** @brief Broker 연결, topic 구독과 network loop를 시작한다. */
      bool start();

      /** @brief Mosquitto loop와 연결을 종료하고 자원을 해제한다. */
      void stop();

      /**
       * @brief 화재 알림 등 상위 계층의 일반 MQTT 메시지를 발행한다.
       *
       * 기본값은 QoS 1, retain false다.
       */
      bool publish(
          const std::string& topic,
          const std::string& payload,
          int qos = 1,
          bool retain = false
      );

      /** @brief Qt 관제 클라이언트용 상태·이벤트를 발행한다. */
      bool publishQtEvent(
          const std::string& topic,
          const std::string& payload,
          int qos = 1,
          bool retain = false
      );

      /** @brief 카메라 촬영 요청 등 서버 application 메시지를 발행한다. */
      bool publishApplicationEvent(
          const std::string& topic,
          const std::string& payload,
          int qos = 1,
          bool retain = false
      );

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

  }  // namespace mqtt

  이 구조에서는 모두 정상적으로 사용할 수 있습니다.

  // develop 화재 알림
  mqtt_bridge.publish(topic, payload);

  // Qt 주차 상태
  mqtt_bridge.publishQtEvent(topic, payload, 1, true);

  // 30초·60초 촬영 요청
  mqtt_bridge.publishApplicationEvent(topic, payload, 1, false);

  MqttEventBridge.cpp에서는 4개 인자를 받는 구현 하나를 유지해야 합니다.

  bool MqttEventBridge::publish(
      const std::string& topic,
      const std::string& payload,
      const int qos,
      const bool retain
  ) {
      // 기존 mosquitto_publish 구현
  }
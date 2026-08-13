  #pragma once

  #include "parking/ParkingSensorEvent.hpp"
  #include "sensor/SensorProtocolVersion.hpp"

  #include <chrono>
  #include <cstdint>
  #include <optional>
  #include <string>

  namespace sensor {

  // Transport-neutral result of parsing a UART/LoRa/test text packet.
  // It intentionally contains sensor_id rather than slot_id. Mapping belongs to
  // configuration on the Raspberry Pi.
  struct SensorProtocolMessage {
      std::string sensorId;
      parking::ParkingSensorState state{
          parking::ParkingSensorState::Vacant};
      std::chrono::system_clock::time_point occurredAt{
          std::chrono::system_clock::now()};
      std::optional<std::uint64_t> sequence;
      SensorProtocolVersion protocolVersion{SensorProtocolVersion::LegacyV1};
      std::optional<std::string> bootId;
      std::string transport{"text-test"};

      // LoRa 규격 v1.1(SENSOR:<node>:<sensor>:<state>:<seq>)에서만 채워지는 STM
      // 노드 ID다("STM1"/"STM2"). 센서 ID가 이미 노드를 가로질러 전역 고유하므로
      // 슬롯 매핑에는 쓰이지 않고, 링크 진단용으로만 보관한다.
      std::optional<std::string> node;

      // 수신 시점의 단조시계 값이다.
      // NTP 등으로 시스템 시간이 변경되어도 점유 확정 시간과 경과시간을
      // 안정적으로 계산하기 위해 occurredAt과 함께 전달한다.
      std::chrono::steady_clock::time_point receivedMonotonic{
          std::chrono::steady_clock::now()};
  };

  }  // namespace sensor

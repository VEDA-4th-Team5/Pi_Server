  #pragma once

  #include <chrono>
  #include <functional>
  #include <optional>
  #include <string>

  namespace parking {

  /**
   * @brief 촬영 요청이 생성된 이유다.
   *
   * MQTT payload의 reason 필드와 일대일로 대응한다.
   */
  enum class CaptureReason {
      HallOccupied30s,
      HallOccupied60s
  };

  [[nodiscard]] inline const char* toReasonString(
      const CaptureReason reason
  ) noexcept {
      switch (reason) {
          case CaptureReason::HallOccupied30s:
              return "HALL_OCCUPIED_30S";

          case CaptureReason::HallOccupied60s:
              return "HALL_OCCUPIED_60S";
      }

      return "HALL_OCCUPIED";
  }

  /**
   * @brief 주차면 촬영에 필요한 카메라·채널·ROI 정보다.
   *
   * AppConfig::iva_areas에서 슬롯 ID를 기준으로 변환한다.
   * ROI는 해상도에 독립적인 0.0~1.0 정규화 좌표를 사용한다.
   */
  struct CaptureTarget {
      std::string cameraId;
      std::string channelId;
      std::string areaName;

      double roiX{0.0};
      double roiY{0.0};
      double roiWidth{1.0};
      double roiHeight{1.0};
  };

  /**
   * @brief CaptureScheduler가 생성해 MQTT 발행 계층에 전달하는 촬영 요청이다.
   */
  struct CaptureRequest {
      // SQLite PARKING_SESSION.session_id를 문자열로 직렬화한 값이다.
      // Qt, HTTP 이미지 조회, 출차 예약 취소가 동일한 DB 세션을 참조한다.
      std::string sessionId;

      std::string slotId;
      std::string sensorId;
      CaptureTarget target;
      CaptureReason reason{
          CaptureReason::HallOccupied30s
      };

      // 1부터 시작하며 MQTT 발행 재시도 때 증가한다.
      int attempt{1};

      // DB, 로그 및 MQTT payload에 사용하는 실제 시각이다.
      std::chrono::system_clock::time_point sessionStartedAt;

      // NTP 벽시계 변경에 영향받지 않는 촬영 마감시각이다.
      std::chrono::steady_clock::time_point scheduledFor;

      // 향후 카메라 응답 규약에서 사용할 응답 제한시간이다.
      std::chrono::milliseconds responseTimeout{3000};
  };

  /**
   * @brief 슬롯 ID를 카메라·채널·ROI 대상으로 변환한다.
   *
   * 슬롯에 대응하는 카메라 설정이 없으면 std::nullopt를 반환한다.
   */
  using CaptureTargetResolver =
      std::function<
          std::optional<CaptureTarget>(
              const std::string& slotId
          )
      >;

  }  // namespace parking

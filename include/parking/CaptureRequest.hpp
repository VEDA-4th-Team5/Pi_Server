#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace parking {

enum class CaptureReason {
    HallOccupied30s,
    HallOccupied60s
};

[[nodiscard]] inline const char* toReasonString(
    const CaptureReason reason) noexcept {
    switch (reason) {
        case CaptureReason::HallOccupied30s:
            return "HALL_OCCUPIED_30S";
        case CaptureReason::HallOccupied60s:
            return "HALL_OCCUPIED_60S";
    }
    return "HALL_OCCUPIED";
}

struct CaptureTarget {
    std::string cameraId;
    std::string channelId;
    std::string areaName;
    double roiX{0.0};
    double roiY{0.0};
    double roiWidth{1.0};
    double roiHeight{1.0};
};

struct CaptureRequest {
    // 현재 서버의 SQLite PARKING_SESSION.session_id를 문자열로 직렬화한다.
    std::string sessionId;
    std::string slotId;
    std::string sensorId;
    CaptureTarget target;
    CaptureReason reason{CaptureReason::HallOccupied30s};
    int attempt{1};
    std::chrono::system_clock::time_point sessionStartedAt;
    std::chrono::steady_clock::time_point scheduledFor;
    std::chrono::milliseconds responseTimeout{3000};
};

using CaptureTargetResolver =
    std::function<std::optional<CaptureTarget>(const std::string& slotId)>;

}  // namespace parking

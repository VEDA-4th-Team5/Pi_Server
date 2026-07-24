#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace parking {

// Why a capture was requested. Maps 1:1 to the "reason" string the camera
// capture protocol expects (hall_ocr_integration/Camera MQTT Capture Protocol
// §4). Kept as an enum so the scheduling policy never hard-codes wire strings.
enum class CaptureReason {
    HallOccupied30s,
    HallOccupied60s
};

[[nodiscard]] inline const char* toReasonString(
    CaptureReason reason) noexcept {
    switch (reason) {
        case CaptureReason::HallOccupied30s:
            return "HALL_OCCUPIED_30S";
        case CaptureReason::HallOccupied60s:
            return "HALL_OCCUPIED_60S";
    }
    return "HALL_OCCUPIED";
}

// Slot -> camera channel -> ROI binding for one parking slot. Resolved from
// AppConfig::iva_areas by the composition root (main.cpp); the Core scheduler
// never reads AppConfig itself. ROI is normalized (0.0~1.0) like IvaAreaConfig.
struct CaptureTarget {
    std::string cameraId;
    std::string channelId;
    std::string areaName;
    double roiX{0.0};
    double roiY{0.0};
    double roiWidth{1.0};
    double roiHeight{1.0};
};

// A single scheduled capture request. Produced by CaptureScheduler and handed
// to the injected CapturePublisher. It carries everything the transport needs
// so the publisher stays a pure serializer with no lookups of its own.
struct CaptureRequest {
    std::string sessionId;
    std::string slotId;
    std::string sensorId;
    CaptureTarget target;
    CaptureReason reason{CaptureReason::HallOccupied30s};
    int attempt{1};  // 1-based: 1 = first try, then retries.
    std::chrono::system_clock::time_point sessionStartedAt;  // T0
    std::chrono::system_clock::time_point scheduledFor;      // due wall-clock
    std::chrono::milliseconds responseTimeout{3000};
};

// Resolves a slot id to its camera/channel/ROI. Returns nullopt when the slot
// has no observation binding (then no capture is scheduled for it).
using CaptureTargetResolver =
    std::function<std::optional<CaptureTarget>(const std::string& slotId)>;

}  // namespace parking

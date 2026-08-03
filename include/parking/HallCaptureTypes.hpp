#pragma once

#include "parking/CaptureRequest.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace parking {

enum class CaptureStage {
    First30s,
    Second60s
};

[[nodiscard]] inline CaptureStage toStage(const CaptureReason reason) noexcept {
    return reason == CaptureReason::HallOccupied60s ? CaptureStage::Second60s
                                                    : CaptureStage::First30s;
}

[[nodiscard]] inline const char* toEnhancementType(
    const CaptureStage stage) noexcept {
    return stage == CaptureStage::Second60s ? "HALL_60S" : "HALL_30S";
}

struct CapturedImage {
    std::int64_t sessionId{-1};
    std::string slotId;
    CaptureStage stage{CaptureStage::First30s};
    std::string originalPath;
    std::string enhancedPath;
};

struct HallOcrOutcome {
    std::int64_t sessionId{-1};
    CaptureStage stage{CaptureStage::First30s};
    bool recognized{false};
    std::string plateNumber;
    double confidence{0.0};
    std::string classification;
};

enum class ImageStoreResult {
    Inserted,
    Duplicate,
    InactiveSession,
    Failed
};

enum class CaptureImageResult {
    Stored,
    Duplicate,
    InactiveSession,
    Failed
};

using ImageLogWriter =
    std::function<ImageStoreResult(const CapturedImage& image)>;
using OcrSubmitter = std::function<void(const CapturedImage& image)>;
using OcrFailureWriter =
    std::function<void(std::int64_t sessionId, const std::string& slotId,
                       int attempts)>;
using EvTimerRegistrar =
    std::function<void(std::int64_t sessionId, const std::string& slotId,
                       const std::string& plateNumber)>;

struct HallCapturePorts {
    ImageLogWriter writeImageLog;
    OcrSubmitter submitOcr;
    OcrFailureWriter writeOcrFailure;
    EvTimerRegistrar registerEvTimer;
};

}  // namespace parking

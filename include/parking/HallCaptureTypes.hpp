#pragma once

#include "parking/CaptureRequest.hpp"

#include <functional>
#include <optional>
#include <string>

namespace parking {

// Which of the two post-entry captures an image belongs to (EVDA-135 schedules
// them at T0+30s / T0+60s). Maps 1:1 to IMAGE_LOG.enhancement_type, so the
// wire/DB strings live here and nowhere else.
enum class CaptureStage {
    First30s,
    Second60s
};

[[nodiscard]] inline CaptureStage toStage(CaptureReason reason) noexcept {
    return reason == CaptureReason::HallOccupied60s ? CaptureStage::Second60s
                                                    : CaptureStage::First30s;
}

[[nodiscard]] inline const char* toEnhancementType(
    CaptureStage stage) noexcept {
    return stage == CaptureStage::Second60s ? "HALL_60S" : "HALL_30S";
}

// One capture image that already exists on disk. Core never touches pixels or
// builds paths itself: the Service layer crops the slot ROI, writes the file,
// and hands the result over as this DTO (see the layering rule in
// references/pi-server.md -- no cv::Mat in a Core signature).
struct CapturedImage {
    std::string sessionId;     // Core (string) session id from ParkingSlotManager
    std::string slotId;
    CaptureStage stage{CaptureStage::First30s};
    std::string originalPath;  // data/snapshots/chN/EVxx/scene/..._HALL_30S.jpg
    std::string enhancedPath;  // .../enhanced/..._enhanced.png; empty is normal
};

// Result of one OCR round trip for a previously submitted CapturedImage.
// `recognized` is the policy-relevant bit: it is true only when a plausible
// plate was actually read, never merely because the HTTP call succeeded.
struct HallOcrOutcome {
    std::string sessionId;
    CaptureStage stage{CaptureStage::First30s};
    bool recognized{false};
    std::string plateNumber;
    double confidence{0.0};
    // "EV" / "PHEV" / "NON_EV" / "UNKNOWN" / "OCR_FAILED" as returned by
    // EventDatabase::applyPlateOcr.
    std::string classification;
};

// ---------------------------------------------------------------------------
// Ports. Every one is a std::function, so Core compiles and links without
// sqlite3, libcurl, mosquitto or OpenCV. main.cpp binds them to the real
// Service/DataAccess objects (same inversion FireAlarmManager and
// CaptureScheduler already use).
// ---------------------------------------------------------------------------

// Opens the PARKING_SESSION row for a freshly occupied slot and returns its
// session_id. Returns nullopt when the row could not be created (DB down, slot
// missing): the hall state machine and the capture schedule keep running, only
// the DB linkage is skipped for that session.
using SessionRowOpener =
    std::function<std::optional<int>(const std::string& slotId)>;

// Closes the PARKING_SESSION row (exit_time, duration, status=ENDED).
using SessionRowCloser =
    std::function<void(int dbSessionId, const std::string& slotId)>;

// Writes one IMAGE_LOG row under dbSessionId. Always called, including for
// evidence-only images, so every capture is retrievable from the session.
using ImageLogWriter =
    std::function<void(int dbSessionId, const CapturedImage& image)>;

// Hands an image to the OCR worker. Must not block: the result comes back
// asynchronously through HallCaptureCoordinator::onOcrOutcome().
using OcrSubmitter =
    std::function<void(int dbSessionId, const CapturedImage& image)>;

// Records ocr_status=FAILED / ev_status=UNKNOWN once the attempt budget is
// spent. Must never write NON_EV -- an unread plate is not a proven
// combustion vehicle (Capture Protocol §8).
using OcrFailureWriter =
    std::function<void(int dbSessionId, const std::string& slotId,
                       int attempts)>;

// Registers an EV/PHEV-confirmed session on the 1-hour overtime timer, reusing
// the session_id that already exists. Creating a second PARKING_SESSION here
// would both duplicate the session and trip ux_parking_session_active_slot.
using EvTimerRegistrar =
    std::function<void(int dbSessionId, const std::string& slotId,
                       const std::string& plateNumber)>;

struct HallCapturePorts {
    SessionRowOpener openSessionRow;
    SessionRowCloser closeSessionRow;
    ImageLogWriter writeImageLog;
    OcrSubmitter submitOcr;
    OcrFailureWriter writeOcrFailure;
    EvTimerRegistrar registerEvTimer;
};

}  // namespace parking

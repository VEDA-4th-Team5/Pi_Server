#pragma once

#include "parking/HallCaptureTypes.hpp"
#include "parking/HallOcrPolicy.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace parking {

// L1 Core orchestration that ties the hall-occupancy path to the database
// (EVDA-136).
//
// Before this class the two halves never met: ParkingSessionWorker /
// CaptureScheduler tracked sessions purely in memory under a string id
// ("EV01-20260722T120030Z"), while OcrWorker and EventDatabase key everything
// off PARKING_SESSION.session_id, an INTEGER. The coordinator owns that
// binding, so one physical parking event produces exactly one session_id and
// every capture, OCR result, image row and event row hangs off it.
//
// Responsibilities:
//   * SessionStarted -> open the PARKING_SESSION row, remember string -> int;
//     SessionCompleted -> close the row, settle a still-unread plate, forget.
//   * capture image arrives -> always record it as evidence, then let
//     HallOcrPolicy decide whether it also gets an OCR call.
//   * OCR answers -> fold into the policy; on success register the confirmed
//     EV/PHEV vehicle on the 1-hour timer under the SAME session_id, on final
//     failure record ocr_status=FAILED / ev_status=UNKNOWN.
//
// I/O boundary: every effect goes through a HallCapturePorts std::function, so
// this file links without sqlite3/curl/mosquitto and Core keeps knowing
// nothing about transports (same inversion as FireAlarmManager).
//
// Threading: transitions arrive on the UART/session-worker thread, capture
// images on the capture-scheduler timer thread, and OCR outcomes on the OCR
// worker thread. State is mutex-guarded, and ports are always invoked after
// the lock is released -- a port does DB or queue work and must never be able
// to re-enter this object while it holds its own mutex.
class HallCaptureCoordinator {
public:
    explicit HallCaptureCoordinator(HallCapturePorts ports,
                                    int maxOcrAttempts = 2);

    // ParkingSessionWorker::TransitionSink entry point.
    void onTransition(const ParkingTransitionResult& transition);

    // PARKING_SESSION.session_id bound to a Core session id, for callers that
    // must stamp it onto an outgoing capture request. nullopt when the session
    // is unknown or its row could not be created.
    [[nodiscard]] std::optional<int> dbSessionId(
        const std::string& sessionId) const;

    // A capture image is cropped and on disk. Records it against the session
    // and, when the policy allows, submits it for OCR.
    void onCaptureImage(const CapturedImage& image);

    // One OCR round trip finished.
    void onOcrOutcome(const HallOcrOutcome& outcome);

    [[nodiscard]] std::size_t trackedSessions() const;

private:
    struct Binding {
        int dbSessionId{-1};
        std::string slotId;
        // The 60s image parked while the 30s OCR is still in flight. The
        // policy decides *whether* to read it; the pixels live here because
        // the policy stays free of paths.
        std::optional<CapturedImage> heldImage;
    };

    mutable std::mutex mutex_;
    HallCapturePorts ports_;
    HallOcrPolicy policy_;
    std::unordered_map<std::string, Binding> bindings_;
};

}  // namespace parking

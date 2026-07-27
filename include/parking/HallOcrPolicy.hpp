#pragma once

#include "parking/HallCaptureTypes.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace parking {

// What to do with a capture image that just landed on disk.
enum class ImageAction {
    RunOcr,             // send it to Gemini
    StoreEvidenceOnly,  // keep the file + IMAGE_LOG row, but spend no quota
    HoldForPendingOcr,  // 30s OCR still in flight; decide once it answers
    Drop                // unknown session (already closed / never opened)
};

// Terminal OCR state of one parking session.
enum class OcrSessionState {
    Pending,
    Resolved,  // a plausible plate was read
    Failed     // attempt budget spent -> ocr_status=FAILED, ev_status=UNKNOWN
};

[[nodiscard]] const char* toString(ImageAction action) noexcept;
[[nodiscard]] const char* toString(OcrSessionState state) noexcept;

// L1 Core OCR retry policy for the two post-entry captures (Capture Protocol
// §8, EVDA-136).
//
//   * the 30s photo drives the first OCR;
//   * if that read succeeds, the 60s photo is stored as evidence and never
//     sent to Gemini -- this is the quota guard, not an optimisation;
//   * if it fails, the 60s photo gets the second (and last) attempt;
//   * after `maxAttempts` failures the session is Failed, which callers must
//     persist as ocr_status=FAILED / ev_status=UNKNOWN. An unread plate is
//     never NON_EV.
//
// Ordering is not assumed. The 60s image can arrive while the 30s OCR is still
// in flight (a slow Gemini call easily outlives the 30s gap); that image is
// held, then either released for the second attempt or demoted to evidence,
// depending on how the first attempt ends. Late, duplicate and out-of-order
// deliveries all collapse to StoreEvidenceOnly rather than burning an attempt.
//
// Pure and deterministic like CaptureScheduler: no clock, no I/O, no file
// paths. It stores decisions, never images -- HallCaptureCoordinator holds the
// held image itself. Every method is mutex-guarded because captures arrive on
// the scheduler timer thread while OCR results arrive on the OCR worker thread.
class HallOcrPolicy {
public:
    explicit HallOcrPolicy(int maxAttempts = 2);

    void openSession(const std::string& sessionId);

    // Result of folding one OCR answer into a session.
    struct OcrFold {
        bool tracked{false};           // false = stale/unknown result, ignore
        bool resolved{false};          // plate read; stop spending quota
        bool exhausted{false};         // budget spent; persist FAILED
        bool releaseHeldImage{false};  // held 60s image should now run OCR
        int attempts{0};
    };

    // State of a session at the moment it was closed, so the caller can
    // persist a terminal FAILED for a car that left before OCR ever resolved.
    struct CloseReport {
        bool tracked{false};
        OcrSessionState state{OcrSessionState::Pending};
        int attempts{0};
        // True only when closing is what settled the session as Failed. A
        // session that already exhausted its budget through onOcrResult()
        // reports state == Failed with this false, so the caller writes the
        // ocr_status=FAILED row exactly once.
        bool settledAtClose{false};
    };

    CloseReport closeSession(const std::string& sessionId);

    [[nodiscard]] ImageAction onImageArrived(const std::string& sessionId,
                                             CaptureStage stage);

    OcrFold onOcrResult(const std::string& sessionId, CaptureStage stage,
                        bool recognized);

    [[nodiscard]] OcrSessionState state(const std::string& sessionId) const;
    [[nodiscard]] std::size_t trackedSessions() const;

private:
    // Per-stage progress. Held applies to the 60s stage only.
    enum class StageState {
        NotArrived,
        Held,
        Running,
        Done
    };

    struct SessionState {
        OcrSessionState result{OcrSessionState::Pending};
        StageState stage30{StageState::NotArrived};
        StageState stage60{StageState::NotArrived};
        int attempts{0};
    };

    [[nodiscard]] static StageState& stageRef(SessionState& session,
                                              CaptureStage stage) noexcept;

    mutable std::mutex mutex_;
    int maxAttempts_;
    std::unordered_map<std::string, SessionState> sessions_;
};

}  // namespace parking

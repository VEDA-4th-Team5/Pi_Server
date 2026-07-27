#include "parking/HallCaptureCoordinator.hpp"

#include "util/Logger.hpp"

#include <utility>

namespace parking {

namespace {

std::string describe(const CapturedImage& image) {
    return "slot=" + image.slotId + " session=" + image.sessionId +
           " stage=" + toEnhancementType(image.stage) +
           " image=" + image.originalPath;
}

// Only a positive identification puts a car on the overtime timer. "UNKNOWN"
// (plate read but not in VEHICLE) and "OCR_FAILED" must not, and neither must
// "NON_EV" -- that one is a real answer, just not one the EV timer cares about.
bool isElectric(const std::string& classification) {
    return classification == "EV" || classification == "PHEV";
}

}  // namespace

HallCaptureCoordinator::HallCaptureCoordinator(HallCapturePorts ports,
                                               int maxOcrAttempts)
    : ports_(std::move(ports)), policy_(maxOcrAttempts) {
}

void HallCaptureCoordinator::onTransition(
    const ParkingTransitionResult& transition) {
    if (transition.code == ParkingTransitionCode::SessionStarted) {
        // Open the row before taking the lock: this is a DB write and the
        // session id it returns is exactly what we are about to store.
        std::optional<int> dbSessionId =
            ports_.openSessionRow ? ports_.openSessionRow(transition.slotId)
                                  : std::nullopt;
        if (!dbSessionId.has_value()) {
            // The hall state machine, the capture schedule and the slot status
            // all keep working; only the DB linkage is missing for this car.
            util::logLine("HALL_OCR",
                          "no PARKING_SESSION row for slot=" +
                              transition.slotId +
                              "; captures will not be linked session=" +
                              transition.sessionId);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            Binding binding;
            binding.dbSessionId = *dbSessionId;
            binding.slotId = transition.slotId;
            bindings_.emplace(transition.sessionId, std::move(binding));
        }
        policy_.openSession(transition.sessionId);

        util::logLine("HALL_OCR",
                      "session opened slot=" + transition.slotId +
                          " session=" + transition.sessionId +
                          " session_id=" + std::to_string(*dbSessionId));
        return;
    }

    if (transition.code != ParkingTransitionCode::SessionCompleted) {
        return;
    }

    Binding binding;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bindings_.find(transition.sessionId);
        if (it != bindings_.end()) {
            binding = std::move(it->second);
            bindings_.erase(it);
            found = true;
        }
    }
    const HallOcrPolicy::CloseReport report =
        policy_.closeSession(transition.sessionId);
    if (!found) {
        return;
    }

    // --- lock released; the ports below do I/O ---

    // The car left with the plate still unread. Settle it as UNKNOWN now,
    // otherwise the session ends looking like a read that never finished.
    // Only when closing is what settled it -- a budget that ran out earlier
    // already wrote its row in onOcrOutcome().
    if (report.settledAtClose && ports_.writeOcrFailure) {
        ports_.writeOcrFailure(binding.dbSessionId, binding.slotId,
                               report.attempts);
    }
    if (ports_.closeSessionRow) {
        ports_.closeSessionRow(binding.dbSessionId, binding.slotId);
    }

    util::logLine("HALL_OCR",
                  "session closed slot=" + binding.slotId +
                      " session=" + transition.sessionId +
                      " session_id=" + std::to_string(binding.dbSessionId) +
                      " ocr=" + toString(report.state) +
                      " attempts=" + std::to_string(report.attempts));
}

std::optional<int> HallCaptureCoordinator::dbSessionId(
    const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bindings_.find(sessionId);
    if (it == bindings_.end()) {
        return std::nullopt;
    }
    return it->second.dbSessionId;
}

void HallCaptureCoordinator::onCaptureImage(const CapturedImage& image) {
    int dbSessionId = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bindings_.find(image.sessionId);
        if (it == bindings_.end()) {
            util::logLine("HALL_OCR",
                          "capture image for unknown session dropped " +
                              describe(image));
            return;
        }
        dbSessionId = it->second.dbSessionId;
    }

    const ImageAction action = policy_.onImageArrived(image.sessionId,
                                                      image.stage);
    if (action == ImageAction::Drop) {
        return;
    }

    if (action == ImageAction::HoldForPendingOcr) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bindings_.find(image.sessionId);
        if (it != bindings_.end()) {
            it->second.heldImage = image;
        }
    }

    // --- lock released; the ports below do I/O ---

    // Every capture becomes an IMAGE_LOG row under this session, whether or
    // not it is read: evidence-only images are still what the control room
    // looks at, and the row must exist before OCR can update it by path.
    if (ports_.writeImageLog) {
        ports_.writeImageLog(dbSessionId, image);
    }

    if (action == ImageAction::RunOcr && ports_.submitOcr) {
        ports_.submitOcr(dbSessionId, image);
    }

    util::logLine("HALL_OCR", std::string(toString(action)) + " " +
                                  describe(image) +
                                  " session_id=" + std::to_string(dbSessionId));
}

void HallCaptureCoordinator::onOcrOutcome(const HallOcrOutcome& outcome) {
    const HallOcrPolicy::OcrFold fold =
        policy_.onOcrResult(outcome.sessionId, outcome.stage,
                            outcome.recognized);
    if (!fold.tracked) {
        return;  // session closed, or an answer for a stage we are not on
    }

    int dbSessionId = -1;
    std::string slotId;
    std::optional<CapturedImage> releasedImage;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bindings_.find(outcome.sessionId);
        if (it == bindings_.end()) {
            return;
        }
        dbSessionId = it->second.dbSessionId;
        slotId = it->second.slotId;
        if (fold.releaseHeldImage) {
            releasedImage = std::move(it->second.heldImage);
        }
        // Resolved or exhausted: the held image stays evidence only, and its
        // IMAGE_LOG row was already written when it arrived.
        it->second.heldImage.reset();
    }

    // --- lock released; the ports below do I/O ---

    if (fold.resolved) {
        if (isElectric(outcome.classification) && ports_.registerEvTimer) {
            // Reuse the existing session_id. Creating a session here would
            // both duplicate the parking event and trip the partial unique
            // index on (slot_id) for ACTIVE rows.
            ports_.registerEvTimer(dbSessionId, slotId, outcome.plateNumber);
        }
        util::logLine("HALL_OCR",
                      "plate resolved slot=" + slotId + " session_id=" +
                          std::to_string(dbSessionId) + " plate=" +
                          outcome.plateNumber + " class=" +
                          outcome.classification + " stage=" +
                          toEnhancementType(outcome.stage));
        return;
    }

    if (releasedImage.has_value() && ports_.submitOcr) {
        util::logLine("HALL_OCR",
                      "30s read failed; retrying with held 60s image " +
                          describe(*releasedImage));
        ports_.submitOcr(dbSessionId, *releasedImage);
        return;
    }

    if (fold.exhausted) {
        // ocr_status=FAILED / ev_status=UNKNOWN. Deliberately NOT NON_EV: an
        // unread plate is no evidence that the car is not electric.
        if (ports_.writeOcrFailure) {
            ports_.writeOcrFailure(dbSessionId, slotId, fold.attempts);
        }
        util::logLine("HALL_OCR",
                      "plate unresolved after " + std::to_string(fold.attempts) +
                          " attempt(s) slot=" + slotId + " session_id=" +
                          std::to_string(dbSessionId) +
                          " ocr_status=FAILED ev_status=UNKNOWN");
        return;
    }

    util::logLine("HALL_OCR",
                  "read failed at " + std::string(toEnhancementType(outcome.stage)) +
                      "; waiting for the next capture slot=" + slotId +
                      " session_id=" + std::to_string(dbSessionId));
}

std::size_t HallCaptureCoordinator::trackedSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_.size();
}

}  // namespace parking

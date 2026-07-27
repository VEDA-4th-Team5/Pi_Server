#include "parking/HallOcrPolicy.hpp"

#include <algorithm>

namespace parking {

const char* toString(ImageAction action) noexcept {
    switch (action) {
        case ImageAction::RunOcr:
            return "RUN_OCR";
        case ImageAction::StoreEvidenceOnly:
            return "EVIDENCE_ONLY";
        case ImageAction::HoldForPendingOcr:
            return "HELD";
        case ImageAction::Drop:
            return "DROP";
    }
    return "DROP";
}

const char* toString(OcrSessionState state) noexcept {
    switch (state) {
        case OcrSessionState::Pending:
            return "PENDING";
        case OcrSessionState::Resolved:
            return "RESOLVED";
        case OcrSessionState::Failed:
            return "FAILED";
    }
    return "PENDING";
}

HallOcrPolicy::HallOcrPolicy(int maxAttempts)
    : maxAttempts_(std::max(1, maxAttempts)) {
}

void HallOcrPolicy::openSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Re-opening an id would silently reset a live session's attempt budget.
    // Session ids are slot+T0 so a collision means a bug upstream, not a retry.
    sessions_.emplace(sessionId, SessionState{});
}

HallOcrPolicy::CloseReport HallOcrPolicy::closeSession(
    const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseReport report;

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return report;
    }

    report.tracked = true;
    report.attempts = it->second.attempts;
    report.state = it->second.result;
    // The car left with the plate still unread -- e.g. the 60s capture never
    // came back, so onOcrResult never got to spend the budget. Report that as
    // Failed (not Pending) so the caller writes the terminal UNKNOWN row
    // instead of leaving a session that looks like it is still being read.
    // A session that already reached Failed on its own is not flagged here:
    // its row was written when the budget ran out.
    if (report.state == OcrSessionState::Pending && report.attempts > 0) {
        report.state = OcrSessionState::Failed;
        report.settledAtClose = true;
    }
    sessions_.erase(it);
    return report;
}

ImageAction HallOcrPolicy::onImageArrived(const std::string& sessionId,
                                          CaptureStage stage) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return ImageAction::Drop;
    }

    SessionState& session = it->second;
    // Already decided (plate read, or budget spent): the file is still worth
    // keeping as evidence, but it buys us nothing to read it again.
    if (session.result != OcrSessionState::Pending) {
        return ImageAction::StoreEvidenceOnly;
    }

    StageState& current = stageRef(session, stage);
    if (current != StageState::NotArrived) {
        return ImageAction::StoreEvidenceOnly;  // duplicate/late redelivery
    }

    // The 30s photo is the primary read. If its OCR has not answered yet we
    // cannot know whether the 60s photo is a retry or a waste of quota, so
    // park it until onOcrResult() decides.
    if (stage == CaptureStage::Second60s &&
        session.stage30 == StageState::Running) {
        current = StageState::Held;
        return ImageAction::HoldForPendingOcr;
    }

    current = StageState::Running;
    return ImageAction::RunOcr;
}

HallOcrPolicy::OcrFold HallOcrPolicy::onOcrResult(const std::string& sessionId,
                                                  CaptureStage stage,
                                                  bool recognized) {
    std::lock_guard<std::mutex> lock(mutex_);
    OcrFold fold;

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return fold;  // session closed while the call was in flight
    }

    SessionState& session = it->second;
    StageState& current = stageRef(session, stage);
    if (current != StageState::Running) {
        return fold;  // stale or duplicate answer for a stage we are not on
    }

    fold.tracked = true;
    current = StageState::Done;

    if (recognized) {
        session.result = OcrSessionState::Resolved;
        // AC: a successful 30s read suppresses the 60s call entirely.
        if (session.stage60 == StageState::Held) {
            session.stage60 = StageState::Done;
        }
        fold.resolved = true;
        fold.attempts = session.attempts;
        return fold;
    }

    session.attempts += 1;
    if (session.attempts >= maxAttempts_) {
        session.result = OcrSessionState::Failed;
        fold.exhausted = true;
        if (session.stage60 == StageState::Held) {
            session.stage60 = StageState::Done;
        }
    } else if (session.stage60 == StageState::Held) {
        session.stage60 = StageState::Running;
        fold.releaseHeldImage = true;
    }

    fold.attempts = session.attempts;
    return fold;
}

OcrSessionState HallOcrPolicy::state(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? OcrSessionState::Pending : it->second.result;
}

std::size_t HallOcrPolicy::trackedSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

HallOcrPolicy::StageState& HallOcrPolicy::stageRef(SessionState& session,
                                                    CaptureStage stage) noexcept {
    return stage == CaptureStage::Second60s ? session.stage60 : session.stage30;
}

}  // namespace parking

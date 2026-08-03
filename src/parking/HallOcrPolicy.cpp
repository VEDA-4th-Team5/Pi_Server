#include "parking/HallOcrPolicy.hpp"

#include <algorithm>

namespace parking {

const char* toString(const ImageAction action) noexcept {
    switch (action) {
        case ImageAction::RunOcr: return "RUN_OCR";
        case ImageAction::StoreEvidenceOnly: return "EVIDENCE_ONLY";
        case ImageAction::HoldForPendingOcr: return "HELD";
        case ImageAction::Drop: return "DROP";
    }
    return "DROP";
}

const char* toString(const OcrSessionState state) noexcept {
    switch (state) {
        case OcrSessionState::Pending: return "PENDING";
        case OcrSessionState::Resolved: return "RESOLVED";
        case OcrSessionState::Failed: return "FAILED";
    }
    return "PENDING";
}

HallOcrPolicy::HallOcrPolicy(const int maxAttempts)
    : maxAttempts_(std::max(1, maxAttempts)) {}

void HallOcrPolicy::openSession(const std::string& sessionId) {
    std::lock_guard lock(mutex_);
    sessions_.emplace(sessionId, SessionState{});
}

HallOcrPolicy::CloseReport HallOcrPolicy::closeSession(
    const std::string& sessionId) {
    std::lock_guard lock(mutex_);
    CloseReport report;
    const auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) return report;

    report.tracked = true;
    report.state = found->second.result;
    report.attempts = found->second.attempts;
    if (report.state == OcrSessionState::Pending && report.attempts > 0) {
        report.state = OcrSessionState::Failed;
        report.settledAtClose = true;
    }
    sessions_.erase(found);
    return report;
}

ImageAction HallOcrPolicy::onImageArrived(const std::string& sessionId,
                                          const CaptureStage stage) {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) return ImageAction::Drop;

    SessionState& session = found->second;
    if (session.result != OcrSessionState::Pending)
        return ImageAction::StoreEvidenceOnly;

    StageState& current = stageRef(session, stage);
    if (current != StageState::NotArrived)
        return ImageAction::StoreEvidenceOnly;

    if (stage == CaptureStage::Second60s &&
        session.stage30 == StageState::Running) {
        current = StageState::Held;
        return ImageAction::HoldForPendingOcr;
    }
    current = StageState::Running;
    return ImageAction::RunOcr;
}

HallOcrPolicy::OcrFold HallOcrPolicy::onOcrResult(
    const std::string& sessionId, const CaptureStage stage,
    const bool recognized) {
    std::lock_guard lock(mutex_);
    OcrFold fold;
    const auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) return fold;

    SessionState& session = found->second;
    StageState& current = stageRef(session, stage);
    if (current != StageState::Running) return fold;

    fold.tracked = true;
    current = StageState::Done;
    if (recognized) {
        session.result = OcrSessionState::Resolved;
        if (session.stage60 == StageState::Held)
            session.stage60 = StageState::Done;
        fold.resolved = true;
        fold.attempts = session.attempts;
        return fold;
    }

    ++session.attempts;
    if (session.attempts >= maxAttempts_) {
        session.result = OcrSessionState::Failed;
        fold.exhausted = true;
        if (session.stage60 == StageState::Held)
            session.stage60 = StageState::Done;
    } else if (session.stage60 == StageState::Held) {
        session.stage60 = StageState::Running;
        fold.releaseHeldImage = true;
    }
    fold.attempts = session.attempts;
    return fold;
}

OcrSessionState HallOcrPolicy::state(const std::string& sessionId) const {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(sessionId);
    return found == sessions_.end() ? OcrSessionState::Pending
                                    : found->second.result;
}

std::size_t HallOcrPolicy::trackedSessions() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

HallOcrPolicy::StageState& HallOcrPolicy::stageRef(
    SessionState& session, const CaptureStage stage) noexcept {
    return stage == CaptureStage::Second60s ? session.stage60
                                            : session.stage30;
}

}  // namespace parking

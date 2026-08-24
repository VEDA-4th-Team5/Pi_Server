#include "parking/HallCaptureCoordinator.hpp"

#include "util/Logger.hpp"

#include <charconv>
#include <utility>

namespace parking {
namespace {

std::string keyFor(const std::int64_t sessionId) {
    return std::to_string(sessionId);
}

std::string describe(const CapturedImage& image) {
    return "slot=" + image.slotId + " session=" +
           std::to_string(image.sessionId) + " stage=" +
           toEnhancementType(image.stage) + " image=" + image.originalPath;
}

}  // namespace

HallCaptureCoordinator::HallCaptureCoordinator(HallCapturePorts ports,
                                               const int maxOcrAttempts)
    : ports_(std::move(ports)), policy_(maxOcrAttempts) {}

std::optional<std::int64_t> HallCaptureCoordinator::parseSessionId(
    const std::string& value) noexcept {
    std::int64_t parsed{-1};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < 0) {
        return std::nullopt;
    }
    return parsed;
}

void HallCaptureCoordinator::onTransition(
    const ParkingTransitionResult& transition) {
    if (transition.code == ParkingTransitionCode::SessionStarted) {
        const auto sessionId = parseSessionId(transition.sessionId);
        if (!sessionId) {
            util::logError("Hall OCR received a non-SQLite session id: " +
                           transition.sessionId);
            return;
        }
        const std::string key = keyFor(*sessionId);
        policy_.openSession(key);
        {
            std::lock_guard lock(mutex_);
            const auto [it, inserted] = bindings_.emplace(
                key, Binding{*sessionId, transition.slotId, std::nullopt});
            if (!inserted) {
                util::logLine("HALL_OCR",
                              "duplicate session bind ignored session=" + key);
                return;
            }
        }
        util::logLine("HALL_OCR", "session bound slot=" + transition.slotId +
                                      " session_id=" + key);
        return;
    }

    if (transition.code != ParkingTransitionCode::SessionCompleted) return;
    const auto sessionId = parseSessionId(transition.sessionId);
    if (!sessionId) return;
    const std::string key = keyFor(*sessionId);

    Binding binding;
    HallOcrPolicy::CloseReport report;
    bool found = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = bindings_.find(key);
        if (it != bindings_.end()) {
            report = policy_.closeSession(key);
            binding = std::move(it->second);
            bindings_.erase(it);
            found = true;
        }
    }
    if (!found) return;

    if (report.settledAtClose && ports_.writeOcrFailure) {
        ports_.writeOcrFailure(binding.sessionId, binding.slotId,
                               report.attempts);
    }
    util::logLine("HALL_OCR", "session released slot=" + binding.slotId +
                                  " session_id=" + key + " ocr=" +
                                  toString(report.state) + " attempts=" +
                                  std::to_string(report.attempts));
}

CaptureImageResult HallCaptureCoordinator::onCaptureImage(
    const CapturedImage& image) {
    const std::string key = keyFor(image.sessionId);
    std::unique_lock lock(mutex_);
    if (!bindings_.contains(key)) {
        util::logLine("HALL_OCR", "late/unknown capture dropped " +
                                          describe(image));
        return CaptureImageResult::InactiveSession;
    }
    if (!ports_.writeImageLog) return CaptureImageResult::Failed;
    const ImageStoreResult stored = ports_.writeImageLog(image);
    if (stored == ImageStoreResult::Duplicate)
        return CaptureImageResult::Duplicate;
    if (stored == ImageStoreResult::InactiveSession)
        return CaptureImageResult::InactiveSession;
    if (stored != ImageStoreResult::Inserted)
        return CaptureImageResult::Failed;

    const ImageAction action = policy_.onImageArrived(key, image.stage);
    if (action == ImageAction::Drop)
        return CaptureImageResult::InactiveSession;
    if (action == ImageAction::HoldForPendingOcr) {
        const auto it = bindings_.find(key);
        if (it != bindings_.end()) it->second.heldImage = image;
    }
    lock.unlock();
    if (action == ImageAction::RunOcr && ports_.submitOcr)
        ports_.submitOcr(image);

    util::logLine("HALL_OCR", std::string(toString(action)) + " " +
                                  describe(image));
    return CaptureImageResult::Stored;
}

void HallCaptureCoordinator::onOcrOutcome(const HallOcrOutcome& outcome) {
    const std::string key = keyFor(outcome.sessionId);
    HallOcrPolicy::OcrFold fold;
    std::string slotId;
    std::optional<CapturedImage> releasedImage;
    {
        std::lock_guard lock(mutex_);
        const auto it = bindings_.find(key);
        if (it == bindings_.end()) return;
        fold = policy_.onOcrResult(key, outcome.stage, outcome.recognized);
        if (!fold.tracked) return;
        slotId = it->second.slotId;
        if (fold.releaseHeldImage)
            releasedImage = std::move(it->second.heldImage);
        it->second.heldImage.reset();
    }

    if (fold.resolved) {
        // EV는 장기점유 타이머에, NON_EV는 즉시 위반 경보에 연결해야 하므로
        // 분류 결과와 관계없이 ParkingSlotManager의 단일 정책 진입점을 호출한다.
        if (ports_.handleRecognizedSession) {
            ports_.handleRecognizedSession(outcome.sessionId, slotId,
                                           outcome.plateNumber);
        }
        util::logLine("HALL_OCR", "plate resolved slot=" + slotId +
                                      " session_id=" + key + " plate=" +
                                      outcome.plateNumber + " class=" +
                                      outcome.classification);
        return;
    }
    if (releasedImage && ports_.submitOcr) {
        ports_.submitOcr(*releasedImage);
        return;
    }
    if (fold.exhausted) {
        if (ports_.writeOcrFailure)
            ports_.writeOcrFailure(outcome.sessionId, slotId, fold.attempts);
        util::logLine("HALL_OCR", "plate unresolved slot=" + slotId +
                                      " session_id=" + key + " attempts=" +
                                      std::to_string(fold.attempts) +
                                      " ocr_status=FAILED ev_status=UNKNOWN");
    }
}

std::size_t HallCaptureCoordinator::trackedSessions() const {
    std::lock_guard lock(mutex_);
    return bindings_.size();
}

}  // namespace parking

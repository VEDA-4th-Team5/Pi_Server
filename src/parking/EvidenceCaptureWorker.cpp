#include "parking/EvidenceCaptureWorker.hpp"

#include "parking_timer/Types.hpp"
#include "util/Logger.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace parking {

const char* toString(const EvidenceReason reason) noexcept {
    return reason == EvidenceReason::OccupancyStart
        ? "OCCUPANCY_START_EVIDENCE"
        : "OVERSTAY_EVIDENCE";
}

bool EvidenceCaptureWorker::Later::operator()(const Job& left,
                                               const Job& right) const noexcept {
    if (left.deadline != right.deadline) return left.deadline > right.deadline;
    return left.sequence > right.sequence;
}

EvidenceCaptureWorker::EvidenceCaptureWorker(
    snapshot::SnapshotStorage& storage,
    database::EventDatabase& database,
    Config config,
    Completion completion,
    Capture capture,
    RoiResolver roi_resolver)
    : storage_(storage),
      database_(database),
      config_(std::move(config)),
      completion_(std::move(completion)),
      capture_(std::move(capture)),
      roi_resolver_(std::move(roi_resolver)) {
    if (config_.overstayDelay <= std::chrono::seconds::zero() ||
        config_.maxPendingJobs < 2) {
        throw std::invalid_argument("invalid evidence capture worker config");
    }
}

EvidenceCaptureWorker::~EvidenceCaptureWorker() { stop(); }

bool EvidenceCaptureWorker::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    stopping_ = false;
    try {
        worker_ = std::thread(&EvidenceCaptureWorker::run, this);
        running_ = true;
        return true;
    } catch (const std::exception& error) {
        util::logError("Evidence worker start failed: " +
                       std::string(error.what()));
        return false;
    }
}

void EvidenceCaptureWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    running_ = false;
    while (!jobs_.empty()) jobs_.pop();
    scheduledSessions_.clear();
    canceledSessions_.clear();
    inFlightSession_.reset();
    inFlightReason_.reset();
}

bool EvidenceCaptureWorker::scheduleSession(EvidenceCaptureRequest request) {
    return scheduleSessionImpl(std::move(request), true, true, false);
}

bool EvidenceCaptureWorker::restoreSession(EvidenceCaptureRequest request) {
    try {
        const bool include_start = !database_.findEvidenceImagePath(
            request.sessionId, "OCCUPANCY_START_EVIDENCE").has_value();
        const bool include_overstay = !database_.findEvidenceImagePath(
            request.sessionId, "OVERSTAY_EVIDENCE").has_value();
        return scheduleSessionImpl(std::move(request), include_start,
                                   include_overstay, true);
    } catch (const std::exception& error) {
        util::logError("Evidence restore lookup failed: session=" +
                       std::to_string(request.sessionId) + " error=" +
                       error.what());
        return false;
    }
}

bool EvidenceCaptureWorker::scheduleSessionImpl(
    EvidenceCaptureRequest request,
    const bool include_start,
    const bool include_overstay,
    const bool restored) {
    if (request.sessionId < 0 || request.slotId.empty() || !request.channel)
        return false;
    const std::size_t requested_jobs =
        static_cast<std::size_t>(include_start) +
        static_cast<std::size_t>(include_overstay);
    if (requested_jobs == 0) {
        util::logLine("EVIDENCE_CAPTURE",
            "restore skipped; evidence already complete session=" +
            std::to_string(request.sessionId) + " slot=" + request.slotId);
        return true;
    }
    std::lock_guard lock(mutex_);
    if (!running_ || stopping_) return false;
    if (!scheduledSessions_.insert(request.sessionId).second) {
        util::logLine("EVIDENCE_CAPTURE",
            "duplicate schedule ignored session=" +
            std::to_string(request.sessionId) + " slot=" + request.slotId);
        return true;
    }
    if (jobs_.size() + requested_jobs > config_.maxPendingJobs) {
        scheduledSessions_.erase(request.sessionId);
        util::logError("Evidence queue full: session=" +
                       std::to_string(request.sessionId) + " slot=" +
                       request.slotId);
        return false;
    }
    canceledSessions_.erase(request.sessionId);
    const auto now = Clock::now();
    const auto session_id = request.sessionId;
    const auto slot_id = request.slotId;
    const auto channel_id = request.channel->channel_id;
    if (include_start) {
        jobs_.push(Job{now, nextSequence_++, 0, request,
                       EvidenceReason::OccupancyStart, !include_overstay});
        util::logLine("EVIDENCE_CAPTURE",
            std::string(restored ? "restored start capture" :
                                   "start capture scheduled") +
            " session=" + std::to_string(session_id) + " slot=" + slot_id +
            " channel=" + channel_id +
            " reason=OCCUPANCY_START_EVIDENCE");
    }
    if (include_overstay) {
        jobs_.push(Job{request.startedAtMonotonic + config_.overstayDelay,
                       nextSequence_++, overstayGeneration_, std::move(request),
                       EvidenceReason::Overstay, true});
        util::logLine("EVIDENCE_CAPTURE",
            std::string(restored ? "restored overstay capture" :
                                   "overstay capture scheduled") +
            " session=" + std::to_string(session_id) + " slot=" + slot_id +
            " channel=" + channel_id +
            " reason=OVERSTAY_EVIDENCE delay_ms=" +
            std::to_string(config_.overstayDelay.count()));
    }
    condition_.notify_all();
    return true;
}

void EvidenceCaptureWorker::cancelSession(const std::int64_t session_id) {
    if (session_id < 0) return;
    std::size_t removed{};
    {
        std::lock_guard lock(mutex_);
        canceledSessions_.insert(session_id);
        std::vector<Job> retained;
        retained.reserve(jobs_.size());
        while (!jobs_.empty()) {
            Job job = jobs_.top();
            jobs_.pop();
            if (job.request.sessionId == session_id) {
                ++removed;
            } else {
                retained.push_back(std::move(job));
            }
        }
        jobs_ = decltype(jobs_){Later{}, std::move(retained)};
        scheduledSessions_.erase(session_id);
        if (!inFlightSession_ || *inFlightSession_ != session_id)
            canceledSessions_.erase(session_id);
    }
    util::logLine("EVIDENCE_CAPTURE",
        "pending jobs removed by cancellation session=" +
        std::to_string(session_id) + " removed=" +
        std::to_string(removed));
    condition_.notify_all();
}

bool EvidenceCaptureWorker::expediteOverstay(
    const std::int64_t session_id) {
    if (session_id < 0) return false;
    std::lock_guard lock(mutex_);
    bool found = inFlightSession_ && *inFlightSession_ == session_id &&
                 inFlightReason_ && *inFlightReason_ == EvidenceReason::Overstay;
    std::vector<Job> rebuilt;
    rebuilt.reserve(jobs_.size());
    const auto now = Clock::now();
    while (!jobs_.empty()) {
        Job job = jobs_.top();
        jobs_.pop();
        if (job.request.sessionId == session_id &&
            job.reason == EvidenceReason::Overstay) {
            job.deadline = now;
            found = true;
        }
        rebuilt.push_back(std::move(job));
    }
    jobs_ = decltype(jobs_){Later{}, std::move(rebuilt)};
    if (found) condition_.notify_all();
    return found;
}

std::size_t EvidenceCaptureWorker::updateOverstayDelay(
    const std::chrono::milliseconds delay) {
    if (delay <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("overstay evidence delay must be positive");
    }
    std::lock_guard lock(mutex_);
    config_.overstayDelay = delay;
    ++overstayGeneration_;
    std::size_t updated{};
    std::vector<Job> rebuilt;
    rebuilt.reserve(jobs_.size());
    while (!jobs_.empty()) {
        Job job = jobs_.top();
        jobs_.pop();
        if (job.reason == EvidenceReason::Overstay) {
            job.deadline = job.request.startedAtMonotonic + delay;
            job.generation = overstayGeneration_;
            ++updated;
        }
        rebuilt.push_back(std::move(job));
    }
    jobs_ = decltype(jobs_){Later{}, std::move(rebuilt)};
    condition_.notify_all();
    util::logLine("EVIDENCE_CAPTURE",
        "active overstay captures rescheduled count=" +
        std::to_string(updated) + " delay_ms=" +
        std::to_string(delay.count()));
    return updated;
}

std::size_t EvidenceCaptureWorker::pendingCount() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

bool EvidenceCaptureWorker::canceled(const std::int64_t session_id) const {
    std::lock_guard lock(mutex_);
    return stopping_ || canceledSessions_.contains(session_id);
}

void EvidenceCaptureWorker::run() noexcept {
    try {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (jobs_.empty()) {
                condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                continue;
            }
            const auto deadline = jobs_.top().deadline;
            condition_.wait_until(lock, deadline, [this, deadline] {
                return stopping_ || jobs_.empty() ||
                       jobs_.top().deadline < deadline;
            });
            if (stopping_) break;
            if (jobs_.empty() || jobs_.top().deadline > Clock::now()) continue;
            Job job = jobs_.top();
            jobs_.pop();
            if (job.reason == EvidenceReason::Overstay &&
                job.generation != overstayGeneration_) {
                continue;
            }
            if (canceledSessions_.contains(job.request.sessionId)) {
                util::logLine("EVIDENCE_CAPTURE",
                    "capture canceled session=" +
                    std::to_string(job.request.sessionId) + " slot=" +
                    job.request.slotId + " channel=" +
                    job.request.channel->channel_id + " reason=" +
                    toString(job.reason));
                if (job.terminal) {
                    scheduledSessions_.erase(job.request.sessionId);
                    canceledSessions_.erase(job.request.sessionId);
                }
                continue;
            }
            const auto completed_session_id = job.request.sessionId;
            const bool terminal = job.terminal;
            inFlightSession_ = completed_session_id;
            inFlightReason_ = job.reason;
            lock.unlock();
            process(std::move(job));
            lock.lock();
            inFlightSession_.reset();
            inFlightReason_.reset();
            canceledSessions_.erase(completed_session_id);
            if (terminal) {
                scheduledSessions_.erase(completed_session_id);
            }
        }
    } catch (const std::exception& error) {
        util::logError("Evidence worker loop failed: " +
                       std::string(error.what()));
    } catch (...) {
        util::logError("Evidence worker loop failed: unknown error");
    }
}

void EvidenceCaptureWorker::process(Job job) noexcept {
    const auto session_id = job.request.sessionId;
    const std::string reason = toString(job.reason);
    const std::string channel_id = job.request.channel
        ? job.request.channel->channel_id : std::string{};
    EvidenceCaptureResult result{session_id, job.request.slotId, channel_id,
                                 job.reason, {}, {}, job.request.roi,
                                 job.request.roiRevision, false, false, {}};
    if (canceled(session_id)) return;

    try {
        if (roi_resolver_) {
            const auto applied = roi_resolver_(job.request.slotId);
            if (!applied) {
                result.message = "runtime ROI is not configured";
                emit(std::move(result));
                return;
            }
            job.request.roi = applied->value;
            job.request.roiRevision = applied->revision;
            result.roi = applied->value;
            result.roiRevision = applied->revision;
        }
        snapshot::StoredImagePair paths;
        if (capture_) {
            paths = capture_(job.request, job.reason);
        } else {
            paths.originalPath = storage_.saveEvidenceSnapshot(
                job.request.channel, session_id, job.request.slotId, reason,
                job.request.roi);
        }
        result.imagePath = paths.originalPath;
        result.enhancedImagePath = paths.enhancedPath;
        std::error_code file_error;
        const bool original_valid = !paths.originalPath.empty() &&
            std::filesystem::is_regular_file(paths.originalPath, file_error) &&
            !file_error;
        file_error.clear();
        const bool enhanced_valid = paths.enhancedPath.empty() ||
            (std::filesystem::is_regular_file(paths.enhancedPath, file_error) &&
             !file_error);
        if (!original_valid || !enhanced_valid) {
            for (const auto* path : {&paths.originalPath, &paths.enhancedPath}) {
                if (path->empty()) continue;
                std::error_code ignored;
                std::filesystem::remove(*path, ignored);
            }
            result.imagePath.clear();
            result.enhancedImagePath.clear();
            result.message = "camera API/FrameBuffer image file save failed";
            util::logError("Evidence capture failed: session=" +
                std::to_string(session_id) + " slot=" + job.request.slotId +
                " channel=" + channel_id + " reason=" + reason +
                " error=" + result.message);
            emit(std::move(result));
            return;
        }
        const auto inserted = database_.insertEvidenceImage(
            session_id, paths.originalPath, reason, parking_timer::utcNow(),
            paths.enhancedPath, job.request.roi,
            job.request.roiRevision);
        if (inserted != database::EvidenceInsertResult::Inserted) {
            for (const auto* path : {&paths.originalPath, &paths.enhancedPath}) {
                if (path->empty()) continue;
                std::error_code ignored;
                std::filesystem::remove(*path, ignored);
            }
            result.imagePath.clear();
            result.enhancedImagePath.clear();
            result.duplicate =
                inserted == database::EvidenceInsertResult::Duplicate;
            result.message = result.duplicate
                ? "duplicate evidence ignored"
                : "session is no longer active";
            util::logLine("EVIDENCE_CAPTURE",
                result.message + " session=" + std::to_string(session_id) +
                " slot=" + job.request.slotId + " channel=" + channel_id +
                " reason=" + reason);
            emit(std::move(result));
            return;
        }
        result.stored = true;
        result.message = "evidence stored";
        util::logLine("EVIDENCE_CAPTURE",
            "capture success session=" + std::to_string(session_id) +
            " slot=" + job.request.slotId + " channel=" + channel_id +
            " reason=" + reason + " path=" + paths.originalPath +
            " enhanced=" + paths.enhancedPath);
        emit(std::move(result));
    } catch (const std::exception& error) {
        for (const auto* path : {&result.imagePath,
                                 &result.enhancedImagePath}) {
            if (path->empty()) continue;
            std::error_code ignored;
            std::filesystem::remove(*path, ignored);
        }
        result.imagePath.clear();
        result.enhancedImagePath.clear();
        result.message = error.what();
        util::logError("Evidence DB/save failed: session=" +
            std::to_string(session_id) + " slot=" + job.request.slotId +
            " channel=" + channel_id + " reason=" + reason +
            " error=" + result.message);
        emit(std::move(result));
    } catch (...) {
        result.message = "unknown evidence failure";
        emit(std::move(result));
    }
}

void EvidenceCaptureWorker::emit(EvidenceCaptureResult result) noexcept {
    if (!completion_) return;
    try {
        completion_(result);
    } catch (const std::exception& error) {
        util::logError("Evidence completion callback failed: " +
                       std::string(error.what()));
    } catch (...) {
        util::logError("Evidence completion callback failed: unknown error");
    }
}

}  // namespace parking

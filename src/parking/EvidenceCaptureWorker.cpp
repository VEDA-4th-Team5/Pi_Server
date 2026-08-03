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
    Completion completion)
    : storage_(storage),
      database_(database),
      config_(std::move(config)),
      completion_(std::move(completion)) {
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
}

bool EvidenceCaptureWorker::scheduleSession(EvidenceCaptureRequest request) {
    if (request.sessionId < 0 || request.slotId.empty() || !request.channel)
        return false;
    std::lock_guard lock(mutex_);
    if (!running_ || stopping_) return false;
    if (!scheduledSessions_.insert(request.sessionId).second) {
        util::logLine("EVIDENCE_CAPTURE",
            "duplicate schedule ignored session=" +
            std::to_string(request.sessionId) + " slot=" + request.slotId);
        return true;
    }
    if (jobs_.size() + 2 > config_.maxPendingJobs) {
        scheduledSessions_.erase(request.sessionId);
        util::logError("Evidence queue full: session=" +
                       std::to_string(request.sessionId) + " slot=" +
                       request.slotId);
        return false;
    }
    const auto now = Clock::now();
    jobs_.push(Job{now, nextSequence_++, request,
                   EvidenceReason::OccupancyStart});
    jobs_.push(Job{request.startedAtMonotonic + config_.overstayDelay,
                   nextSequence_++, std::move(request),
                   EvidenceReason::Overstay});
    const auto& queued = jobs_.top().request;
    util::logLine("EVIDENCE_CAPTURE",
        "start capture scheduled session=" +
        std::to_string(queued.sessionId) + " slot=" + queued.slotId +
        " channel=" + queued.channel->channel_id +
        " reason=OCCUPANCY_START_EVIDENCE");
    util::logLine("EVIDENCE_CAPTURE",
        "overstay capture scheduled session=" +
        std::to_string(queued.sessionId) + " slot=" + queued.slotId +
        " channel=" + queued.channel->channel_id +
        " reason=OVERSTAY_EVIDENCE delay_ms=" +
        std::to_string(config_.overstayDelay.count()));
    condition_.notify_all();
    return true;
}

void EvidenceCaptureWorker::cancelSession(const std::int64_t session_id) {
    if (session_id < 0) return;
    {
        std::lock_guard lock(mutex_);
        canceledSessions_.insert(session_id);
    }
    condition_.notify_all();
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
            if (canceledSessions_.contains(job.request.sessionId)) {
                util::logLine("EVIDENCE_CAPTURE",
                    "capture canceled session=" +
                    std::to_string(job.request.sessionId) + " slot=" +
                    job.request.slotId + " channel=" +
                    job.request.channel->channel_id + " reason=" +
                    toString(job.reason));
                if (job.reason == EvidenceReason::Overstay) {
                    scheduledSessions_.erase(job.request.sessionId);
                    canceledSessions_.erase(job.request.sessionId);
                }
                continue;
            }
            const auto completed_session_id = job.request.sessionId;
            const auto completed_reason = job.reason;
            lock.unlock();
            process(std::move(job));
            lock.lock();
            if (completed_reason == EvidenceReason::Overstay) {
                scheduledSessions_.erase(completed_session_id);
                canceledSessions_.erase(completed_session_id);
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
                                 job.reason, {}, false, false, {}};
    if (canceled(session_id)) return;

    try {
        const std::string path = storage_.saveEvidenceSnapshot(
            job.request.channel, session_id, job.request.slotId, reason,
            job.request.roi);
        result.imagePath = path;
        std::error_code file_error;
        if (path.empty() ||
            !std::filesystem::is_regular_file(path, file_error) || file_error) {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            result.imagePath.clear();
            result.message = "FrameBuffer/ROI/image file save failed";
            util::logError("Evidence capture failed: session=" +
                std::to_string(session_id) + " slot=" + job.request.slotId +
                " channel=" + channel_id + " reason=" + reason +
                " error=" + result.message);
            emit(std::move(result));
            return;
        }
        const auto inserted = database_.insertEvidenceImage(
            session_id, path, reason, parking_timer::utcNow());
        if (inserted != database::EvidenceInsertResult::Inserted) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            result.imagePath.clear();
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
            " reason=" + reason + " path=" + path);
        emit(std::move(result));
    } catch (const std::exception& error) {
        if (!result.imagePath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(result.imagePath, ignored);
        }
        result.imagePath.clear();
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

#include "parking/SlotTransitionActor.hpp"

#include "util/Logger.hpp"

#include <algorithm>
#include <exception>
#include <limits>

namespace parking {
namespace {

class ProcessingReset {
public:
    ProcessingReset(std::mutex& mutex,
                    bool& processing,
                    std::condition_variable& idleCondition) noexcept
        : mutex_(mutex),
          processing_(processing),
          idle_condition_(idleCondition) {}

    ~ProcessingReset() {
        std::lock_guard lock(mutex_);
        processing_ = false;
        idle_condition_.notify_all();
    }

private:
    std::mutex& mutex_;
    bool& processing_;
    std::condition_variable& idle_condition_;
};

}  // namespace

SlotTransitionActor::SlotTransitionActor(
    database::SessionTransitionStore& store,
    Config config,
    EffectSink effectSink)
    : store_(store),
      config_(std::move(config)),
      effect_sink_(std::move(effectSink)) {
    config_.transportAdmissionCapacity =
        std::max<std::size_t>(1, config_.transportAdmissionCapacity);
    config_.durablePendingCapacity =
        std::max<std::size_t>(1, config_.durablePendingCapacity);
    config_.retryDelay = std::max(config_.retryDelay,
                                  std::chrono::milliseconds(1));
}

SlotTransitionActor::~SlotTransitionActor() {
    if (!stopAndDrain(std::chrono::seconds(30))) {
        util::logError(
            "SlotTransitionActor shutdown timed out; durable commands remain");
        // A join here can wait forever on a persistent SQLite failure while
        // an owning callback target unwinds. The application shutdown barrier
        // handles the explicit false result by retaining owners and failing
        // closed; destructor-only teardown must not continue past that barrier.
        std::terminate();
    }
}

bool SlotTransitionActor::start() {
    if (!store_.ready()) {
        util::logError(
            "SlotTransitionActor start rejected: runtime schema is not ready");
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_) return true;
        stop_requested_ = false;
        drain_cutoff_epoch_ms_ = 0;
        ingress_open_ = false;
        started_ = true;
    }
    try {
        for (;;) {
            bool progressed = false;
            while (processRunnable(nowEpochMs())) progressed = true;
            progressed = processDueDeadlines(nowEpochMs()) || progressed;
            while (processRunnable(nowEpochMs())) progressed = true;
            progressed = processEffects(nowEpochMs()) || progressed;
            if (!progressed) break;
        }
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        started_ = false;
        util::logError("SlotTransitionActor startup replay failed: " +
                       std::string(error.what()));
        return false;
    } catch (...) {
        std::lock_guard lock(mutex_);
        started_ = false;
        return false;
    }
    try {
        worker_ = std::thread(&SlotTransitionActor::run, this);
    } catch (...) {
        std::lock_guard lock(mutex_);
        started_ = false;
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        ingress_open_ = true;
        condition_.notify_all();
    }
    return true;
}

SlotSubmitResult SlotTransitionActor::submit(
    const SlotTransitionCommand& command) {
    if (command.commandId.empty() || command.slotId.empty() ||
        command.sourceIdentity.empty()) {
        return {SlotSubmitCode::Conflict, command.commandId,
                "slot transition command identity is incomplete"};
    }
    std::lock_guard lock(mutex_);
    if (!started_ || !ingress_open_ || stop_requested_) {
        return {SlotSubmitCode::Closed, command.commandId,
                "slot transition ingress is closed"};
    }
    const std::string identity_key =
        std::string(toString(command.kind)) + "|" + command.sourceIdentity;
    if (const auto existing = ingress_identity_index_.find(identity_key);
        existing != ingress_identity_index_.end()) {
        for (const auto& [slot_id, lane] : ingress_by_slot_) {
            (void)slot_id;
            const auto queued = std::find_if(
                lane.begin(), lane.end(), [&existing](const IngressItem& item) {
                    return item.command.commandId == existing->second;
                });
            if (queued == lane.end()) continue;
            const auto& stored = queued->command;
            const bool same = sameSlotSourceFact(stored, command);
            return {same ? SlotSubmitCode::Existing
                         : SlotSubmitCode::Conflict,
                    existing->second,
                    same ? "source fact already waits for durable admission"
                         : "queued source identity payload conflict"};
        }
        return {SlotSubmitCode::RetryableFailure, existing->second,
                "ingress identity index is inconsistent"};
    }
    if (ingress_size_ >= config_.transportAdmissionCapacity) {
        return {SlotSubmitCode::QueueFull, command.commandId,
                "slot transition transport queue is full"};
    }
    ingress_by_slot_[command.slotId].push_back(
        {command, ++next_transport_ordinal_, 0});
    ingress_identity_index_.emplace(identity_key, command.commandId);
    ++ingress_size_;
    condition_.notify_all();
    return {SlotSubmitCode::TransportQueued, command.commandId,
            "normalized command queued for durable admission"};
}

void SlotTransitionActor::closeIngress() {
    std::lock_guard lock(mutex_);
    ingress_open_ = false;
}

bool SlotTransitionActor::stopAndDrain(
    const std::chrono::milliseconds timeout) {
    closeIngress();
    {
        std::lock_guard lock(mutex_);
        if (!started_) return true;
        stop_requested_ = true;
        if (drain_cutoff_epoch_ms_ == 0)
            drain_cutoff_epoch_ms_ = nowEpochMs();
        condition_.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
        std::unique_lock lock(mutex_);
        if (!idle_condition_.wait_until(lock, deadline,
                [this] { return !started_; })) return false;
    }
    if (worker_.joinable()) worker_.join();
    return true;
}

bool SlotTransitionActor::waitUntilIdle(
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            idle_condition_.wait_until(lock, deadline, [this] {
                return ingress_size_ == 0 && !admitting_ &&
                    !deadline_admitting_ && !processing_;
            });
            if (ingress_size_ != 0 || admitting_ || deadline_admitting_ ||
                processing_) return false;
        }
        const auto next_deadline = store_.nextDeadlineEpochMs();
        const bool due_deadline =
            next_deadline && *next_deadline <= nowEpochMs();
        if (store_.pendingCount() == 0 &&
            store_.pendingEffectCount() == 0 && !due_deadline) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::unique_lock lock(mutex_);
        idle_condition_.wait_until(lock, deadline);
    }
}

bool SlotTransitionActor::processIngress(const std::int64_t now_epoch_ms) {
    bool attempted = false;
    for (;;) {
        SlotTransitionCommand command;
        std::uint64_t selected_ordinal =
            std::numeric_limits<std::uint64_t>::max();
        {
            std::lock_guard lock(mutex_);
            for (const auto& [slot_id, lane] : ingress_by_slot_) {
                (void)slot_id;
                if (lane.empty() ||
                    lane.front().nextAttemptAtEpochMs > now_epoch_ms ||
                    lane.front().transportOrdinal >= selected_ordinal) {
                    continue;
                }
                selected_ordinal = lane.front().transportOrdinal;
                command = lane.front().command;
            }
            if (selected_ordinal ==
                std::numeric_limits<std::uint64_t>::max()) break;
            admitting_ = true;
        }

        SlotAdmissionResult admission;
        try {
            admission = store_.admit(command, config_.durablePendingCapacity);
        } catch (const std::exception& error) {
            admission.code = SlotAdmissionCode::RetryableFailure;
            admission.commandId = command.commandId;
            admission.message = error.what();
        } catch (...) {
            admission.code = SlotAdmissionCode::RetryableFailure;
            admission.commandId = command.commandId;
            admission.message = "unknown durable admission failure";
        }

        {
            std::lock_guard lock(mutex_);
            admitting_ = false;
            auto lane = ingress_by_slot_.find(command.slotId);
            if (lane != ingress_by_slot_.end() && !lane->second.empty() &&
                lane->second.front().command.commandId == command.commandId) {
                const bool terminal = admission.accepted() ||
                    admission.code == SlotAdmissionCode::RejectedStale ||
                    admission.code == SlotAdmissionCode::Conflict;
                if (terminal) {
                    const std::string identity_key =
                        std::string(toString(command.kind)) + "|" +
                        command.sourceIdentity;
                    ingress_identity_index_.erase(identity_key);
                    lane->second.pop_front();
                    --ingress_size_;
                    if (lane->second.empty()) ingress_by_slot_.erase(lane);
                    idle_condition_.notify_all();
                } else {
                    lane->second.front().nextAttemptAtEpochMs =
                        now_epoch_ms + config_.retryDelay.count();
                    util::logWarn(
                        "Slot transition durable admission deferred: command=" +
                        command.commandId + " slot=" + command.slotId +
                        " reason=" + admission.message);
                }
            }
            condition_.notify_all();
        }
        attempted = true;
    }
    return attempted;
}

bool SlotTransitionActor::ingressOpen() const {
    std::lock_guard lock(mutex_);
    return ingress_open_;
}

std::int64_t SlotTransitionActor::nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool SlotTransitionActor::processRunnable(const std::int64_t now_epoch_ms) {
    bool made_progress = false;
    const auto runnable = store_.listRunnable(now_epoch_ms);
    for (const auto& command : runnable) {
        {
            std::lock_guard lock(mutex_);
            processing_ = true;
        }
        ProcessingReset processing_reset(
            mutex_, processing_, idle_condition_);
        CommittedOccupancyTransition result;
        try {
            result = store_.apply(command.command.commandId);
        } catch (const std::exception& error) {
            result.commandId = command.command.commandId;
            result.code = CommittedOccupancyCode::RetryableFailure;
            result.message = error.what();
        } catch (...) {
            result.commandId = command.command.commandId;
            result.code = CommittedOccupancyCode::RetryableFailure;
            result.message = "unknown authoritative transition failure";
        }
        if (result.code == CommittedOccupancyCode::RetryableFailure) {
            const bool deferred = store_.defer(
                command.command.commandId,
                now_epoch_ms + config_.retryDelay.count(), result.message);
            if (!deferred) {
                util::logError(
                    "Slot transition retry could not be persisted: command=" +
                    command.command.commandId);
            }
            made_progress = deferred || made_progress;
        } else {
            made_progress = true;
        }
    }
    return made_progress;
}

bool SlotTransitionActor::processDueDeadlines(
    const std::int64_t now_epoch_ms) {
    if (config_.checkpoint)
        config_.checkpoint(SlotActorCheckpoint::BeforeDeadlineLinearization);
    std::vector<std::string> blocked_slots;
    {
        std::lock_guard lock(mutex_);
        // This mutex section is the ordering point shared with submit(). A
        // queued slot is excluded from this deadline batch until its ingress
        // head crosses durable admission and reduces. Independent slots still
        // progress. A later submit may enqueue without waiting for SQLite but
        // is ordered after this deadline-admission marker.
        blocked_slots.reserve(ingress_by_slot_.size());
        for (const auto& [slot_id, lane] : ingress_by_slot_) {
            if (!lane.empty()) blocked_slots.push_back(slot_id);
        }
        deadline_admitting_ = true;
    }

    std::size_t admitted{};
    try {
        admitted = store_.admitDueDeadlines(
            now_epoch_ms, config_.durablePendingCapacity, blocked_slots);
        if (admitted != 0 && config_.checkpoint)
            config_.checkpoint(SlotActorCheckpoint::AfterDeadlineAdmission);
    } catch (const std::exception& error) {
        util::logError("Due exit deadline admission failed: " +
                       std::string(error.what()));
    } catch (...) {
        util::logError("Due exit deadline admission failed");
    }
    {
        std::lock_guard lock(mutex_);
        deadline_admitting_ = false;
        idle_condition_.notify_all();
        condition_.notify_all();
    }
    return admitted != 0;
}

bool SlotTransitionActor::processEffects(const std::int64_t now_epoch_ms) {
    bool made_progress = false;
    const auto effects = store_.listPendingEffects(now_epoch_ms);
    for (const auto& effect : effects) {
        {
            std::lock_guard lock(mutex_);
            processing_ = true;
        }
        ProcessingReset processing_reset(
            mutex_, processing_, idle_condition_);
        bool accepted = false;
        std::string error;
        try {
            accepted = !effect_sink_ || effect_sink_(effect);
            if (!accepted) error = "effect sink rejected committed transition";
        } catch (const std::exception& failure) {
            error = failure.what();
        } catch (...) {
            error = "unknown committed effect failure";
        }

        bool persisted = false;
        if (accepted) {
            try {
                persisted = store_.completeEffects(effect.commandId);
                if (!persisted)
                    error = "effect acknowledgement could not be persisted";
            } catch (const std::exception& failure) {
                error = failure.what();
            } catch (...) {
                error = "unknown effect acknowledgement failure";
            }
        }
        if (!accepted || !persisted) {
            const bool deferred = store_.deferEffects(
                effect.commandId,
                now_epoch_ms + config_.retryDelay.count(), error);
            made_progress = deferred || made_progress;
            util::logError(
                "Committed occupancy effect deferred without domain "
                "compensation: command=" + effect.commandId + " slot=" +
                effect.slotId + " reason=" + error);
        } else {
            made_progress = true;
        }
    }
    return made_progress;
}

void SlotTransitionActor::purgeSettledIfDue(const std::int64_t now_epoch_ms) {
    if (config_.retentionPeriod.count() <= 0 || config_.purgeBatchSize == 0)
        return;
    const auto interval_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            config_.purgeInterval).count();
    if (last_purge_epoch_ms_ != 0 &&
        now_epoch_ms - last_purge_epoch_ms_ < interval_ms) {
        return;
    }
    last_purge_epoch_ms_ = now_epoch_ms;
    const auto retention_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            config_.retentionPeriod).count();
    const std::size_t removed = store_.purgeSettled(
        now_epoch_ms - retention_ms, config_.purgeBatchSize);
    if (removed != 0) {
        util::logInfo("Settled occupancy commands purged: count=" +
                      std::to_string(removed));
    }
}

void SlotTransitionActor::run() {
    for (;;) {
        bool progressed = false;
        try {
            const auto now = nowEpochMs();
            progressed = processIngress(now);
            while (processRunnable(nowEpochMs())) progressed = true;
            progressed = processDueDeadlines(nowEpochMs()) || progressed;
            while (processRunnable(nowEpochMs())) progressed = true;
            progressed = processEffects(nowEpochMs()) || progressed;
            // 정리는 진행(progress)으로 치지 않는다. progressed를 세우면
            // 폴링 대기를 건너뛰어 루프가 바쁘게 돌게 된다.
            purgeSettledIfDue(nowEpochMs());
        } catch (const std::exception& error) {
            util::logError("SlotTransitionActor iteration failed: " +
                           std::string(error.what()));
        } catch (...) {
            util::logError("SlotTransitionActor iteration failed");
        }

        std::unique_lock lock(mutex_);
        const bool stop = stop_requested_;
        if (stop && !processing_) {
            if (progressed) continue;
            if (ingress_size_ != 0 || admitting_) {
                condition_.wait_for(lock, config_.retryDelay);
                continue;
            }
            const auto drain_cutoff = drain_cutoff_epoch_ms_;
            lock.unlock();
            bool drain_remaining = true;
            try {
                drain_remaining = store_.pendingDrainCount(drain_cutoff) != 0;
            } catch (const std::exception& error) {
                util::logError("Slot transition drain status failed: " +
                               std::string(error.what()));
            } catch (...) {
                util::logError("Slot transition drain status failed");
            }
            lock.lock();
            if (drain_remaining) {
                condition_.wait_for(lock, config_.retryDelay);
                continue;
            }
            // Confirmations/deadlines that were not yet due at the shutdown
            // cutover remain durable and are replayed on the next start.
            started_ = false;
            idle_condition_.notify_all();
            return;
        }
        if (progressed) continue;

        auto wait_for = config_.retryDelay;
        try {
            if (const auto deadline = store_.nextDeadlineEpochMs()) {
                const auto remaining_ms = *deadline - nowEpochMs();
                if (remaining_ms > 0) {
                    wait_for = std::min(
                        wait_for, std::chrono::milliseconds(remaining_ms));
                }
                // A past-due deadline was already attempted in this actor
                // turn. If capacity or a same-slot ingress head blocked it,
                // pace the retry instead of spinning SQLite at 0 ms.
            }
        } catch (...) {
        }
        condition_.wait_for(lock, wait_for);
    }
}

}  // namespace parking

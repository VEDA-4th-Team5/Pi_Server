#pragma once

#include "database/SessionTransitionStore.hpp"
#include "parking/SlotTransitionTypes.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace parking {

enum class SlotSubmitCode {
    TransportQueued,
    Admitted,
    Existing,
    AlreadyTerminal,
    RejectedStale,
    Conflict,
    QueueFull,
    Closed,
    RetryableFailure
};

struct SlotSubmitResult {
    SlotSubmitCode code{SlotSubmitCode::RetryableFailure};
    std::string commandId;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == SlotSubmitCode::TransportQueued ||
               code == SlotSubmitCode::Admitted ||
               code == SlotSubmitCode::Existing ||
               code == SlotSubmitCode::AlreadyTerminal;
    }
};

enum class SlotActorCheckpoint {
    BeforeDeadlineLinearization,
    AfterDeadlineAdmission
};

class SlotTransitionActor {
public:
    using EffectSink =
        std::function<bool(const CommittedOccupancyTransition&)>;

    struct Config {
        std::size_t transportAdmissionCapacity{16};
        std::size_t durablePendingCapacity{100};
        std::chrono::milliseconds retryDelay{50};
        std::function<void(SlotActorCheckpoint)> checkpoint;
    };

    SlotTransitionActor(database::SessionTransitionStore& store,
                        Config config,
                        EffectSink effectSink);
    ~SlotTransitionActor();

    SlotTransitionActor(const SlotTransitionActor&) = delete;
    SlotTransitionActor& operator=(const SlotTransitionActor&) = delete;

    bool start();
    SlotSubmitResult submit(const SlotTransitionCommand& command);
    void closeIngress();
    bool stopAndDrain(std::chrono::milliseconds timeout);
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);
    [[nodiscard]] bool ingressOpen() const;

private:
    struct IngressItem {
        SlotTransitionCommand command;
        std::uint64_t transportOrdinal{};
        std::int64_t nextAttemptAtEpochMs{};
    };

    void run();
    bool processIngress(std::int64_t nowEpochMs);
    bool processRunnable(std::int64_t nowEpochMs);
    bool processDueDeadlines(std::int64_t nowEpochMs);
    bool processEffects(std::int64_t nowEpochMs);
    [[nodiscard]] static std::int64_t nowEpochMs();

    database::SessionTransitionStore& store_;
    Config config_;
    EffectSink effect_sink_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idle_condition_;
    std::thread worker_;
    std::unordered_map<std::string, std::deque<IngressItem>>
        ingress_by_slot_;
    std::unordered_map<std::string, std::string> ingress_identity_index_;
    std::size_t ingress_size_{};
    std::uint64_t next_transport_ordinal_{};
    bool admitting_{};
    bool deadline_admitting_{};
    bool started_{};
    bool ingress_open_{};
    bool stop_requested_{};
    bool processing_{};
    std::int64_t drain_cutoff_epoch_ms_{};
};

}  // namespace parking

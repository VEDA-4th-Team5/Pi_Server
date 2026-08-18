#pragma once

#include "event/FireAlarmEvent.hpp"
#include "event/FirePersistence.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace database {
class EventDatabase;
}

namespace event {

struct FireChannelBinding {
    std::string sensorId;
    std::string channelId;
    std::string retainedTopic;
};

struct FireBindingParseResult {
    std::vector<FireChannelBinding> bindings;
    std::string error;

    [[nodiscard]] bool valid() const noexcept {
        return error.empty() && !bindings.empty();
    }
};

[[nodiscard]] FireBindingParseResult parseFireChannelBindingsStrict(
    const std::string& specification,
    const std::string& retained_topic_prefix);

enum class FireCommandStatus {
    Rejected,
    QueueFull,
    Queued,
    DurablyCommitted,
    Idempotent,
    Failed
};

struct FireCommandResult {
    std::uint64_t ticket{};
    FireCommandStatus status{FireCommandStatus::Rejected};
    std::string channelId;
    std::uint64_t fireRevision{};
    std::string error;
    bool retryable{};
    bool blocksChannel{};
};

class FireAlarmService {
public:
    struct Config {
        std::string cameraId;
        std::string lifecycleTopicPrefix{"parking/v1/events"};
        std::size_t commandQueueCapacity{128};
        std::chrono::milliseconds transientFailureRetryDelay{
            std::chrono::milliseconds(100)};
    };

    using CommitObserver = std::function<void(const FireCommandResult&)>;
    using AckReadiness =
        std::function<bool(const std::string&, std::uint64_t)>;
    using MutationBegin = std::function<bool(const std::string&)>;
    using MutationEnd = std::function<void(const std::string&)>;

    FireAlarmService(database::EventDatabase& database,
                     Config config,
                     std::vector<FireChannelBinding> bindings,
                     CommitObserver observer = {},
                     AckReadiness ack_readiness = {},
                     MutationBegin mutation_begin = {},
                     MutationEnd mutation_end = {});
    ~FireAlarmService();

    FireAlarmService(const FireAlarmService&) = delete;
    FireAlarmService& operator=(const FireAlarmService&) = delete;

    /** Creates/validates every channel and its revision-1 retained snapshot. */
    bool initialize();
    bool start();

    FireCommandResult submitSignal(FireSignal signal);
    FireCommandResult submitAcknowledge(std::string channel_id,
                                        std::string alarm_id);
    /** Resolves an active alarm without advancing the physical sensor cursor. */
    FireCommandResult submitManualClear(std::string channel_id);

    void closeIngress() noexcept;
    bool drainCommitted(std::chrono::milliseconds timeout);
    bool stop(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    [[nodiscard]] bool configurationValid() const noexcept;
    [[nodiscard]] std::string configurationError() const;
    [[nodiscard]] std::size_t bindingCount() const noexcept;

private:
    enum class CommandKind { Signal, Acknowledge, ManualClear };
    struct Command {
        CommandKind kind{CommandKind::Signal};
        std::uint64_t ticket{};
        FireSignal signal;
        std::string channelId;
        std::string alarmId;
        std::size_t retryCount{};
        std::chrono::steady_clock::time_point retryAfter{};
        bool holdsChannelBlock{};
    };

    FireCommandResult enqueue(Command command);
    FireCommandResult process(const Command& command);
    FireCommandResult processSignal(const Command& command,
                                    const FireChannelBinding& binding);
    FireCommandResult processAcknowledge(const Command& command);
    FireCommandResult processManualClear(const Command& command);
    FireStoreMutationResult applyTransition(
        const FireStateMutation& mutation);
    const FireChannelBinding* findBySensor(const std::string& sensor_id) const;
    const FireChannelBinding* findByChannel(
        const std::string& channel_id) const;
    void run() noexcept;
    void notify(const FireCommandResult& result) noexcept;

    database::EventDatabase& database_;
    Config config_;
    std::vector<FireChannelBinding> bindings_;
    CommitObserver observer_;
    AckReadiness ack_readiness_;
    MutationBegin mutation_begin_;
    MutationEnd mutation_end_;
    std::string configuration_error_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable drained_condition_;
    std::deque<Command> queue_;
    std::map<std::string, std::uint64_t> blocked_channel_ticket_;
    std::map<std::pair<std::string, std::string>, std::uint64_t>
        pending_ack_ticket_;
    std::thread worker_;
    std::uint64_t next_ticket_{1};
    bool initialized_{};
    bool running_{};
    bool accepting_{};
    bool processing_{};
    bool stop_requested_{};
};

}  // namespace event

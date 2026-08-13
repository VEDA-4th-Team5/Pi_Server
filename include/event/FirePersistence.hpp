#pragma once

#include "event/FireAlarmEvent.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace event {

inline constexpr std::uint64_t kMaxFireRevision =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

enum class FireProtocolMode {
    Unseen,
    Legacy,
    Versioned
};

enum class FireDeliverySinkKind {
    RetainedState,
    LifecycleEvent
};

enum class FireDeliveryState {
    Pending,
    InFlight,
    Acknowledged
};

struct FireAlarmStateRecord {
    std::string channelId;
    std::string sensorId;
    std::string retainedTopic;
    FireAlarmLifecycle desiredLifecycle{FireAlarmLifecycle::Resolved};
    std::string activeAlarmId;
    std::string lastEventId;
    std::uint64_t fireRevision{};
    FireProtocolMode protocolMode{FireProtocolMode::Unseen};
    std::optional<std::string> activeBootId;
    std::optional<std::uint64_t> lastSourceSequence;
    std::string lastSignalJson;
    std::string updatedAt;
};

struct FireOutboxRecord {
    std::string deliveryKey;
    FireDeliverySinkKind sinkKind{FireDeliverySinkKind::LifecycleEvent};
    std::string logicalKey;
    std::string sensorId;
    std::string channelId;
    std::string eventId;
    std::string alarmId;
    std::uint64_t fireRevision{};
    std::string topic;
    std::string payloadJson;
    int qos{1};
    bool retain{};
    std::uint64_t attemptCount{};
    std::string nextAttemptAt;
    std::string lastError;
    FireDeliveryState deliveryState{FireDeliveryState::Pending};
    std::optional<std::uint64_t> acknowledgedRevision;
    std::string createdAt;
    std::string updatedAt;
};

struct FireChannelBootstrap {
    FireAlarmStateRecord initialState;
    FireOutboxRecord retainedDelivery;
};

enum class FireStoreMutationOutcome {
    Committed,
    Idempotent,
    Conflict,
    Invalid,
    Failed
};

struct FireStoreMutationResult {
    FireStoreMutationOutcome outcome{FireStoreMutationOutcome::Failed};
    std::uint64_t fireRevision{};
    std::string error;

    [[nodiscard]] bool committed() const noexcept {
        return outcome == FireStoreMutationOutcome::Committed ||
               outcome == FireStoreMutationOutcome::Idempotent;
    }
};

// expectedRevision is the durable compare-and-swap value. A lifecycle
// transition advances it by exactly one and supplies both delivery intents.
// A sequence-only duplicate keeps the revision and supplies neither intent.
struct FireStateMutation {
    std::uint64_t expectedRevision{};
    FireAlarmStateRecord nextState;
    std::optional<FireOutboxRecord> retainedDelivery;
    std::optional<FireOutboxRecord> lifecycleDelivery;
};

[[nodiscard]] const char* toString(FireAlarmLifecycle lifecycle) noexcept;
[[nodiscard]] const char* toString(FireProtocolMode mode) noexcept;
[[nodiscard]] const char* toString(FireDeliverySinkKind kind) noexcept;
[[nodiscard]] const char* toString(FireDeliveryState state) noexcept;

[[nodiscard]] std::optional<FireAlarmLifecycle> fireAlarmLifecycleFromString(
    const std::string& value) noexcept;
[[nodiscard]] std::optional<FireProtocolMode> fireProtocolModeFromString(
    const std::string& value) noexcept;
[[nodiscard]] std::optional<FireDeliverySinkKind> fireDeliverySinkFromString(
    const std::string& value) noexcept;
[[nodiscard]] std::optional<FireDeliveryState> fireDeliveryStateFromString(
    const std::string& value) noexcept;

}  // namespace event

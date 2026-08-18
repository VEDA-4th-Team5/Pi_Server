#include "event/FireAlarmService.hpp"

#include "database/EventDatabase.hpp"
#include "event/EventPayloadBuilder.hpp"
#include "sensor/SensorSequencePolicy.hpp"
#include "util/Logger.hpp"
#include "util/TimeUtil.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#include <utility>

namespace event {
namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string joinTopic(const std::string& prefix, const std::string& suffix) {
    if (prefix.empty()) return suffix;
    if (prefix.back() == '/') return prefix + suffix;
    return prefix + '/' + suffix;
}

std::string fireEventId(const std::string& channel_id,
                        const std::uint64_t revision) {
    return "fire-event:" + channel_id + ':' + std::to_string(revision);
}

std::string fireAlarmId(const std::string& channel_id,
                        const std::uint64_t revision) {
    return "fire-alarm:" + channel_id + ':' + std::to_string(revision);
}

std::string retainedDeliveryId(const std::string& channel_id) {
    return "fire-state:" + channel_id;
}

std::string lifecycleDeliveryId(const std::string& event_id) {
    return "fire-lifecycle:" + event_id;
}

std::string serializeSignal(const FireSignal& signal) {
    nlohmann::json document{
        {"sensor_id", signal.sensorId},
        {"detected", signal.detected},
        {"occurred_at_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(
             signal.occurredAt.time_since_epoch()).count()},
        {"source_transport", signal.sourceTransport},
        {"source_protocol", sensor::toString(signal.sourceProtocolVersion)},
        {"raw_payload", signal.rawPayload}};
    if (signal.sourceSequence) {
        document["source_sequence"] =
            std::to_string(*signal.sourceSequence);
    }
    if (signal.sourceBootId) document["source_boot_id"] = *signal.sourceBootId;
    return document.dump();
}

std::optional<FireSignal> deserializeSignal(const std::string& encoded) {
    try {
        const auto document = nlohmann::json::parse(encoded);
        if (!document.is_object()) return std::nullopt;
        FireSignal signal;
        signal.sensorId = document.value("sensor_id", std::string{});
        signal.detected = document.value("detected", false);
        signal.occurredAt = std::chrono::system_clock::time_point{
            std::chrono::milliseconds(document.value("occurred_at_ms", 0LL))};
        signal.sourceTransport =
            document.value("source_transport", std::string{"unknown"});
        const auto protocol = sensor::sensorProtocolVersionFromString(
            document.value("source_protocol", std::string{"LEGACY_V1"}));
        if (!protocol) return std::nullopt;
        signal.sourceProtocolVersion = *protocol;
        signal.rawPayload = document.value("raw_payload", std::string{});
        if (document.contains("source_sequence") &&
            document["source_sequence"].is_string()) {
            const auto sequence = document["source_sequence"].get<std::string>();
            std::uint64_t parsed{};
            const auto result = std::from_chars(
                sequence.data(), sequence.data() + sequence.size(), parsed);
            if (result.ec != std::errc{} ||
                result.ptr != sequence.data() + sequence.size()) {
                return std::nullopt;
            }
            signal.sourceSequence = parsed;
        }
        if (document.contains("source_boot_id") &&
            document["source_boot_id"].is_string()) {
            signal.sourceBootId =
                document["source_boot_id"].get<std::string>();
        }
        if (signal.sensorId.empty()) return std::nullopt;
        return signal;
    } catch (...) {
        return std::nullopt;
    }
}

FireProtocolMode protocolFor(const FireSignal& signal) {
    return signal.sourceProtocolVersion ==
            sensor::SensorProtocolVersion::BootEpochV2
        ? FireProtocolMode::Versioned
        : FireProtocolMode::Legacy;
}

sensor::SensorSequenceMode sequenceModeFor(const FireProtocolMode mode) {
    switch (mode) {
    case FireProtocolMode::Unseen:
        return sensor::SensorSequenceMode::Unseen;
    case FireProtocolMode::Legacy:
        return sensor::SensorSequenceMode::Legacy;
    case FireProtocolMode::Versioned:
        return sensor::SensorSequenceMode::Versioned;
    }
    return sensor::SensorSequenceMode::Unseen;
}

bool activeLifecycle(const FireAlarmLifecycle lifecycle) {
    return lifecycle == FireAlarmLifecycle::Open ||
           lifecycle == FireAlarmLifecycle::Acknowledged;
}

FireOutboxRecord makeDelivery(
    const FireDeliverySinkKind sink_kind,
    std::string delivery_key,
    std::string logical_key,
    const FireAlarmStateRecord& state,
    std::string event_id,
    std::string alarm_id,
    std::string topic,
    std::string payload,
    const std::string& timestamp) {
    FireOutboxRecord row;
    row.deliveryKey = std::move(delivery_key);
    row.sinkKind = sink_kind;
    row.logicalKey = std::move(logical_key);
    row.sensorId = state.sensorId;
    row.channelId = state.channelId;
    row.eventId = std::move(event_id);
    row.alarmId = std::move(alarm_id);
    row.fireRevision = state.fireRevision;
    row.topic = std::move(topic);
    row.payloadJson = std::move(payload);
    row.qos = 1;
    row.retain = sink_kind == FireDeliverySinkKind::RetainedState;
    row.nextAttemptAt = timestamp;
    row.deliveryState = FireDeliveryState::Pending;
    row.createdAt = timestamp;
    row.updatedAt = timestamp;
    return row;
}

FireCommandResult failedResult(const std::uint64_t ticket,
                               const std::string& channel_id,
                               const std::uint64_t revision,
                               std::string error) {
    return {ticket, FireCommandStatus::Failed, channel_id, revision,
            std::move(error)};
}

FireCommandResult retryableResult(const std::uint64_t ticket,
                                  const std::string& channel_id,
                                  const std::uint64_t revision,
                                  std::string error,
                                  const bool blocks_channel = true) {
    return {ticket, FireCommandStatus::Failed, channel_id, revision,
            std::move(error), true, blocks_channel};
}

FireCommandResult mutationResult(const std::uint64_t ticket,
                                 const std::string& channel_id,
                                 const FireStoreMutationResult& result) {
    if (result.outcome == FireStoreMutationOutcome::Committed) {
        return {ticket, FireCommandStatus::DurablyCommitted, channel_id,
                result.fireRevision, {}};
    }
    if (result.outcome == FireStoreMutationOutcome::Idempotent) {
        return {ticket, FireCommandStatus::Idempotent, channel_id,
                result.fireRevision, {}};
    }
    if (result.outcome == FireStoreMutationOutcome::Failed) {
        return retryableResult(ticket, channel_id, result.fireRevision,
                               result.error);
    }
    return failedResult(ticket, channel_id, result.fireRevision,
                        result.error);
}

}  // namespace

FireBindingParseResult parseFireChannelBindingsStrict(
    const std::string& specification,
    const std::string& retained_topic_prefix) {
    FireBindingParseResult result;
    if (trim(retained_topic_prefix).empty()) {
        result.error = "Fire retained topic prefix is empty";
        return result;
    }

    std::set<std::string> sensors;
    std::set<std::string> channels;
    std::set<std::string> topics;
    std::istringstream stream(specification);
    std::string entry;
    while (std::getline(stream, entry, ',')) {
        const auto normalized = trim(entry);
        if (normalized.empty()) {
            result.error = "Fire mapping contains an empty entry";
            return result;
        }
        const auto equals = normalized.find('=');
        if (equals == std::string::npos ||
            normalized.find('=', equals + 1) != std::string::npos) {
            result.error = "Fire mapping entry must be sensor=channel: " +
                           normalized;
            return result;
        }
        FireChannelBinding binding;
        binding.sensorId = trim(normalized.substr(0, equals));
        binding.channelId = trim(normalized.substr(equals + 1));
        if (binding.sensorId.empty() || binding.channelId.empty() ||
            binding.sensorId.find('/') != std::string::npos ||
            binding.channelId.find('/') != std::string::npos) {
            result.error = "Fire mapping contains an invalid identifier: " +
                           normalized;
            return result;
        }
        binding.retainedTopic =
            joinTopic(retained_topic_prefix, binding.channelId);
        if (!sensors.insert(binding.sensorId).second ||
            !channels.insert(binding.channelId).second ||
            !topics.insert(binding.retainedTopic).second) {
            result.error =
                "Fire sensor, channel, and retained topic must be one-to-one";
            return result;
        }
        result.bindings.push_back(std::move(binding));
    }
    if (result.bindings.empty()) {
        result.error = "Fire alarm is enabled but no mapping is configured";
    }
    return result;
}

FireAlarmService::FireAlarmService(database::EventDatabase& database,
    Config config,
    std::vector<FireChannelBinding> bindings,
    CommitObserver observer,
    AckReadiness ack_readiness,
    MutationBegin mutation_begin,
    MutationEnd mutation_end)
    : database_(database),
      config_(std::move(config)),
      bindings_(std::move(bindings)),
      observer_(std::move(observer)),
      ack_readiness_(std::move(ack_readiness)),
      mutation_begin_(std::move(mutation_begin)),
      mutation_end_(std::move(mutation_end)) {
    if (config_.cameraId.empty()) {
        configuration_error_ = "Fire camera ID is empty";
    } else if (config_.lifecycleTopicPrefix.empty()) {
        configuration_error_ = "Fire lifecycle topic prefix is empty";
    } else if (config_.commandQueueCapacity == 0) {
        configuration_error_ = "Fire command queue capacity is zero";
    } else if (config_.transientFailureRetryDelay <=
               std::chrono::milliseconds::zero()) {
        configuration_error_ = "Fire retry delay must be positive";
    } else if (static_cast<bool>(mutation_begin_) !=
               static_cast<bool>(mutation_end_)) {
        configuration_error_ =
            "Fire mutation boundary requires both begin and end callbacks";
    } else if (bindings_.empty()) {
        configuration_error_ = "Fire channel mapping is empty";
    }
    std::set<std::string> sensors;
    std::set<std::string> channels;
    std::set<std::string> topics;
    for (const auto& binding : bindings_) {
        if (binding.sensorId.empty() || binding.channelId.empty() ||
            binding.retainedTopic.empty() ||
            !sensors.insert(binding.sensorId).second ||
            !channels.insert(binding.channelId).second ||
            !topics.insert(binding.retainedTopic).second) {
            configuration_error_ =
                "Fire sensor, channel, and retained topic must be one-to-one";
            break;
        }
    }
}

FireAlarmService::~FireAlarmService() {
    if (!stop()) std::terminate();
}

bool FireAlarmService::initialize() {
    std::lock_guard lock(mutex_);
    if (!configuration_error_.empty() || running_) return false;
    if (initialized_) return true;

    const auto now = std::chrono::system_clock::now();
    const std::string timestamp = util::isoString(now);
    std::vector<FireChannelBootstrap> topology;
    topology.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
        FireSignal signal;
        signal.sensorId = binding.sensorId;
        signal.detected = false;
        signal.occurredAt = now;
        signal.sourceTransport = "bootstrap";
        signal.rawPayload = "bootstrap-resolved";

        FireAlarmStateRecord state;
        state.channelId = binding.channelId;
        state.sensorId = binding.sensorId;
        state.retainedTopic = binding.retainedTopic;
        state.desiredLifecycle = FireAlarmLifecycle::Resolved;
        state.lastEventId = fireEventId(binding.channelId, 1);
        state.fireRevision = 1;
        state.protocolMode = FireProtocolMode::Unseen;
        state.lastSignalJson = serializeSignal(signal);
        state.updatedAt = timestamp;

        const std::string alarm_id =
            "fire-alarm:" + binding.channelId + ":bootstrap";
        const std::string delivery_id = retainedDeliveryId(binding.channelId);
        const std::string payload = EventPayloadBuilder::buildFireJson(
            config_.cameraId, binding.channelId, signal,
            FireAlarmLifecycle::Resolved, state.lastEventId, alarm_id,
            state.fireRevision, delivery_id);
        auto retained = makeDelivery(
            FireDeliverySinkKind::RetainedState, delivery_id,
            binding.retainedTopic, state, state.lastEventId, alarm_id,
            binding.retainedTopic, payload, timestamp);
        topology.push_back({std::move(state), std::move(retained)});
    }
    const auto result = database_.initializeFireTopology(topology);
    if (!result.committed()) {
        configuration_error_ = result.error.empty()
            ? "Fire topology bootstrap failed"
            : result.error;
        return false;
    }

    const auto restored = database_.listFireAlarmStates();
    if (restored.size() != bindings_.size()) {
        configuration_error_ =
            "durable Fire channels do not match configured topology";
        return false;
    }
    for (const auto& row : restored) {
        const auto* binding = findByChannel(row.channelId);
        if (!binding || binding->sensorId != row.sensorId ||
            binding->retainedTopic != row.retainedTopic) {
            configuration_error_ =
                "durable Fire mapping differs from configured topology";
            return false;
        }
    }
    initialized_ = true;
    return true;
}

bool FireAlarmService::start() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || running_ || !configuration_error_.empty()) {
        return false;
    }
    stop_requested_ = false;
    accepting_ = true;
    running_ = true;
    try {
        worker_ = std::thread(&FireAlarmService::run, this);
    } catch (...) {
        running_ = false;
        accepting_ = false;
        return false;
    }
    return true;
}

FireCommandResult FireAlarmService::submitSignal(FireSignal signal) {
    Command command;
    command.kind = CommandKind::Signal;
    command.channelId = findBySensor(signal.sensorId)
        ? findBySensor(signal.sensorId)->channelId
        : std::string{};
    command.signal = std::move(signal);
    return enqueue(std::move(command));
}

FireCommandResult FireAlarmService::submitAcknowledge(
    std::string channel_id, std::string alarm_id) {
    Command command;
    command.kind = CommandKind::Acknowledge;
    command.channelId = std::move(channel_id);
    command.alarmId = std::move(alarm_id);
    return enqueue(std::move(command));
}

FireCommandResult FireAlarmService::submitManualClear(
    std::string channel_id) {
    Command command;
    command.kind = CommandKind::ManualClear;
    command.channelId = std::move(channel_id);
    return enqueue(std::move(command));
}

FireCommandResult FireAlarmService::enqueue(Command command) {
    std::lock_guard lock(mutex_);
    command.ticket = next_ticket_++;
    if (!running_ || !accepting_ || command.channelId.empty()) {
        return {command.ticket, FireCommandStatus::Rejected,
                command.channelId, 0, "Fire command ingress is not available"};
    }
    if (command.kind == CommandKind::Acknowledge) {
        const auto key = std::make_pair(command.channelId, command.alarmId);
        const auto existing = pending_ack_ticket_.find(key);
        if (existing != pending_ack_ticket_.end()) {
            return {existing->second, FireCommandStatus::Queued,
                    command.channelId, 0, {}};
        }
        const auto signal_reserve = std::min(
            config_.commandQueueCapacity, bindings_.size());
        const auto ack_capacity =
            config_.commandQueueCapacity - signal_reserve;
        if (queue_.size() >= ack_capacity) {
            return {command.ticket, FireCommandStatus::QueueFull,
                    command.channelId, 0,
                    "Fire ACK queue reserve is exhausted"};
        }
    }
    if (queue_.size() >= config_.commandQueueCapacity) {
        return {command.ticket, FireCommandStatus::QueueFull,
                command.channelId, 0, "Fire command queue is full"};
    }
    const auto ticket = command.ticket;
    const auto channel = command.channelId;
    if (command.kind == CommandKind::Acknowledge) {
        pending_ack_ticket_.emplace(
            std::make_pair(command.channelId, command.alarmId), ticket);
    }
    queue_.push_back(std::move(command));
    condition_.notify_one();
    return {ticket, FireCommandStatus::Queued, channel, 0, {}};
}

void FireAlarmService::closeIngress() noexcept {
    std::lock_guard lock(mutex_);
    accepting_ = false;
}

bool FireAlarmService::drainCommitted(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return drained_condition_.wait_for(lock, timeout, [this] {
        return queue_.empty() && !processing_;
    });
}

bool FireAlarmService::stop(const std::chrono::milliseconds timeout) {
    closeIngress();
    if (!drainCommitted(timeout)) return false;
    {
        std::lock_guard lock(mutex_);
        if (!running_) return true;
        stop_requested_ = true;
        condition_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    running_ = false;
    return true;
}

bool FireAlarmService::configurationValid() const noexcept {
    std::lock_guard lock(mutex_);
    return configuration_error_.empty();
}

std::string FireAlarmService::configurationError() const {
    std::lock_guard lock(mutex_);
    return configuration_error_;
}

std::size_t FireAlarmService::bindingCount() const noexcept {
    return bindings_.size();
}

const FireChannelBinding* FireAlarmService::findBySensor(
    const std::string& sensor_id) const {
    const auto found = std::find_if(
        bindings_.begin(), bindings_.end(), [&](const auto& binding) {
            return binding.sensorId == sensor_id;
        });
    return found == bindings_.end() ? nullptr : &*found;
}

const FireChannelBinding* FireAlarmService::findByChannel(
    const std::string& channel_id) const {
    const auto found = std::find_if(
        bindings_.begin(), bindings_.end(), [&](const auto& binding) {
            return binding.channelId == channel_id;
        });
    return found == bindings_.end() ? nullptr : &*found;
}

void FireAlarmService::run() noexcept {
    for (;;) {
        Command command;
        bool should_stop{};
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                condition_.wait(lock, [this] {
                    return stop_requested_ || !queue_.empty();
                });
                if (stop_requested_ && queue_.empty()) {
                    should_stop = true;
                    break;
                }

                const auto now = std::chrono::steady_clock::now();
                auto selected = queue_.end();
                auto next_wake = std::chrono::steady_clock::time_point{};
                for (auto iterator = queue_.begin();
                     iterator != queue_.end(); ++iterator) {
                    const auto blocked = blocked_channel_ticket_.find(
                        iterator->channelId);
                    if (blocked != blocked_channel_ticket_.end() &&
                        blocked->second != iterator->ticket) {
                        continue;
                    }
                    if (iterator->retryAfter ==
                            std::chrono::steady_clock::time_point{} ||
                        iterator->retryAfter <= now) {
                        selected = iterator;
                        break;
                    }
                    if (next_wake ==
                            std::chrono::steady_clock::time_point{} ||
                        iterator->retryAfter < next_wake) {
                        next_wake = iterator->retryAfter;
                    }
                }
                if (selected != queue_.end()) {
                    command = std::move(*selected);
                    queue_.erase(selected);
                    processing_ = true;
                    break;
                }
                if (next_wake ==
                    std::chrono::steady_clock::time_point{}) {
                    condition_.wait(lock);
                } else {
                    condition_.wait_until(lock, next_wake);
                }
            }
        }
        if (should_stop) break;

        FireCommandResult result;
        try {
            result = process(command);
        } catch (const std::exception& error) {
            result = retryableResult(command.ticket, command.channelId, 0,
                                     error.what());
        } catch (...) {
            result = retryableResult(command.ticket, command.channelId, 0,
                                     "unknown Fire command failure");
        }

        if (result.retryable) {
            if (command.retryCount == 0) {
                util::logWarn(
                    "Fire command remains admitted for durable retry: " +
                    result.error);
            }
            ++command.retryCount;
            command.retryAfter = std::chrono::steady_clock::now() +
                                 config_.transientFailureRetryDelay;
            std::lock_guard lock(mutex_);
            if (result.blocksChannel) {
                blocked_channel_ticket_[command.channelId] = command.ticket;
                command.holdsChannelBlock = true;
            } else if (command.holdsChannelBlock) {
                const auto blocked = blocked_channel_ticket_.find(
                    command.channelId);
                if (blocked != blocked_channel_ticket_.end() &&
                    blocked->second == command.ticket) {
                    blocked_channel_ticket_.erase(blocked);
                }
                command.holdsChannelBlock = false;
            }
            processing_ = false;
            queue_.push_back(std::move(command));
            condition_.notify_one();
            continue;
        }
        notify(result);

        {
            std::lock_guard lock(mutex_);
            if (command.kind == CommandKind::Acknowledge) {
                const auto key =
                    std::make_pair(command.channelId, command.alarmId);
                const auto pending = pending_ack_ticket_.find(key);
                if (pending != pending_ack_ticket_.end() &&
                    pending->second == command.ticket) {
                    pending_ack_ticket_.erase(pending);
                }
            }
            if (command.holdsChannelBlock) {
                const auto blocked = blocked_channel_ticket_.find(
                    command.channelId);
                if (blocked != blocked_channel_ticket_.end() &&
                    blocked->second == command.ticket) {
                    blocked_channel_ticket_.erase(blocked);
                }
            }
            processing_ = false;
            condition_.notify_one();
            if (queue_.empty()) drained_condition_.notify_all();
        }
    }
    std::lock_guard lock(mutex_);
    processing_ = false;
    drained_condition_.notify_all();
}

FireCommandResult FireAlarmService::process(const Command& command) {
    if (command.kind == CommandKind::Acknowledge) {
        return processAcknowledge(command);
    }
    if (command.kind == CommandKind::ManualClear) {
        return processManualClear(command);
    }
    const auto* binding = findBySensor(command.signal.sensorId);
    if (!binding) {
        return {command.ticket, FireCommandStatus::Rejected, {}, 0,
                "Fire sensor is not mapped"};
    }
    return processSignal(command, *binding);
}

FireCommandResult FireAlarmService::processSignal(
    const Command& command, const FireChannelBinding& binding) {
    const auto current = database_.getFireAlarmState(binding.channelId);
    if (!current) {
        return retryableResult(command.ticket, binding.channelId, 0,
                               "Fire durable state is unavailable");
    }
    const auto& signal = command.signal;
    const FireProtocolMode incoming_mode = protocolFor(signal);
    const bool versioned = signal.sourceProtocolVersion ==
        sensor::SensorProtocolVersion::BootEpochV2;
    if ((versioned && (!signal.sourceBootId ||
                       signal.sourceBootId->empty() ||
                       !signal.sourceSequence)) ||
        (!versioned && signal.sourceBootId)) {
        return {command.ticket, FireCommandStatus::Rejected,
                binding.channelId, current->fireRevision,
                "Fire protocol version, boot ID, and sequence disagree"};
    }

    sensor::SensorSequenceState cursor;
    cursor.mode = sequenceModeFor(current->protocolMode);
    cursor.activeBootId = current->activeBootId;
    cursor.lastSequence = current->lastSourceSequence;
    if (versioned && database_.isSensorBootIdRetired(
            "FIRE", signal.sensorId, *signal.sourceBootId)) {
        cursor.retiredBootIds.insert(*signal.sourceBootId);
    }
    const sensor::SensorSequenceFact fact{
        signal.sourceProtocolVersion,
        signal.sourceBootId,
        signal.sourceSequence};
    const auto sequence_decision =
        sensor::evaluateSensorSequence(cursor, fact);
    if (sequence_decision.code ==
        sensor::SensorSequenceDecisionCode::Duplicate) {
        const auto previous = deserializeSignal(current->lastSignalJson);
        if (previous && previous->detected != signal.detected) {
            return {command.ticket, FireCommandStatus::Rejected,
                    binding.channelId, current->fireRevision,
                    "same Fire sequence has conflicting state"};
        }
        return {command.ticket, FireCommandStatus::Idempotent,
                binding.channelId, current->fireRevision, {}};
    }
    if (!sequence_decision.accepted()) {
        return {command.ticket, FireCommandStatus::Rejected,
                binding.channelId, current->fireRevision,
                sequence_decision.reason};
    }

    const bool currently_active = activeLifecycle(current->desiredLifecycle);
    const bool transition = signal.detected != currently_active;
    FireAlarmStateRecord next = *current;
    next.protocolMode = incoming_mode;
    next.activeBootId = signal.sourceBootId;
    next.lastSourceSequence = signal.sourceSequence;
    next.lastSignalJson = serializeSignal(signal);
    next.updatedAt = util::isoString(std::chrono::system_clock::now());

    FireStateMutation mutation;
    mutation.expectedRevision = current->fireRevision;
    if (!transition) {
        const bool cursor_changed =
            next.protocolMode != current->protocolMode ||
            next.activeBootId != current->activeBootId ||
            next.lastSourceSequence != current->lastSourceSequence;
        if (!cursor_changed) {
            return {command.ticket, FireCommandStatus::Idempotent,
                    binding.channelId, current->fireRevision, {}};
        }
        mutation.nextState = std::move(next);
        const auto result = database_.applyFireStateMutation(mutation);
        return mutationResult(command.ticket, binding.channelId, result);
    }

    if (current->fireRevision == kMaxFireRevision) {
        return failedResult(command.ticket, binding.channelId,
                            current->fireRevision,
                            "Fire revision exhausted");
    }
    next.fireRevision = current->fireRevision + 1;
    next.lastEventId = fireEventId(binding.channelId, next.fireRevision);
    std::string alarm_id;
    if (signal.detected) {
        next.desiredLifecycle = FireAlarmLifecycle::Open;
        next.activeAlarmId = fireAlarmId(binding.channelId, next.fireRevision);
        alarm_id = next.activeAlarmId;
    } else {
        next.desiredLifecycle = FireAlarmLifecycle::Resolved;
        alarm_id = current->activeAlarmId;
        next.activeAlarmId.clear();
    }
    if (alarm_id.empty()) {
        return failedResult(command.ticket, binding.channelId,
                            current->fireRevision,
                            "resolved Fire transition lost alarm identity");
    }

    const std::string retained_id = retainedDeliveryId(binding.channelId);
    const std::string lifecycle_id = lifecycleDeliveryId(next.lastEventId);
    const std::string retained_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, binding.channelId, signal, next.desiredLifecycle,
        next.lastEventId, alarm_id, next.fireRevision, retained_id);
    const std::string lifecycle_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, binding.channelId, signal, next.desiredLifecycle,
        next.lastEventId, alarm_id, next.fireRevision, lifecycle_id);
    mutation.nextState = next;
    mutation.retainedDelivery = makeDelivery(
        FireDeliverySinkKind::RetainedState, retained_id,
        binding.retainedTopic, next, next.lastEventId, alarm_id,
        binding.retainedTopic, retained_payload, next.updatedAt);
    mutation.lifecycleDelivery = makeDelivery(
        FireDeliverySinkKind::LifecycleEvent, lifecycle_id, next.lastEventId,
        next, next.lastEventId, alarm_id,
        joinTopic(config_.lifecycleTopicPrefix, binding.channelId),
        lifecycle_payload, next.updatedAt);
    const auto result = applyTransition(mutation);
    return mutationResult(command.ticket, binding.channelId, result);
}

FireCommandResult FireAlarmService::processAcknowledge(
    const Command& command) {
    const auto* binding = findByChannel(command.channelId);
    if (!binding || command.alarmId.empty()) {
        return {command.ticket, FireCommandStatus::Rejected,
                command.channelId, 0,
                "Fire ACK channel/alarm is invalid"};
    }
    const auto current = database_.getFireAlarmState(command.channelId);
    if (!current) {
        return retryableResult(command.ticket, command.channelId, 0,
                               "Fire durable state is unavailable");
    }
    if (current->activeAlarmId != command.alarmId ||
        !activeLifecycle(current->desiredLifecycle)) {
        return {command.ticket, FireCommandStatus::Rejected,
                command.channelId, current->fireRevision,
                "Fire ACK does not match the active alarm"};
    }
    if (ack_readiness_ &&
        !ack_readiness_(command.channelId, current->fireRevision)) {
        return retryableResult(
            command.ticket, command.channelId, current->fireRevision,
            "Fire ACK is waiting for retained-state synchronization",
            false);
    }
    if (current->desiredLifecycle == FireAlarmLifecycle::Acknowledged) {
        return {command.ticket, FireCommandStatus::Idempotent,
                command.channelId, current->fireRevision, {}};
    }
    if (current->fireRevision == kMaxFireRevision) {
        return failedResult(command.ticket, command.channelId,
                            current->fireRevision,
                            "Fire revision exhausted");
    }
    auto signal = deserializeSignal(current->lastSignalJson);
    if (!signal) {
        return failedResult(command.ticket, command.channelId,
                            current->fireRevision,
                            "active Fire signal cannot be restored");
    }
    signal->occurredAt = std::chrono::system_clock::now();

    FireAlarmStateRecord next = *current;
    next.desiredLifecycle = FireAlarmLifecycle::Acknowledged;
    next.fireRevision = current->fireRevision + 1;
    next.lastEventId = fireEventId(command.channelId, next.fireRevision);
    next.updatedAt = util::isoString(signal->occurredAt);

    const std::string retained_id = retainedDeliveryId(command.channelId);
    const std::string lifecycle_id = lifecycleDeliveryId(next.lastEventId);
    const std::string retained_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, command.channelId, *signal,
        FireAlarmLifecycle::Acknowledged, next.lastEventId,
        command.alarmId, next.fireRevision, retained_id);
    const std::string lifecycle_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, command.channelId, *signal,
        FireAlarmLifecycle::Acknowledged, next.lastEventId,
        command.alarmId, next.fireRevision, lifecycle_id);

    FireStateMutation mutation;
    mutation.expectedRevision = current->fireRevision;
    mutation.nextState = next;
    mutation.retainedDelivery = makeDelivery(
        FireDeliverySinkKind::RetainedState, retained_id,
        binding->retainedTopic, next, next.lastEventId, command.alarmId,
        binding->retainedTopic, retained_payload, next.updatedAt);
    mutation.lifecycleDelivery = makeDelivery(
        FireDeliverySinkKind::LifecycleEvent, lifecycle_id, next.lastEventId,
        next, next.lastEventId, command.alarmId,
        joinTopic(config_.lifecycleTopicPrefix, command.channelId),
        lifecycle_payload, next.updatedAt);
    const auto result = applyTransition(mutation);
    return mutationResult(command.ticket, command.channelId, result);
}

FireCommandResult FireAlarmService::processManualClear(
    const Command& command) {
    const auto* binding = findByChannel(command.channelId);
    if (!binding) {
        return {command.ticket, FireCommandStatus::Rejected,
                command.channelId, 0, "Fire clear channel is invalid"};
    }
    const auto current = database_.getFireAlarmState(command.channelId);
    if (!current) {
        return retryableResult(command.ticket, command.channelId, 0,
                               "Fire durable state is unavailable");
    }
    if (!activeLifecycle(current->desiredLifecycle)) {
        return {command.ticket, FireCommandStatus::Idempotent,
                command.channelId, current->fireRevision, {}};
    }
    if (current->fireRevision == kMaxFireRevision) {
        return failedResult(command.ticket, command.channelId,
                            current->fireRevision,
                            "Fire revision exhausted");
    }
    if (current->activeAlarmId.empty()) {
        return failedResult(command.ticket, command.channelId,
                            current->fireRevision,
                            "manual Fire clear lost alarm identity");
    }

    auto signal = deserializeSignal(current->lastSignalJson);
    if (!signal) {
        signal = FireSignal{};
        signal->sensorId = current->sensorId;
        signal->sourceProtocolVersion =
            current->protocolMode == FireProtocolMode::Versioned
            ? sensor::SensorProtocolVersion::BootEpochV2
            : sensor::SensorProtocolVersion::LegacyV1;
        signal->sourceBootId = current->activeBootId;
        signal->sourceSequence = current->lastSourceSequence;
    }
    signal->detected = false;
    signal->occurredAt = std::chrono::system_clock::now();
    signal->sourceTransport = "manual-control";
    signal->rawPayload = "MANUAL_FIRE_CLEAR:" + command.channelId;

    FireAlarmStateRecord next = *current;
    next.desiredLifecycle = FireAlarmLifecycle::Resolved;
    next.fireRevision = current->fireRevision + 1;
    next.lastEventId = fireEventId(command.channelId, next.fireRevision);
    next.updatedAt = util::isoString(signal->occurredAt);
    const std::string alarm_id = current->activeAlarmId;
    next.activeAlarmId.clear();

    const std::string retained_id = retainedDeliveryId(command.channelId);
    const std::string lifecycle_id = lifecycleDeliveryId(next.lastEventId);
    const std::string retained_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, command.channelId, *signal,
        FireAlarmLifecycle::Resolved, next.lastEventId,
        alarm_id, next.fireRevision, retained_id);
    const std::string lifecycle_payload = EventPayloadBuilder::buildFireJson(
        config_.cameraId, command.channelId, *signal,
        FireAlarmLifecycle::Resolved, next.lastEventId,
        alarm_id, next.fireRevision, lifecycle_id);

    FireStateMutation mutation;
    mutation.expectedRevision = current->fireRevision;
    mutation.nextState = next;
    mutation.retainedDelivery = makeDelivery(
        FireDeliverySinkKind::RetainedState, retained_id,
        binding->retainedTopic, next, next.lastEventId, alarm_id,
        binding->retainedTopic, retained_payload, next.updatedAt);
    mutation.lifecycleDelivery = makeDelivery(
        FireDeliverySinkKind::LifecycleEvent, lifecycle_id, next.lastEventId,
        next, next.lastEventId, alarm_id,
        joinTopic(config_.lifecycleTopicPrefix, command.channelId),
        lifecycle_payload, next.updatedAt);
    const auto result = applyTransition(mutation);
    return mutationResult(command.ticket, command.channelId, result);
}

FireStoreMutationResult FireAlarmService::applyTransition(
    const FireStateMutation& mutation) {
    bool boundary_open{};
    if (mutation_begin_) {
        if (!mutation_begin_(mutation.nextState.channelId)) {
            return {FireStoreMutationOutcome::Failed,
                    mutation.expectedRevision,
                    "Fire readiness boundary is unavailable"};
        }
        boundary_open = true;
    }
    try {
        auto result = database_.applyFireStateMutation(mutation);
        if (boundary_open && mutation_end_)
            mutation_end_(mutation.nextState.channelId);
        return result;
    } catch (...) {
        if (boundary_open && mutation_end_) {
            try {
                mutation_end_(mutation.nextState.channelId);
            } catch (...) {
            }
        }
        throw;
    }
}

void FireAlarmService::notify(const FireCommandResult& result) noexcept {
    if (!observer_) return;
    try {
        observer_(result);
    } catch (...) {
        util::logError("Fire command observer threw an exception");
    }
}

}  // namespace event

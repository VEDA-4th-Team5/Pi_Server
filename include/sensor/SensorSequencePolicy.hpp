#pragma once

#include "sensor/SensorProtocolVersion.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace sensor {

enum class SensorSequenceMode {
    Unseen,
    Legacy,
    Versioned
};

struct SensorSequenceState {
    SensorSequenceMode mode{SensorSequenceMode::Unseen};
    std::optional<std::string> activeBootId;
    std::optional<std::uint64_t> lastSequence;
    std::set<std::string> retiredBootIds;
};

struct SensorSequenceFact {
    SensorProtocolVersion protocolVersion{SensorProtocolVersion::LegacyV1};
    std::optional<std::string> bootId;
    std::optional<std::uint64_t> sequence;
};

enum class SensorSequenceDecisionCode {
    Accept,
    Duplicate,
    Stale,
    LegacyDowngrade,
    RetiredBootId,
    Invalid
};

struct SensorSequenceDecision {
    SensorSequenceDecisionCode code{SensorSequenceDecisionCode::Invalid};
    std::string reason;

    [[nodiscard]] bool accepted() const noexcept {
        return code == SensorSequenceDecisionCode::Accept;
    }
};

[[nodiscard]] inline SensorSequenceDecision evaluateSensorSequence(
    const SensorSequenceState& state,
    const SensorSequenceFact& fact) {
    if (fact.protocolVersion == SensorProtocolVersion::LegacyV1) {
        if (fact.bootId && !fact.bootId->empty()) {
            return {SensorSequenceDecisionCode::Invalid,
                    "legacy frame must not carry a boot ID"};
        }
        if (state.mode == SensorSequenceMode::Versioned) {
            return {SensorSequenceDecisionCode::LegacyDowngrade,
                    "legacy frame rejected after versioned mode"};
        }
        if (!fact.sequence || state.mode == SensorSequenceMode::Unseen ||
            !state.lastSequence) {
            return {SensorSequenceDecisionCode::Accept, {}};
        }
        if (*fact.sequence == *state.lastSequence) {
            return {SensorSequenceDecisionCode::Duplicate,
                    "duplicate sensor sequence"};
        }
        if (*fact.sequence < *state.lastSequence) {
            return {SensorSequenceDecisionCode::Stale,
                    "stale sensor sequence"};
        }
        return {SensorSequenceDecisionCode::Accept, {}};
    }

    if (!fact.bootId || fact.bootId->empty() || !fact.sequence) {
        return {SensorSequenceDecisionCode::Invalid,
                "versioned frame requires boot ID and sequence"};
    }
    if (state.retiredBootIds.count(*fact.bootId) != 0) {
        return {SensorSequenceDecisionCode::RetiredBootId,
                "retired boot ID rejected"};
    }
    if (state.mode != SensorSequenceMode::Versioned) {
        // The numeric watermark from Legacy belongs to a different namespace.
        return {SensorSequenceDecisionCode::Accept, {}};
    }
    if (!state.activeBootId || state.activeBootId->empty() ||
        !state.lastSequence) {
        return {SensorSequenceDecisionCode::Invalid,
                "stored versioned cursor is incomplete"};
    }
    if (*fact.bootId != *state.activeBootId) {
        // A non-retired boot ID opens a new epoch.  Sequence restarts are
        // therefore compared only after this epoch is committed.
        return {SensorSequenceDecisionCode::Accept, {}};
    }
    if (*fact.sequence == *state.lastSequence) {
        return {SensorSequenceDecisionCode::Duplicate,
                "duplicate sensor sequence in active boot epoch"};
    }
    if (*fact.sequence < *state.lastSequence) {
        return {SensorSequenceDecisionCode::Stale,
                "stale sensor sequence in active boot epoch"};
    }
    return {SensorSequenceDecisionCode::Accept, {}};
}

inline void commitSensorSequence(SensorSequenceState& state,
                                 const SensorSequenceFact& fact) {
    if (fact.protocolVersion == SensorProtocolVersion::LegacyV1) {
        state.mode = SensorSequenceMode::Legacy;
        state.activeBootId.reset();
        if (fact.sequence) state.lastSequence = fact.sequence;
        return;
    }

    if (state.mode == SensorSequenceMode::Versioned &&
        state.activeBootId && fact.bootId &&
        *state.activeBootId != *fact.bootId) {
        state.retiredBootIds.insert(*state.activeBootId);
    }
    state.mode = SensorSequenceMode::Versioned;
    state.activeBootId = fact.bootId;
    state.lastSequence = fact.sequence;
}

}  // namespace sensor

#include "parking/ParkingSlotManager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace parking {
namespace {

std::atomic<std::uint64_t> gSessionSequence{0};

ParkingTransitionResult makeResult(
    ParkingTransitionCode code,
    const std::string& slotId,
    const std::string& sessionId,
    const std::string& message) {
    ParkingTransitionResult result;
    result.code = code;
    result.slotId = slotId;
    result.sessionId = sessionId;
    result.message = message;
    return result;
}

}  // namespace

ParkingSlotManager::ParkingSlotManager(
    std::vector<ParkingSlotConfig> configs) {
    if (configs.empty()) {
        throw std::invalid_argument(
            "ParkingSlotManager requires at least one slot");
    }

    for (auto& config : configs) {
        if (config.slotId.empty()) {
            throw std::invalid_argument(
                "ParkingSlotManager received empty slot id");
        }

        const auto slotId = config.slotId;
        const auto inserted = slots_.emplace(
            slotId,
            ParkingSlot(std::move(config)));

        if (!inserted.second) {
            throw std::invalid_argument(
                "ParkingSlotManager received duplicate slot id: " +
                slotId);
        }
    }
}

ParkingTransitionResult ParkingSlotManager::handle(
    const ParkingSensorEvent& event) {
    const auto slotIt = slots_.find(event.slotId);
    if (slotIt == slots_.end()) {
        return makeResult(
            ParkingTransitionCode::UnknownSlot,
            event.slotId,
            {},
            "unknown slot");
    }

    ParkingSlot& slot = slotIt->second;

    if (!slot.config.enabled) {
        return makeResult(
            ParkingTransitionCode::DisabledSlot,
            slot.config.slotId,
            {},
            "slot is disabled");
    }

    if (event.sensorId.empty() ||
        event.sensorId != slot.config.sensorId) {
        return makeResult(
            ParkingTransitionCode::SensorMismatch,
            slot.config.slotId,
            {},
            "sensor id does not match slot configuration");
    }

    if (event.state == ParkingSensorState::Occupied) {
        if (slot.activeSession.has_value()) {
            auto result = makeResult(
                ParkingTransitionCode::DuplicateOccupiedIgnored,
                slot.config.slotId,
                slot.activeSession->sessionId(),
                "slot is already occupied");
            result.session = slot.activeSession;
            return result;
        }

        const std::string sessionId =
            createSessionId(slot.config.slotId, event.occurredAt);

        slot.activeSession.emplace(
            sessionId,
            slot.config.slotId,
            event.sensorId,
            event.occurredAt,
            event.receivedMonotonic);

        auto result = makeResult(
            ParkingTransitionCode::SessionStarted,
            slot.config.slotId,
            sessionId,
            "parking occupancy session started");
        result.session = slot.activeSession;
        return result;
    }

    if (!slot.activeSession.has_value()) {
        return makeResult(
            ParkingTransitionCode::DuplicateVacantIgnored,
            slot.config.slotId,
            {},
            "slot is already vacant");
    }

    if (event.occurredAt < slot.activeSession->startedAt()) {
        auto result = makeResult(
            ParkingTransitionCode::InvalidTimestamp,
            slot.config.slotId,
            slot.activeSession->sessionId(),
            "vacant event time precedes session start");
        result.session = slot.activeSession;
        return result;
    }

    slot.activeSession->complete(event.occurredAt);
    ParkingOccupancySession completed = *slot.activeSession;
    slot.activeSession.reset();

    auto result = makeResult(
        ParkingTransitionCode::SessionCompleted,
        slot.config.slotId,
        completed.sessionId(),
        "parking occupancy session completed");
    result.session = std::move(completed);
    return result;
}

const ParkingSlot* ParkingSlotManager::findSlot(
    const std::string& slotId) const noexcept {
    const auto it = slots_.find(slotId);
    return it == slots_.end() ? nullptr : &it->second;
}

std::size_t ParkingSlotManager::slotCount() const noexcept {
    return slots_.size();
}

std::size_t ParkingSlotManager::activeSlotCount() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : slots_) {
        if (entry.second.config.enabled) {
            ++count;
        }
    }
    return count;
}

std::string ParkingSlotManager::createSessionId(
    const std::string& slotId,
    std::chrono::system_clock::time_point startedAt) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            startedAt.time_since_epoch()).count();

    const auto sequence = gSessionSequence.fetch_add(1);

    return "park_" + slotId + "_" +
           std::to_string(millis) + "_" +
           std::to_string(sequence);
}

const char* toString(ParkingTransitionCode code) noexcept {
    switch (code) {
    case ParkingTransitionCode::SessionStarted:
        return "session_started";
    case ParkingTransitionCode::SessionCompleted:
        return "session_completed";
    case ParkingTransitionCode::DuplicateOccupiedIgnored:
        return "duplicate_occupied_ignored";
    case ParkingTransitionCode::DuplicateVacantIgnored:
        return "duplicate_vacant_ignored";
    case ParkingTransitionCode::UnknownSlot:
        return "unknown_slot";
    case ParkingTransitionCode::DisabledSlot:
        return "disabled_slot";
    case ParkingTransitionCode::SensorMismatch:
        return "sensor_mismatch";
    case ParkingTransitionCode::InvalidTimestamp:
        return "invalid_timestamp";
    }

    return "unknown";
}

}  // namespace parking

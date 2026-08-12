#include "event/IvaSlotOccupancyAggregator.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace event {
namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string canonicalToken(std::string token) {
    token = lowerCopy(std::move(token));
    constexpr const char* longPrefix = "videosourcetoken-";
    if (token.starts_with(longPrefix)) {
        token = "vs-" + token.substr(
            std::char_traits<char>::length(longPrefix));
    }
    return token;
}

}  // namespace

IvaSlotOccupancyAggregator::IvaSlotOccupancyAggregator(
    const std::vector<parking::ParkingSlotConfig>& slots) {
    for (const auto& slot : slots) {
        if (!slot.enabled) continue;
        auto& state = statesBySlot_[slot.slotId];
        for (const auto& binding : slot.cameraBindings) {
            if (!binding.enabled) continue;
            state.configuredAreas.insert(observationKey(
                binding.cameraId, binding.videoSourceToken,
                binding.ruleName));
        }
    }
}

std::optional<IvaSlotOccupancyUpdate>
IvaSlotOccupancyAggregator::update(
    const std::string& slotId,
    const std::string& cameraId,
    const std::string& videoSourceToken,
    const std::string& ruleName,
    const bool active) {
    const std::string key = observationKey(
        cameraId, videoSourceToken, ruleName);
    std::lock_guard lock(mutex_);
    const auto slot = statesBySlot_.find(slotId);
    if (slot == statesBySlot_.end() ||
        !slot->second.configuredAreas.contains(key)) {
        return std::nullopt;
    }

    SlotState& state = slot->second;
    state.observedStates[key] = active;

    std::size_t activeCount{};
    for (const auto& [_, areaActive] : state.observedStates) {
        if (areaActive) ++activeCount;
    }

    const bool allAreasKnown =
        state.observedStates.size() == state.configuredAreas.size();
    if (activeCount == 0 && !allAreasKnown) return std::nullopt;

    const bool occupied = activeCount != 0;
    if (state.lastEmitted && *state.lastEmitted == occupied) {
        return std::nullopt;
    }
    state.lastEmitted = occupied;
    return IvaSlotOccupancyUpdate{
        slotId, occupied, activeCount, state.observedStates.size(),
        state.configuredAreas.size()};
}

std::string IvaSlotOccupancyAggregator::observationKey(
    const std::string& cameraId,
    const std::string& videoSourceToken,
    const std::string& ruleName) {
    return lowerCopy(cameraId) + '\n' + canonicalToken(videoSourceToken) +
           '\n' + lowerCopy(ruleName);
}

}  // namespace event

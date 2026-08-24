#include "device/ParkingAlertController.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace device {
namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](const unsigned char ch) { return std::isspace(ch); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](const unsigned char ch) { return std::isspace(ch); }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::uint64_t eventId(const std::int64_t sessionId) {
    return sessionId > 0 ? static_cast<std::uint64_t>(sessionId) : 0U;
}

}  // namespace

ParkingAlertController::ParkingAlertController(std::string slotMap,
                                               Backend backend)
    : slotMap_(parseSlotMap(slotMap)), backend_(std::move(backend)) {
    if (!backend_.setSlot || !backend_.clearSlot || !backend_.clearAll)
        throw std::invalid_argument("parking alert backend is incomplete");
}

bool ParkingAlertController::initialize(
    const std::vector<std::pair<std::string, std::int64_t>>& activeAlerts) {
    std::lock_guard lock(mutex_);
    try {
        backend_.clearAll();
        for (const auto& [slot, session] : activeAlerts) {
            const auto found = slotMap_.find(slot);
            if (found != slotMap_.end())
                backend_.setSlot(found->second, eventId(session));
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ParkingAlertController::handleEvent(const std::string_view eventType,
                                         const std::int64_t sessionId,
                                         const std::string_view slotId) {
    const bool activate = eventType == "NON_EV_ALERT" ||
                          eventType == "VIOLATION_TRIGGERED";
    const bool deactivate = eventType == "DEPARTURE";
    if (!activate && !deactivate) return true;

    std::lock_guard lock(mutex_);
    const auto found = slotMap_.find(std::string(slotId));
    if (found == slotMap_.end()) return false;
    try {
        if (activate)
            backend_.setSlot(found->second, eventId(sessionId));
        else
            backend_.clearSlot(found->second, eventId(sessionId));
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t ParkingAlertController::mappedSlotCount() const noexcept {
    return slotMap_.size();
}

std::unordered_map<std::string, std::uint32_t>
ParkingAlertController::parseSlotMap(const std::string& value) {
    std::unordered_map<std::string, std::uint32_t> result;
    std::unordered_set<std::uint32_t> indexes;
    std::istringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        const auto separator = token.find(':');
        if (separator == std::string::npos)
            throw std::invalid_argument("invalid parking alert slot map");
        const std::string slot = trim(token.substr(0, separator));
        const std::string indexText = trim(token.substr(separator + 1));
        std::size_t consumed{};
        unsigned long index{};
        try {
            index = std::stoul(indexText, &consumed);
        } catch (...) {
            throw std::invalid_argument("invalid parking alert slot index");
        }
        if (slot.empty() || consumed != indexText.size() || index >= 32U ||
            result.contains(slot) ||
            !indexes.insert(static_cast<std::uint32_t>(index)).second)
            throw std::invalid_argument("duplicate or out-of-range parking alert mapping");
        result.emplace(slot, static_cast<std::uint32_t>(index));
    }
    if (result.empty())
        throw std::invalid_argument("parking alert slot map is empty");
    return result;
}

}  // namespace device

#include "event/IvaEventResolver.hpp"

#include <cctype>

namespace event {
namespace {

std::string lowerCopy(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string canonicalToken(std::string token) {
    token = lowerCopy(std::move(token));
    constexpr const char* longPrefix = "videosourcetoken-";
    if (token.starts_with(longPrefix)) {
        token = "vs-" + token.substr(std::char_traits<char>::length(longPrefix));
    }
    return token;
}

bool identifierCharacter(const char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' || character == '-';
}

bool containsRuleName(const std::string& raw, const std::string& ruleName) {
    if (raw.empty() || ruleName.empty()) return false;
    const std::string haystack = lowerCopy(raw);
    const std::string needle = lowerCopy(ruleName);
    std::size_t position{};
    while ((position = haystack.find(needle, position)) != std::string::npos) {
        const bool leftBoundary =
            position == 0 || !identifierCharacter(haystack[position - 1]);
        const std::size_t end = position + needle.size();
        const bool rightBoundary =
            end == haystack.size() || !identifierCharacter(haystack[end]);
        if (leftBoundary && rightBoundary) return true;
        position = end;
    }
    return false;
}

void setError(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

}  // namespace

std::optional<IvaResolvedTarget> IvaEventResolver::resolve(
    const std::string& cameraId,
    const CameraEvent& cameraEvent,
    const std::vector<parking::ParkingSlotConfig>& slots,
    const std::vector<app::IvaAreaConfig>& areas,
    std::string* error) {
    if (cameraEvent.video_source_token.empty()) {
        setError(error, "missing or invalid video source token");
        return std::nullopt;
    }

    const std::string raw = cameraEvent.raw_topic + '\n' +
                            cameraEvent.raw_payload;
    const std::string eventToken =
        canonicalToken(cameraEvent.video_source_token);
    const parking::ParkingSlotConfig* matchedSlot = nullptr;
    const parking::SlotObservationBinding* matchedBinding = nullptr;

    for (const auto& slot : slots) {
        if (!slot.enabled) continue;
        for (const auto& binding : slot.cameraBindings) {
            if (!binding.enabled || binding.cameraId != cameraId ||
                canonicalToken(binding.videoSourceToken) != eventToken ||
                !containsRuleName(raw, binding.ruleName)) {
                continue;
            }
            if (matchedSlot != nullptr) {
                setError(error, "ambiguous camera/token/rule mapping");
                return std::nullopt;
            }
            matchedSlot = &slot;
            matchedBinding = &binding;
        }
    }

    if (matchedSlot == nullptr || matchedBinding == nullptr) {
        setError(error, "camera/token/rule mapping not found");
        return std::nullopt;
    }

    const app::IvaAreaConfig* matchedArea = nullptr;
    for (const auto& area : areas) {
        if (area.slot_id == matchedSlot->slotId) {
            if (matchedArea != nullptr) {
                setError(error, "duplicate ROI mapping for slot");
                return std::nullopt;
            }
            matchedArea = &area;
        }
    }
    if (matchedArea == nullptr) {
        setError(error, "ROI mapping not found for slot");
        return std::nullopt;
    }

    return IvaResolvedTarget{
        matchedSlot->slotId,
        matchedArea->channel_id,
        matchedBinding->ruleName,
        matchedArea->area_name,
        matchedArea->roi_x,
        matchedArea->roi_y,
        matchedArea->roi_width,
        matchedArea->roi_height};
}

}  // namespace event

#include "notification/TelegramMessageFormatter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>

namespace {

std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char value) {
        return static_cast<char>(
            std::toupper(static_cast<unsigned char>(value)));
    });
    return value;
}

std::string stringField(const nlohmann::json& document,
                        const char* key) {
    const auto found = document.find(key);
    return found != document.end() && found->is_string()
        ? found->get<std::string>() : std::string{};
}

std::string displayTimestamp(const nlohmann::json& document) {
    std::string timestamp = stringField(document, "timestamp");
    if (timestamp.empty()) timestamp = stringField(document, "occurred_at");
    if (timestamp.size() > 10 && timestamp[10] == 'T') timestamp[10] = ' ';
    return timestamp;
}

void appendField(std::ostringstream& output,
                 const std::string_view label,
                 const std::string& value) {
    if (!value.empty()) output << '\n' << label << ": " << value;
}

bool importantSensorError(const nlohmann::json& document) {
    const std::string severity = upperCopy(stringField(document, "severity"));
    return severity == "ERROR" || severity == "CRITICAL";
}

std::optional<nlohmann::json> parsePayload(
    const notification::TelegramMessage& message) {
    if (message.retain) return std::nullopt;
    try {
        auto document = nlohmann::json::parse(message.payload);
        if (!document.is_object()) return std::nullopt;
        return document;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

namespace notification {

bool TelegramMessageFormatter::shouldNotify(
    const TelegramMessage& message) const {
    const auto document = parsePayload(message);
    if (!document) return false;
    const std::string event_type =
        upperCopy(stringField(*document, "event_type"));
    return event_type == "FIRE_SUSPECTED" ||
           event_type == "FIRE_CLEARED" ||
           event_type == "NON_EV_ALERT" ||
           event_type == "OVERTIME_VIOLATION" ||
           event_type == "VIOLATION_TRIGGERED" ||
           (event_type == "SENSOR_ERROR" &&
            importantSensorError(*document));
}

std::string TelegramMessageFormatter::eventId(
    const TelegramMessage& message) const {
    const auto document = parsePayload(message);
    return document ? stringField(*document, "event_id") : std::string{};
}

std::string TelegramMessageFormatter::format(
    const TelegramMessage& message
) const {
    const auto document = parsePayload(message);
    if (!document) return {};

    const std::string event_type =
        upperCopy(stringField(*document, "event_type"));
    const std::string slot_id = stringField(*document, "slot_id");
    const std::string channel_id = stringField(*document, "channel_id");
    const std::string plate = stringField(*document, "plate_number");
    const std::string timestamp = displayTimestamp(*document);
    std::ostringstream output;

    if (event_type == "FIRE_SUSPECTED") {
        output << "🚨 화재 의심 감지";
        appendField(output, "감지 채널", channel_id);
        appendField(output, "센서", stringField(*document, "source_id"));
    } else if (event_type == "FIRE_CLEARED") {
        output << "✅ 화재 경보 해제";
        appendField(output, "감지 채널", channel_id);
    } else if (event_type == "NON_EV_ALERT") {
        output << "⚠️ 비전기차 주차 감지";
        appendField(output, "주차면", slot_id);
        appendField(output, "차량번호", plate.empty() ? "미확인" : plate);
    } else if (event_type == "OVERTIME_VIOLATION" ||
               event_type == "VIOLATION_TRIGGERED") {
        output << "⏱ 장기 점유 경고";
        appendField(output, "주차면", slot_id);
        appendField(output, "차량번호", plate.empty() ? "미확인" : plate);
        const auto seconds = document->value("occupied_seconds", 0);
        if (seconds > 0) {
            appendField(output, "기준 시간",
                        std::to_string(seconds / 60) + "분");
        }
    } else if (event_type == "SENSOR_ERROR" &&
               importantSensorError(*document)) {
        output << "⚠️ 센서 오류";
        appendField(output, "위치", slot_id.empty() ? "시스템" : slot_id);
        appendField(output, "오류 코드",
                    stringField(*document, "error_code"));
        appendField(output, "장치", stringField(*document, "device"));
    } else {
        return {};
    }
    appendField(output, "발생 시각", timestamp);
    return truncate(output.str());
}

std::string TelegramMessageFormatter::truncate(const std::string& text) {
    if (text.size() <= kTelegramMaxTextLength) {
        return text;
    }

    constexpr std::string_view suffix = "\n[truncated]";
    const std::size_t max_body =
        static_cast<std::size_t>(kTelegramMaxTextLength);
    if (max_body <= suffix.size()) {
        return std::string(suffix);
    }
    const std::size_t cutoff = max_body - suffix.size();
    return text.substr(0, cutoff) + std::string(suffix);
}

}  // namespace notification

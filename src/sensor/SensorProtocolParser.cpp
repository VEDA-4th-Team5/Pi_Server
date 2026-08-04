#include "sensor/SensorProtocolParser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <vector>

namespace sensor {
namespace {

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());
    return value;
}

std::string upper(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    return value;
}

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> fields;
    std::stringstream stream(value);
    std::string field;
    while (std::getline(stream, field, ':')) {
        fields.push_back(trim(field));
    }
    return fields;
}

bool parseUnsigned64(
    const std::string& text,
    std::uint64_t* result) {
    if (text.empty() || result == nullptr) {
        return false;
    }

    std::uint64_t value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }

    *result = value;
    return true;
}

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// Fields 4 and 5 (sequence, unix_epoch_ms) are optional and identical for the
// SENSOR and FIRE frames, so both parsers share this tail.
bool applyOptionalTail(
    const std::vector<std::string>& fields,
    std::optional<std::uint64_t>* sequence,
    std::chrono::system_clock::time_point* occurredAt,
    std::string* error) {
    if (fields.size() >= 4) {
        std::uint64_t parsedSequence = 0;
        if (!parseUnsigned64(fields[3], &parsedSequence)) {
            setError(error, "sensor sequence is not an unsigned integer");
            return false;
        }
        *sequence = parsedSequence;
    }

    if (fields.size() == 5) {
        std::uint64_t epochMs = 0;
        if (!parseUnsigned64(fields[4], &epochMs)) {
            setError(error, "sensor unix timestamp is invalid");
            return false;
        }

        using Millis = std::chrono::milliseconds;
        const auto maxMillis = static_cast<std::uint64_t>(
            std::numeric_limits<Millis::rep>::max());
        if (epochMs > maxMillis) {
            setError(error, "sensor unix timestamp is out of range");
            return false;
        }

        *occurredAt =
            std::chrono::system_clock::time_point(
                Millis(static_cast<Millis::rep>(epochMs)));
    }

    return true;
}

}  // namespace

std::optional<SensorProtocolMessage> SensorProtocolParser::parse(
    const std::string& line,
    std::chrono::system_clock::time_point receivedAt,
    std::string* error) const {
    const auto normalizedLine = trim(line);
    if (normalizedLine.empty()) {
        setError(error, "sensor message is empty");
        return std::nullopt;
    }

    const auto fields = split(normalizedLine);
    if (fields.size() < 3 || fields.size() > 5) {
        setError(
            error,
            "expected SENSOR:sensor_id:state[:sequence[:unix_ms]]");
        return std::nullopt;
    }

    if (upper(fields[0]) != "SENSOR") {
        setError(error, "unsupported sensor message type");
        return std::nullopt;
    }
    if (fields[1].empty()) {
        setError(error, "sensor id is empty");
        return std::nullopt;
    }

    SensorProtocolMessage message;
    message.sensorId = fields[1];
    message.occurredAt = receivedAt;

    const auto state = upper(fields[2]);
    if (state == "OCCUPIED") {
        message.state = parking::ParkingSensorState::Occupied;
    } else if (state == "VACANT") {
        message.state = parking::ParkingSensorState::Vacant;
    } else {
        setError(error, "sensor state must be OCCUPIED or VACANT");
        return std::nullopt;
    }

    if (!applyOptionalTail(
            fields, &message.sequence, &message.occurredAt, error)) {
        return std::nullopt;
    }

    return message;
}

std::optional<FireSensorMessage> SensorProtocolParser::parseFire(
    const std::string& line,
    std::chrono::system_clock::time_point receivedAt,
    std::string* error) const {
    const auto normalizedLine = trim(line);
    if (normalizedLine.empty()) {
        setError(error, "fire message is empty");
        return std::nullopt;
    }

    const auto fields = split(normalizedLine);
    if (fields.size() < 3 || fields.size() > 5) {
        setError(
            error,
            "expected FIRE:sensor_id:state[:sequence[:unix_ms]]");
        return std::nullopt;
    }

    if (upper(fields[0]) != "FIRE") {
        setError(error, "unsupported sensor message type");
        return std::nullopt;
    }
    if (fields[1].empty()) {
        setError(error, "fire sensor id is empty");
        return std::nullopt;
    }

    FireSensorMessage message;
    message.sensorId = fields[1];
    message.occurredAt = receivedAt;

    const auto state = upper(fields[2]);
    if (state == "DETECTED") {
        message.state = FireSensorState::Detected;
    } else if (state == "CLEARED") {
        message.state = FireSensorState::Cleared;
    } else {
        setError(error, "fire state must be DETECTED or CLEARED");
        return std::nullopt;
    }

    if (!applyOptionalTail(
            fields, &message.sequence, &message.occurredAt, error)) {
        return std::nullopt;
    }

    return message;
}

bool SensorProtocolParser::isFireLine(const std::string& line) {
    const auto normalizedLine = trim(line);
    const auto separator = normalizedLine.find(':');
    if (separator == std::string::npos) {
        return false;
    }
    return upper(normalizedLine.substr(0, separator)) == "FIRE";
}

}  // namespace sensor

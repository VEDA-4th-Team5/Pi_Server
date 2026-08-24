/** @file TimeUtil.cpp @brief 이벤트와 파일명에 사용하는 시각 변환 구현. */
#include "util/TimeUtil.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace util {

std::string isoString(const std::chrono::system_clock::time_point value) {
    const std::time_t now_time = std::chrono::system_clock::to_time_t(value);

    std::tm tm_buf{};
    localtime_r(&now_time, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

std::string nowIsoString() {
    return isoString(std::chrono::system_clock::now());
}

std::string nowStringForFilename() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count() % 1000;

    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_time, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
        << "_" << std::setw(3) << std::setfill('0') << ms;

    return oss.str();
}

std::optional<std::chrono::system_clock::time_point> parseIso8601Utc(
    const std::string& value) {
    if (value.size() < 20) return std::nullopt;
    std::tm utc{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) return std::nullopt;

    std::size_t cursor = 19;
    std::chrono::milliseconds fraction{};
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        int milliseconds{};
        int digits{};
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9') {
            if (digits < 3)
                milliseconds = milliseconds * 10 + (value[cursor] - '0');
            ++digits;
            ++cursor;
        }
        if (digits == 0) return std::nullopt;
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
        fraction = std::chrono::milliseconds(milliseconds);
    }

    int offsetSeconds{};
    if (cursor < value.size() &&
        (value[cursor] == 'Z' || value[cursor] == 'z')) {
        ++cursor;
    } else if (cursor + 6 == value.size() &&
               (value[cursor] == '+' || value[cursor] == '-') &&
               value[cursor + 3] == ':') {
        const auto digit = [&value](const std::size_t index) -> int {
            return value[index] >= '0' && value[index] <= '9'
                ? value[index] - '0' : -1;
        };
        const int h1 = digit(cursor + 1);
        const int h2 = digit(cursor + 2);
        const int m1 = digit(cursor + 4);
        const int m2 = digit(cursor + 5);
        if (h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0) return std::nullopt;
        const int hours = h1 * 10 + h2;
        const int minutes = m1 * 10 + m2;
        if (hours > 23 || minutes > 59) return std::nullopt;
        offsetSeconds = (hours * 60 + minutes) * 60;
        if (value[cursor] == '-') offsetSeconds = -offsetSeconds;
        cursor += 6;
    } else {
        return std::nullopt;
    }
    if (cursor != value.size()) return std::nullopt;

#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&utc);
#else
    const std::time_t seconds = timegm(&utc);
#endif
    if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(seconds) + fraction -
        std::chrono::seconds(offsetSeconds);
}

}

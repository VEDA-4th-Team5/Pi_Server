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
    localtime_r(&raw_time, &tm_buf);

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

}

#include "util/Logger.hpp"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_log_mutex;

// LOG_LEVEL 이 걸러내는 심각도 단계. 값이 클수록 더 수다스럽다.
enum class Severity { Off = 0, Error = 1, Warn = 2, Info = 3 };

// 태그를 환경변수 접미사로 바꾼다: "PLATE_OCR" -> LOG_PLATE_OCR.
// 영숫자가 아닌 문자는 '_' 로 접어서 셸에서 쓸 수 있는 이름만 만든다.
std::string normalizeTag(const std::string& tag) {
    std::string normalized;
    normalized.reserve(tag.size());
    for (const char raw_character : tag) {
        // <cctype> 함수는 unsigned char 범위 밖의 값에 UB 이므로 먼저 승격한다.
        const auto character = static_cast<unsigned char>(raw_character);
        normalized.push_back(
            std::isalnum(character) != 0
                ? static_cast<char>(std::toupper(character))
                : '_');
    }
    return normalized;
}

// 환경변수 값에서 공백을 걷어내고 소문자로 맞춘다. " True " -> "true"
std::string squash(const char* raw) {
    std::string value;
    if (raw == nullptr) return value;
    for (const char* cursor = raw; *cursor != '\0'; ++cursor) {
        const auto character = static_cast<unsigned char>(*cursor);
        if (std::isspace(character) == 0)
            value.push_back(static_cast<char>(std::tolower(character)));
    }
    return value;
}

// 알아볼 수 없는 값은 조용히 fallback 으로 둔다. 오타 하나로 로그가 통째로
// 사라지는 것보다, 설정이 안 먹은 채 계속 보이는 편이 디버깅에 안전하다.
bool parseBool(const char* raw, const bool fallback) {
    const std::string value = squash(raw);
    if (value.empty()) return fallback;
    if (value == "1" || value == "true" || value == "on" || value == "yes")
        return true;
    if (value == "0" || value == "false" || value == "off" || value == "no")
        return false;
    return fallback;
}

Severity parseLevel(const char* raw) {
    const std::string value = squash(raw);
    if (value.empty()) return Severity::Info;
    if (value == "off" || value == "none" || value == "silent")
        return Severity::Off;
    if (value == "error") return Severity::Error;
    if (value == "warn" || value == "warning") return Severity::Warn;
    return Severity::Info;
}

struct Filter {
    Severity level;
    bool tag_default;
    // 태그별 판정 결과 캐시. getenv 와 대문자 변환을 매 줄마다 하지 않는다.
    std::unordered_map<std::string, bool> decisions;
};

// 환경변수는 프로세스 시작 후 바뀌지 않으므로 최초 1회만 읽는다.
Filter& filterInstance() {
    static Filter instance{parseLevel(std::getenv("LOG_LEVEL")),
                           parseBool(std::getenv("LOG_DEFAULT"), true),
                           {}};
    return instance;
}

// 판정 결과를 태그 이름으로 캐시하므로, 기본값도 호출자가 아니라 태그 자체에서
// 결정해야 한다. 그래야 같은 태그가 어디서 불리든 항상 같은 답이 나온다.
bool isSeverityTag(const std::string& key) {
    return key == "INFO" || key == "WARN" || key == "ERROR";
}

/** @brief 이 태그/심각도를 출력할지 판정한다. g_log_mutex 를 잡고 호출해야 한다. */
bool allowedLocked(const std::string& tag, const Severity severity) {
    Filter& filter = filterInstance();
    if (filter.level == Severity::Off) return false;
    if (static_cast<int>(severity) > static_cast<int>(filter.level)) return false;

    const std::string key = normalizeTag(tag);
    if (const auto cached = filter.decisions.find(key);
        cached != filter.decisions.end()) {
        return cached->second;
    }

    // INFO/WARN/ERROR 는 LOG_DEFAULT 를 따르지 않는다. LOG_DEFAULT=false 로 태그
    // 로그를 전부 꺼도 경고·오류까지 사라지면 사고를 놓치기 때문이다.
    // 이 셋은 LOG_WARN=false 처럼 명시적으로 지정해야만 꺼진다.
    const bool fallback = isSeverityTag(key) ? true : filter.tag_default;
    const bool allowed =
        parseBool(std::getenv(("LOG_" + key).c_str()), fallback);
    filter.decisions.emplace(key, allowed);
    return allowed;
}

void emit(const std::string& tag,
          const std::string& message,
          const Severity severity,
          std::ostream& stream) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!allowedLocked(tag, severity)) return;
    stream << "[" << tag << "] " << message << std::endl;
}

}  // namespace

namespace util {

void logLine(const std::string& level, const std::string& message) {
    // 태그 로그는 INFO 수준으로 취급하고 LOG_<TAG> / LOG_DEFAULT 를 따른다.
    emit(level, message, Severity::Info, std::cout);
}

void logInfo(const std::string& message) {
    emit("INFO", message, Severity::Info, std::cout);
}

void logWarn(const std::string& message) {
    emit("WARN", message, Severity::Warn, std::cerr);
}

void logError(const std::string& message) {
    emit("ERROR", message, Severity::Error, std::cerr);
}

bool logEnabled(const std::string& tag) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return allowedLocked(tag, Severity::Info);
}

}

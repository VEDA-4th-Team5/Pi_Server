#pragma once
#include <string>

namespace util {
void logLine(const std::string& level, const std::string& message);
void logInfo(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);
}

#include "util/Logger.hpp"

#include <iostream>
#include <mutex>

namespace {
std::mutex g_log_mutex;
}

namespace util {

void logLine(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[" << level << "] " << message << std::endl;
}

void logInfo(const std::string& message) {
    logLine("INFO", message);
}

void logWarn(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "[WARN] " << message << std::endl;
}

void logError(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "[ERROR] " << message << std::endl;
}

}

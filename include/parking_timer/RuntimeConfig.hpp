#pragma once

#include <chrono>
#include <filesystem>

namespace parking_timer {

struct RuntimeConfig {
    std::chrono::milliseconds parking_timeout{std::chrono::seconds{60}};
    std::filesystem::path database_path{"events.sqlite3"};

    static RuntimeConfig load(const std::filesystem::path& file);
    void applyEnvironment();
};

}  // namespace parking_timer


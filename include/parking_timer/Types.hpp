#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace parking_timer {

enum class VehicleCategory {
    Ev,
    Phev,
    NonEv,
    Unknown,
};

struct LogRecord {
    std::int64_t id{};
    std::string car_number;
    int zone_id{};
    std::string status;
    std::string parked_at;
    std::optional<std::string> violation_at;
    std::optional<std::string> departed_at;
    std::optional<std::string> image_path_1;
    std::optional<std::string> image_path_2;
    bool is_canceled{};
};

struct EntryResult {
    bool accepted{};
    VehicleCategory category{VehicleCategory::Unknown};
    std::optional<std::int64_t> log_id;
    std::string message;
};

const char* toString(VehicleCategory category) noexcept;
std::string utcNow();

}  // namespace parking_timer


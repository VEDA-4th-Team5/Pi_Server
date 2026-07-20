#pragma once

#include "parking_timer/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace parking_timer {

class EventDatabase {
public:
    explicit EventDatabase(const std::filesystem::path& database_path);
    ~EventDatabase();

    EventDatabase(const EventDatabase&) = delete;
    EventDatabase& operator=(const EventDatabase&) = delete;

    void initialize(const std::filesystem::path& schema_file,
                    const std::filesystem::path& seed_file);

    VehicleCategory classifyVehicle(std::string_view car_number) const;

    std::int64_t insertParked(const std::string& car_number,
                              int zone_id,
                              const std::string& parked_at,
                              const std::string& image_path_1);

    bool markViolation(std::int64_t log_id,
                       const std::string& violation_at,
                       const std::string& image_path_2);

    bool cancelUnscheduled(std::int64_t log_id, const std::string& canceled_at);

    std::optional<LogRecord> departActiveByZone(int zone_id,
                                                const std::string& departed_at);

    std::optional<LogRecord> findActiveByZone(int zone_id) const;
    std::optional<LogRecord> findLogById(std::int64_t log_id) const;
    std::vector<LogRecord> listLogs() const;
    std::vector<std::pair<std::string, std::string>> listVehicles() const;

    void clearTimerLogs();

private:
    void executeSqlUnlocked(const std::string& sql);
    static std::string readTextFile(const std::filesystem::path& path);

    sqlite3* db_{};
    mutable std::mutex mutex_;
};

}  // namespace parking_timer

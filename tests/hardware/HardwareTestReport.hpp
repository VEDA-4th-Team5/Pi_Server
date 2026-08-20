#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hardware_test {

struct RunMetadata {
    std::string runId;
    std::string startedAt;
    std::string gitCommit;
    std::string device;
    int baud{};
    std::string mode;
    std::string scenario;
    int repeat{};
    std::string latencyMethod;
    std::string firmwareVersion;
    std::string hardwareId;
};

struct CaseResult {
    std::string testId;
    std::string sensorId;
    std::string action;
    int trial{};
    bool passed{};
    std::optional<long long> latencyMs;
    std::optional<double> observedValue;
    std::string observedUnit;
    std::string detail;
};

struct CaseSummary {
    std::string testId;
    std::string sensorId;
    std::string action;
    int attempts{};
    int passed{};
    int failed{};
    double successRate{};
    std::optional<long long> minimumLatencyMs;
    std::optional<double> averageLatencyMs;
    std::optional<long long> p95LatencyMs;
    std::optional<long long> maximumLatencyMs;
};

struct ReportPaths {
    std::filesystem::path csv;
    std::filesystem::path markdown;
};

class HardwareTestReport {
public:
    explicit HardwareTestReport(RunMetadata metadata);

    void record(CaseResult result);

    [[nodiscard]] const RunMetadata& metadata() const noexcept;
    [[nodiscard]] const std::vector<CaseResult>& results() const noexcept;
    [[nodiscard]] std::vector<CaseSummary> summaries() const;

    [[nodiscard]] ReportPaths writeTo(
        const std::filesystem::path& directory) const;

private:
    RunMetadata metadata_;
    std::vector<CaseResult> results_;
};

[[nodiscard]] std::string utcTimestamp();
[[nodiscard]] std::string defaultRunId();

}  // namespace hardware_test

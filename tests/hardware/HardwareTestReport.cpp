#include "HardwareTestReport.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace hardware_test {
namespace {

using SummaryKey = std::tuple<std::string, std::string, std::string>;

struct SummaryAccumulator {
    int attempts{};
    int passed{};
    std::vector<long long> latencies;
};

std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string escaped{"\""};
    for (const char character : value) {
        if (character == '\"') escaped += '\"';
        escaped += character;
    }
    escaped += '\"';
    return escaped;
}

std::string markdownEscape(std::string value) {
    std::size_t offset{};
    while ((offset = value.find('|', offset)) != std::string::npos) {
        value.replace(offset, 1, "\\|");
        offset += 2;
    }
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

std::string optionalInteger(const std::optional<long long>& value) {
    return value ? std::to_string(*value) : std::string{};
}

std::string optionalDecimal(const std::optional<double>& value) {
    if (!value) return {};
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << *value;
    return stream.str();
}

std::string safeFileStem(std::string value) {
    for (char& character : value) {
        const bool allowed =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_';
        if (!allowed) character = '_';
    }
    return value.empty() ? "hardware-test" : value;
}

void ensureWritable(const std::ofstream& output,
                    const std::filesystem::path& path) {
    if (!output)
        throw std::runtime_error("failed to write hardware test report: " +
                                 path.string());
}

}  // namespace

HardwareTestReport::HardwareTestReport(RunMetadata metadata)
    : metadata_(std::move(metadata)) {}

void HardwareTestReport::record(CaseResult result) {
    if (result.testId.empty())
        throw std::invalid_argument("hardware test ID must not be empty");
    if (result.trial <= 0)
        throw std::invalid_argument("hardware test trial must be positive");
    results_.push_back(std::move(result));
}

const RunMetadata& HardwareTestReport::metadata() const noexcept {
    return metadata_;
}

const std::vector<CaseResult>& HardwareTestReport::results() const noexcept {
    return results_;
}

std::vector<CaseSummary> HardwareTestReport::summaries() const {
    std::map<SummaryKey, SummaryAccumulator> accumulators;
    for (const auto& result : results_) {
        auto& accumulator = accumulators[{
            result.testId, result.sensorId, result.action}];
        ++accumulator.attempts;
        if (result.passed) ++accumulator.passed;
        if (result.latencyMs) accumulator.latencies.push_back(*result.latencyMs);
    }

    std::vector<CaseSummary> output;
    output.reserve(accumulators.size());
    for (auto& [key, accumulator] : accumulators) {
        CaseSummary summary;
        summary.testId = std::get<0>(key);
        summary.sensorId = std::get<1>(key);
        summary.action = std::get<2>(key);
        summary.attempts = accumulator.attempts;
        summary.passed = accumulator.passed;
        summary.failed = accumulator.attempts - accumulator.passed;
        summary.successRate = accumulator.attempts == 0
            ? 0.0
            : 100.0 * static_cast<double>(accumulator.passed) /
                static_cast<double>(accumulator.attempts);

        if (!accumulator.latencies.empty()) {
            std::sort(accumulator.latencies.begin(),
                      accumulator.latencies.end());
            summary.minimumLatencyMs = accumulator.latencies.front();
            summary.maximumLatencyMs = accumulator.latencies.back();
            const long long total = std::accumulate(
                accumulator.latencies.begin(), accumulator.latencies.end(),
                0LL);
            summary.averageLatencyMs =
                static_cast<double>(total) /
                static_cast<double>(accumulator.latencies.size());
            const std::size_t rank =
                (95U * accumulator.latencies.size() + 99U) / 100U;
            summary.p95LatencyMs = accumulator.latencies[rank - 1U];
        }
        output.push_back(std::move(summary));
    }
    return output;
}

ReportPaths HardwareTestReport::writeTo(
    const std::filesystem::path& directory) const {
    if (directory.empty())
        throw std::invalid_argument("hardware report directory is empty");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        throw std::runtime_error("failed to create hardware report directory: " +
                                 error.message());

    const std::string stem = safeFileStem(metadata_.runId);
    ReportPaths paths{directory / (stem + ".csv"),
                      directory / (stem + ".md")};

    {
        std::ofstream csv(paths.csv, std::ios::trunc);
        ensureWritable(csv, paths.csv);
        csv << "run_id,started_at,git_commit,device,baud,mode,scenario,repeat,"
               "latency_method,firmware_version,hardware_id,test_id,sensor_id,"
               "action,trial,status,latency_ms,observed_value,observed_unit,"
               "detail\n";
        for (const auto& result : results_) {
            csv << csvEscape(metadata_.runId) << ','
                << csvEscape(metadata_.startedAt) << ','
                << csvEscape(metadata_.gitCommit) << ','
                << csvEscape(metadata_.device) << ',' << metadata_.baud << ','
                << csvEscape(metadata_.mode) << ','
                << csvEscape(metadata_.scenario) << ',' << metadata_.repeat
                << ',' << csvEscape(metadata_.latencyMethod) << ','
                << csvEscape(metadata_.firmwareVersion) << ','
                << csvEscape(metadata_.hardwareId) << ','
                << csvEscape(result.testId) << ','
                << csvEscape(result.sensorId) << ','
                << csvEscape(result.action) << ',' << result.trial << ','
                << (result.passed ? "PASS" : "FAIL") << ','
                << optionalInteger(result.latencyMs) << ','
                << optionalDecimal(result.observedValue) << ','
                << csvEscape(result.observedUnit) << ','
                << csvEscape(result.detail) << '\n';
        }
        ensureWritable(csv, paths.csv);
    }

    {
        std::ofstream markdown(paths.markdown, std::ios::trunc);
        ensureWritable(markdown, paths.markdown);
        markdown << "# STM32 Sensor HIL Test Result\n\n"
                 << "- Run ID: `" << markdownEscape(metadata_.runId) << "`\n"
                 << "- Started at: `" << markdownEscape(metadata_.startedAt)
                 << "`\n"
                 << "- Git commit: `" << markdownEscape(metadata_.gitCommit)
                 << "`\n"
                 << "- Device: `" << markdownEscape(metadata_.device) << "`\n"
                 << "- Link: `" << markdownEscape(metadata_.mode) << " @ "
                 << metadata_.baud << "`\n"
                 << "- Scenario: `" << markdownEscape(metadata_.scenario)
                 << "`\n"
                 << "- Repeat: " << metadata_.repeat << "\n"
                 << "- Latency method: `"
                 << markdownEscape(metadata_.latencyMethod) << "`\n"
                 << "- Firmware version: `"
                 << markdownEscape(metadata_.firmwareVersion) << "`\n"
                 << "- Hardware ID: `"
                 << markdownEscape(metadata_.hardwareId) << "`\n\n"
                 << "## Summary\n\n"
                 << "| Test ID | Sensor | Action | Attempts | Pass | Fail | "
                    "Success | Min ms | Avg ms | P95 ms | Max ms |\n"
                 << "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";

        for (const auto& summary : summaries()) {
            markdown << "| " << markdownEscape(summary.testId) << " | "
                     << markdownEscape(summary.sensorId) << " | "
                     << markdownEscape(summary.action) << " | "
                     << summary.attempts << " | " << summary.passed << " | "
                     << summary.failed << " | " << std::fixed
                     << std::setprecision(2) << summary.successRate << "% | "
                     << optionalInteger(summary.minimumLatencyMs) << " | "
                     << optionalDecimal(summary.averageLatencyMs) << " | "
                     << optionalInteger(summary.p95LatencyMs) << " | "
                     << optionalInteger(summary.maximumLatencyMs) << " |\n";
        }

        markdown << "\n## Failed Trials\n\n";
        bool foundFailure = false;
        for (const auto& result : results_) {
            if (result.passed) continue;
            foundFailure = true;
            markdown << "- `" << markdownEscape(result.testId) << "` trial "
                     << result.trial;
            if (!result.sensorId.empty())
                markdown << " sensor `" << markdownEscape(result.sensorId)
                         << "`";
            if (!result.detail.empty())
                markdown << ": " << markdownEscape(result.detail);
            markdown << '\n';
        }
        if (!foundFailure) markdown << "- None\n";
        ensureWritable(markdown, paths.markdown);
    }
    return paths;
}

std::string utcTimestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string defaultRunId() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream output;
    output << "hil-" << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return output.str();
}

}  // namespace hardware_test

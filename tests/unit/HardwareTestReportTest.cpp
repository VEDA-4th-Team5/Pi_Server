#include "HardwareTestReport.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path outputDirectory = fs::temp_directory_path() /
        ("hardware-test-report-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        hardware_test::HardwareTestReport report({
            "portfolio-run", "2026-08-20T00:00:00Z", "abc123", "/dev/ttyACM0",
            115200, "uart-line", "hall", 5, "operator-confirmed-to-event",
            "fw-1.1", "stm32-lora-node-01"});

        const long long latencies[] = {10, 20, 30, 40, 100};
        for (int index = 0; index < 5; ++index) {
            report.record({"HIL-HALL-002", "HALL01", "OCCUPIED", index + 1,
                           index != 4, latencies[index], std::nullopt, {},
                           index == 4 ? "limit exceeded, \"slow\"" : ""});
        }
        report.record({"HIL-HALL-004", "HALL01", "CROSS_TALK", 1, true,
                       std::nullopt, 0.0, "events", "none"});

        const auto summaries = report.summaries();
        require(summaries.size() == 2, "summary grouping failed");
        const auto& latency = summaries.front();
        require(latency.attempts == 5 && latency.passed == 4 &&
                    latency.failed == 1,
                "pass/fail summary failed");
        require(latency.successRate == 80.0, "success rate failed");
        require(latency.minimumLatencyMs == 10 &&
                    latency.maximumLatencyMs == 100 &&
                    latency.p95LatencyMs == 100,
                "latency percentile failed");
        require(latency.averageLatencyMs == 40.0,
                "average latency failed");

        const auto paths = report.writeTo(outputDirectory);
        require(fs::is_regular_file(paths.csv), "CSV report missing");
        require(fs::is_regular_file(paths.markdown), "Markdown report missing");

        const std::string csv = readAll(paths.csv);
        require(csv.find("operator-confirmed-to-event") != std::string::npos,
                "latency method missing from CSV");
        require(csv.find("fw-1.1") != std::string::npos,
                "firmware version missing from CSV");
        require(csv.find("\"limit exceeded, \"\"slow\"\"\"") !=
                    std::string::npos,
                "CSV escaping failed");

        const std::string markdown = readAll(paths.markdown);
        require(markdown.find("80.00%") != std::string::npos,
                "Markdown success rate missing");
        require(markdown.find("HIL-HALL-002") != std::string::npos &&
                    markdown.find("P95 ms") != std::string::npos,
                "Markdown summary missing");

        fs::remove_all(outputDirectory);
        return 0;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(outputDirectory, ignored);
        throw;
    }
}

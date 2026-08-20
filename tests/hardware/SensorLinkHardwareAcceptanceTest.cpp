#include "device/SensorLinkManager.hpp"
#include "sensor/SensorProtocolParser.hpp"
#include "HardwareTestReport.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef VEDA_HARDWARE_TEST_GIT_COMMIT
#define VEDA_HARDWARE_TEST_GIT_COMMIT "unknown"
#endif

namespace {

using namespace std::chrono_literals;

constexpr int kSkipReturnCode = 77;

enum class Scenario {
    All,
    Hall,
    Fire,
    Observe
};

enum class ObservationKind {
    Hall,
    Fire
};

struct Options {
    std::string device;
    int baud{115200};
    device::SensorLinkMode mode{device::SensorLinkMode::UartLine};
    Scenario scenario{Scenario::All};
    std::vector<std::string> hallSensors{"HALL01", "HALL02"};
    std::string fireSensor{"FLAME01"};
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds eventTimeout{30000};
    std::chrono::milliseconds crossTalkWindow{2000};
    std::chrono::seconds observeDuration{30};
    int repeat{1};
    std::optional<std::filesystem::path> reportDirectory;
    std::string runId;
    std::string firmwareVersion{"unspecified"};
    std::string hardwareId{"unspecified"};
    std::optional<std::chrono::milliseconds> hallDetectLimit;
    std::optional<std::chrono::milliseconds> hallClearLimit;
    std::optional<std::chrono::milliseconds> fireDetectLimit;
    std::optional<std::chrono::milliseconds> fireClearLimit;
    bool allowRunningServer{false};
    bool allowUnparsed{false};
    bool checkCrossTalk{true};
    bool confirmActions{false};
    bool explicitDevice{false};
};

struct Observation {
    std::size_t id{};
    ObservationKind kind{ObservationKind::Hall};
    std::string sensorId;
    std::string state;
    std::string transport;
    std::optional<std::string> node;
    std::optional<std::uint64_t> sequence;
    std::optional<double> energy;
    std::chrono::steady_clock::time_point receivedAt;
    std::string raw;
};

std::string envValue(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string(value);
}

bool enabledValue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value == "1" || value == "true" || value == "yes" ||
           value == "on";
}

int parsePositiveInt(const std::string_view text, const char* name) {
    int value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value <= 0) {
        throw std::runtime_error(std::string(name) +
                                 " must be a positive integer");
    }
    return value;
}

std::vector<std::string> splitCommaSeparated(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
            result.push_back(item.substr(first, last - first + 1));
    }
    if (result.empty())
        throw std::runtime_error("hall sensor list must not be empty");
    return result;
}

Scenario parseScenario(const std::string& value) {
    if (value == "all") return Scenario::All;
    if (value == "hall") return Scenario::Hall;
    if (value == "fire") return Scenario::Fire;
    if (value == "observe") return Scenario::Observe;
    throw std::runtime_error(
        "scenario must be one of: all, hall, fire, observe");
}

std::string scenarioName(const Scenario scenario) {
    switch (scenario) {
        case Scenario::All: return "all";
        case Scenario::Hall: return "hall";
        case Scenario::Fire: return "fire";
        case Scenario::Observe: return "observe";
    }
    return "unknown";
}

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Semi-automatic STM32 sensor HIL acceptance test.\n\n"
        << "Options:\n"
        << "  --device PATH              UART device (for example /dev/ttyACM0)\n"
        << "  --baud RATE                UART baud rate (default: 115200)\n"
        << "  --mode uart-line|lora-frame\n"
        << "  --scenario all|hall|fire|observe\n"
        << "  --hall-sensors IDS         Comma-separated IDs (default: HALL01,HALL02)\n"
        << "  --fire-sensor ID           Fire sensor ID (default: FLAME01)\n"
        << "  --connect-timeout-ms N     Link connection timeout (default: 5000)\n"
        << "  --event-timeout-ms N       Timeout for each physical action (default: 30000)\n"
        << "  --cross-talk-window-ms N   Hall cross-talk observation (default: 2000)\n"
        << "  --observe-seconds N        Observe scenario duration (default: 30)\n"
        << "  --repeat N                 Physical trials per sensor (default: 1)\n"
        << "  --report-dir PATH          Write raw CSV and Markdown summary\n"
        << "  --run-id ID                Report run ID (default: UTC timestamp)\n"
        << "  --firmware-version TEXT    STM32 firmware identifier\n"
        << "  --hardware-id TEXT         Board/node identifier\n"
        << "  --hall-detect-limit-ms N   Optional Hall OCCUPIED acceptance limit\n"
        << "  --hall-clear-limit-ms N    Optional Hall VACANT acceptance limit\n"
        << "  --fire-detect-limit-ms N   Optional Flame DETECTED acceptance limit\n"
        << "  --fire-clear-limit-ms N    Optional Flame CLEARED acceptance limit\n"
        << "  --confirm-actions          Wait for Enter before timed physical actions\n"
        << "  --allow-running-server     Permit sharing risk with a running pi-server\n"
        << "  --allow-unparsed           Do not fail on unsupported UART lines\n"
        << "  --no-cross-talk            Disable Hall cross-talk assertion\n"
        << "  --help                     Show this help\n\n"
        << "Without --device, RUN_HARDWARE_TESTS=1 and SENSOR_UART_DEVICE are required.\n";
}

Options parseOptions(const int argc, char** argv, bool& showHelp) {
    Options options;
    const std::string envDevice = envValue("SENSOR_UART_DEVICE");
    if (!envDevice.empty()) options.device = envDevice;
    const std::string envBaud = envValue("SENSOR_UART_BAUD");
    if (!envBaud.empty()) options.baud = parsePositiveInt(envBaud, "SENSOR_UART_BAUD");
    const std::string envMode = envValue("SENSOR_LINK_MODE");
    if (!envMode.empty()) options.mode = device::SensorLinkManager::parseMode(envMode);

    auto requireValue = [&](int& index, const char* option) -> std::string {
        if (++index >= argc)
            throw std::runtime_error(std::string(option) + " requires a value");
        return argv[index];
    };

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            showHelp = true;
        } else if (argument == "--device") {
            options.device = requireValue(index, "--device");
            options.explicitDevice = true;
        } else if (argument == "--baud") {
            options.baud = parsePositiveInt(requireValue(index, "--baud"),
                                            "--baud");
        } else if (argument == "--mode") {
            const std::string mode = requireValue(index, "--mode");
            options.mode = device::SensorLinkManager::parseMode(mode);
            if (options.mode == device::SensorLinkMode::Disabled)
                throw std::runtime_error(
                    "--mode must be uart-line or lora-frame");
        } else if (argument == "--scenario") {
            options.scenario = parseScenario(requireValue(index, "--scenario"));
        } else if (argument == "--hall-sensors") {
            options.hallSensors = splitCommaSeparated(
                requireValue(index, "--hall-sensors"));
        } else if (argument == "--fire-sensor") {
            options.fireSensor = requireValue(index, "--fire-sensor");
        } else if (argument == "--connect-timeout-ms") {
            options.connectTimeout = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--connect-timeout-ms"),
                "--connect-timeout-ms"));
        } else if (argument == "--event-timeout-ms") {
            options.eventTimeout = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--event-timeout-ms"),
                "--event-timeout-ms"));
        } else if (argument == "--cross-talk-window-ms") {
            options.crossTalkWindow = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--cross-talk-window-ms"),
                "--cross-talk-window-ms"));
        } else if (argument == "--observe-seconds") {
            options.observeDuration = std::chrono::seconds(parsePositiveInt(
                requireValue(index, "--observe-seconds"),
                "--observe-seconds"));
        } else if (argument == "--repeat") {
            options.repeat = parsePositiveInt(requireValue(index, "--repeat"),
                                              "--repeat");
            if (options.repeat > 1000)
                throw std::runtime_error("--repeat must not exceed 1000");
        } else if (argument == "--report-dir") {
            options.reportDirectory = requireValue(index, "--report-dir");
        } else if (argument == "--run-id") {
            options.runId = requireValue(index, "--run-id");
            if (options.runId.empty())
                throw std::runtime_error("--run-id must not be empty");
        } else if (argument == "--firmware-version") {
            options.firmwareVersion = requireValue(index, "--firmware-version");
        } else if (argument == "--hardware-id") {
            options.hardwareId = requireValue(index, "--hardware-id");
        } else if (argument == "--hall-detect-limit-ms") {
            options.hallDetectLimit = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--hall-detect-limit-ms"),
                "--hall-detect-limit-ms"));
        } else if (argument == "--hall-clear-limit-ms") {
            options.hallClearLimit = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--hall-clear-limit-ms"),
                "--hall-clear-limit-ms"));
        } else if (argument == "--fire-detect-limit-ms") {
            options.fireDetectLimit = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--fire-detect-limit-ms"),
                "--fire-detect-limit-ms"));
        } else if (argument == "--fire-clear-limit-ms") {
            options.fireClearLimit = std::chrono::milliseconds(parsePositiveInt(
                requireValue(index, "--fire-clear-limit-ms"),
                "--fire-clear-limit-ms"));
        } else if (argument == "--confirm-actions") {
            options.confirmActions = true;
        } else if (argument == "--allow-running-server") {
            options.allowRunningServer = true;
        } else if (argument == "--allow-unparsed") {
            options.allowUnparsed = true;
        } else if (argument == "--no-cross-talk") {
            options.checkCrossTalk = false;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    return options;
}

bool piServerRunning() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error || !entry.is_directory()) continue;
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), ::isdigit))
            continue;
        std::ifstream command(entry.path() / "comm");
        std::string name;
        if (command && std::getline(command, name) && name == "pi-server")
            return true;
    }
    return false;
}

class ResultReporter {
public:
    void pass(const std::string& message) {
        ++passed_;
        std::cout << "[PASS] " << message << '\n';
    }

    void fail(const std::string& message) {
        ++failed_;
        std::cerr << "[FAIL] " << message << '\n';
    }

    int failed() const noexcept { return failed_; }
    int passed() const noexcept { return passed_; }

private:
    int passed_{};
    int failed_{};
};

class ObservationRecorder {
public:
    void accept(const std::string& line, const std::string& transport) {
        Observation observation;
        observation.receivedAt = std::chrono::steady_clock::now();
        observation.transport = transport;
        observation.raw = line;
        std::string error;

        if (sensor::SensorProtocolParser::isFireLine(line)) {
            const auto message = parser_.parseFire(
                line, std::chrono::system_clock::now(), &error);
            if (!message) {
                recordInvalid(line, error);
                return;
            }
            observation.kind = ObservationKind::Fire;
            observation.sensorId = message->sensorId;
            observation.state = sensor::toString(message->state);
            observation.node = message->node;
            observation.sequence = message->sequence;
            observation.energy = message->energy;
        } else {
            const auto message = parser_.parse(
                line, std::chrono::system_clock::now(), &error);
            if (!message) {
                recordInvalid(line, error);
                return;
            }
            observation.kind = ObservationKind::Hall;
            observation.sensorId = message->sensorId;
            observation.state =
                message->state == parking::ParkingSensorState::Occupied
                    ? "OCCUPIED" : "VACANT";
            observation.node = message->node;
            observation.sequence = message->sequence;
        }

        std::string sequenceNote;
        {
            std::lock_guard lock(mutex_);
            observation.id = nextId_++;
            if (observation.sequence) {
                const std::string key = observation.node
                    ? "node:" + *observation.node
                    : "sensor:" + observation.sensorId;
                const auto previous = lastSequence_.find(key);
                if (previous != lastSequence_.end()) {
                    if (*observation.sequence < previous->second) {
                        ++sequenceRegressions_;
                        sequenceNote = " SEQUENCE_REGRESSION";
                    } else if (*observation.sequence == previous->second) {
                        ++sequenceDuplicates_;
                        sequenceNote = " SEQUENCE_DUPLICATE";
                    }
                }
                if (previous == lastSequence_.end() ||
                    *observation.sequence > previous->second)
                    lastSequence_[key] = *observation.sequence;
            }
            if (observation.kind == ObservationKind::Hall)
                latestHallStates_[observation.sensorId] = observation.state;
            observations_.push_back(observation);
        }
        condition_.notify_all();

        std::cout << "[RX] transport=" << observation.transport
                  << " sensor=" << observation.sensorId
                  << " state=" << observation.state;
        if (observation.node) std::cout << " node=" << *observation.node;
        if (observation.sequence)
            std::cout << " sequence=" << *observation.sequence;
        if (observation.energy) std::cout << " energy=" << *observation.energy;
        std::cout << sequenceNote << '\n';
    }

    std::size_t cursor() const {
        std::lock_guard lock(mutex_);
        return nextId_;
    }

    std::optional<Observation> waitFor(
        const std::size_t after,
        const std::function<bool(const Observation&)>& predicate,
        const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto findMatching = [&]() -> std::optional<Observation> {
            const auto found = std::find_if(
                observations_.begin(), observations_.end(),
                [&](const Observation& value) {
                    return value.id >= after && predicate(value);
                });
            return found == observations_.end()
                ? std::nullopt : std::optional<Observation>(*found);
        };
        std::optional<Observation> result = findMatching();
        if (result) return result;
        if (!condition_.wait_for(lock, timeout, [&] {
                result = findMatching();
                return result.has_value();
            }))
            return std::nullopt;
        return result;
    }

    bool otherHallOccupied(const std::size_t after, const std::size_t through,
                           const std::string& expectedSensor) const {
        std::lock_guard lock(mutex_);
        return std::any_of(observations_.begin(), observations_.end(),
                           [&](const Observation& value) {
            return value.id >= after && value.id <= through &&
                   value.kind == ObservationKind::Hall &&
                   value.sensorId != expectedSensor &&
                   value.state == "OCCUPIED";
        });
    }

    bool waitForAllHallStates(const std::vector<std::string>& sensorIds,
                              const std::string& state,
                              const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto allMatch = [&] {
            return std::all_of(sensorIds.begin(), sensorIds.end(),
                               [&](const std::string& sensorId) {
                const auto found = latestHallStates_.find(sensorId);
                return found != latestHallStates_.end() &&
                       found->second == state;
            });
        };
        return allMatch() || condition_.wait_for(lock, timeout, allMatch);
    }

    std::size_t parsedCount() const {
        std::lock_guard lock(mutex_);
        return observations_.size();
    }

    std::size_t invalidCount() const {
        std::lock_guard lock(mutex_);
        return invalidCount_;
    }

    std::size_t sequenceRegressions() const {
        std::lock_guard lock(mutex_);
        return sequenceRegressions_;
    }

    std::size_t sequenceDuplicates() const {
        std::lock_guard lock(mutex_);
        return sequenceDuplicates_;
    }

private:
    void recordInvalid(const std::string& line, const std::string& error) {
        {
            std::lock_guard lock(mutex_);
            ++invalidCount_;
        }
        condition_.notify_all();
        std::cerr << "[WARN] unparsed sensor line: " << error
                  << " raw=" << line << '\n';
    }

    sensor::SensorProtocolParser parser_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Observation> observations_;
    std::map<std::string, std::uint64_t> lastSequence_;
    std::map<std::string, std::string> latestHallStates_;
    std::size_t nextId_{};
    std::size_t invalidCount_{};
    std::size_t sequenceRegressions_{};
    std::size_t sequenceDuplicates_{};
};

std::optional<Observation> waitForHall(
    ObservationRecorder& recorder, const std::size_t cursor,
    const std::string& sensorId, const std::string& state,
    const std::chrono::milliseconds timeout) {
    return recorder.waitFor(cursor, [&](const Observation& value) {
        return value.kind == ObservationKind::Hall &&
               value.sensorId == sensorId && value.state == state;
    }, timeout);
}

std::optional<Observation> waitForFire(
    ObservationRecorder& recorder, const std::size_t cursor,
    const std::string& sensorId, const std::string& state,
    const std::chrono::milliseconds timeout) {
    return recorder.waitFor(cursor, [&](const Observation& value) {
        return value.kind == ObservationKind::Fire &&
               value.sensorId == sensorId && value.state == state;
    }, timeout);
}

long long elapsedMillis(const std::chrono::steady_clock::time_point start,
                        const std::chrono::steady_clock::time_point finish) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
        .count();
}

void preparePhysicalAction(const Options& options,
                           const std::string& instruction) {
    std::cout << "[ACTION] " << instruction << '\n';
    if (!options.confirmActions) return;
    std::cout << "[READY] Press Enter immediately before performing the action."
              << std::flush;
    std::string confirmation;
    if (!std::getline(std::cin, confirmation))
        throw std::runtime_error(
            "stdin closed while waiting for physical action confirmation");
    std::cout << '\n';
}

bool withinLimit(
    const long long latency,
    const std::optional<std::chrono::milliseconds>& limit) {
    return !limit || latency <= limit->count();
}

void recordCase(ResultReporter& result,
                hardware_test::HardwareTestReport& report,
                hardware_test::CaseResult caseResult,
                const std::string& message) {
    if (caseResult.passed)
        result.pass(message);
    else
        result.fail(message);
    report.record(std::move(caseResult));
}

bool runHallScenario(const Options& options, ObservationRecorder& recorder,
                     ResultReporter& result,
                     hardware_test::HardwareTestReport& report,
                     const std::string& sensorId, const int trial) {
    std::cout << "\n[TRIAL] Hall " << sensorId << ' ' << trial << '/'
              << options.repeat << '\n';
    preparePhysicalAction(
        options, "Remove every target from all configured Hall sensors and "
                 "establish a VACANT baseline.");
    if (!recorder.waitForAllHallStates(options.hallSensors, "VACANT",
                                       options.eventTimeout)) {
        recordCase(result, report,
                   {"HIL-HALL-001", sensorId, "BASELINE_VACANT", trial, false,
                    std::nullopt, std::nullopt, {},
                    "not every configured Hall sensor reported VACANT"},
                   sensorId +
                       " baseline failed: not every Hall sensor reported VACANT");
        return false;
    }
    recordCase(result, report,
               {"HIL-HALL-001", sensorId, "BASELINE_VACANT", trial, true,
                std::nullopt, std::nullopt, {}, "all Hall sensors VACANT"},
               sensorId + " baseline VACANT received for all Hall sensors");

    preparePhysicalAction(options,
                          "Place the vehicle/magnet over " + sensorId + ".");
    auto cursor = recorder.cursor();
    const auto started = std::chrono::steady_clock::now();
    const auto occupied = waitForHall(recorder, cursor, sensorId, "OCCUPIED",
                                      options.eventTimeout);
    if (!occupied) {
        recordCase(result, report,
                   {"HIL-HALL-002", sensorId, "OCCUPIED", trial, false,
                    std::nullopt, std::nullopt, {}, "event timeout"},
                   sensorId + " did not report OCCUPIED before timeout");
        return false;
    }
    const long long occupiedLatency =
        elapsedMillis(started, occupied->receivedAt);
    const bool occupiedAccepted =
        withinLimit(occupiedLatency, options.hallDetectLimit);
    const std::string occupiedDetail = occupiedAccepted
        ? "state transition received"
        : "latency limit exceeded";
    recordCase(result, report,
               {"HIL-HALL-002", sensorId, "OCCUPIED", trial,
                occupiedAccepted, occupiedLatency, std::nullopt, {},
                occupiedDetail},
               sensorId + " OCCUPIED latency=" +
                   std::to_string(occupiedLatency) + "ms" +
                   (occupiedAccepted ? "" : " exceeds acceptance limit"));
    if (options.checkCrossTalk)
        std::this_thread::sleep_for(options.crossTalkWindow);
    const std::size_t crossTalkThrough = recorder.cursor();
    if (options.checkCrossTalk) {
        const bool crossTalk = crossTalkThrough != 0 &&
            recorder.otherHallOccupied(cursor, crossTalkThrough - 1, sensorId);
        recordCase(result, report,
                   {"HIL-HALL-004", sensorId, "CROSS_TALK", trial, !crossTalk,
                    std::nullopt, crossTalk ? 1.0 : 0.0, "events",
                    crossTalk ? "another Hall sensor reported OCCUPIED"
                              : "no cross-talk observed"},
                   crossTalk
                       ? sensorId +
                           " stimulus also produced OCCUPIED on another Hall sensor"
                       : sensorId + " cross-talk not observed");
    }

    preparePhysicalAction(options,
                          "Remove the vehicle/magnet from " + sensorId + ".");
    cursor = recorder.cursor();
    const auto removed = std::chrono::steady_clock::now();
    const auto vacant = waitForHall(recorder, cursor, sensorId, "VACANT",
                                    options.eventTimeout);
    if (!vacant) {
        recordCase(result, report,
                   {"HIL-HALL-003", sensorId, "VACANT", trial, false,
                    std::nullopt, std::nullopt, {}, "event timeout"},
                   sensorId + " did not return to VACANT before timeout");
        return false;
    }
    const long long vacantLatency = elapsedMillis(removed, vacant->receivedAt);
    const bool vacantAccepted = withinLimit(vacantLatency,
                                            options.hallClearLimit);
    recordCase(result, report,
               {"HIL-HALL-003", sensorId, "VACANT", trial, vacantAccepted,
                vacantLatency, std::nullopt, {},
                vacantAccepted ? "state transition received"
                               : "latency limit exceeded"},
               sensorId + " VACANT latency=" +
                   std::to_string(vacantLatency) + "ms" +
                   (vacantAccepted ? "" : " exceeds acceptance limit"));
    return true;
}

bool runFireScenario(const Options& options, ObservationRecorder& recorder,
                     ResultReporter& result,
                     hardware_test::HardwareTestReport& report,
                     const int trial) {
    std::cout << "\n[TRIAL] Flame " << options.fireSensor << ' ' << trial << '/'
              << options.repeat << '\n';
    preparePhysicalAction(options, "Remove the flame source from " +
        options.fireSensor + " and establish a CLEARED baseline.");
    auto cursor = recorder.cursor();
    if (!waitForFire(recorder, cursor, options.fireSensor, "CLEARED",
                     options.eventTimeout)) {
        recordCase(result, report,
                   {"HIL-FIRE-001", options.fireSensor, "BASELINE_CLEARED",
                    trial, false, std::nullopt, std::nullopt, {},
                    "event timeout"},
                   options.fireSensor +
                       " did not report baseline CLEARED before timeout");
        return false;
    }
    recordCase(result, report,
               {"HIL-FIRE-001", options.fireSensor, "BASELINE_CLEARED", trial,
                true, std::nullopt, std::nullopt, {},
                "baseline state received"},
               options.fireSensor + " baseline CLEARED received");

    preparePhysicalAction(options, "Expose " + options.fireSensor +
        " to the controlled flame source.");
    cursor = recorder.cursor();
    const auto exposed = std::chrono::steady_clock::now();
    const auto detected = waitForFire(recorder, cursor, options.fireSensor,
                                      "DETECTED", options.eventTimeout);
    if (!detected) {
        recordCase(result, report,
                   {"HIL-FIRE-002", options.fireSensor, "DETECTED", trial,
                    false, std::nullopt, std::nullopt, {}, "event timeout"},
                   options.fireSensor +
                       " did not report DETECTED before timeout");
        return false;
    }
    const long long detectedLatency =
        elapsedMillis(exposed, detected->receivedAt);
    const bool detectedAccepted =
        withinLimit(detectedLatency, options.fireDetectLimit);
    std::string detectedMessage = options.fireSensor + " DETECTED latency=" +
        std::to_string(detectedLatency) + "ms";
    if (detected->energy)
        detectedMessage += " energy=" + std::to_string(*detected->energy);
    if (!detectedAccepted) detectedMessage += " exceeds acceptance limit";
    recordCase(result, report,
               {"HIL-FIRE-002", options.fireSensor, "DETECTED", trial,
                detectedAccepted, detectedLatency, detected->energy,
                detected->energy ? "fft_energy" : "",
                detectedAccepted ? "state transition received"
                                 : "latency limit exceeded"},
               detectedMessage);

    preparePhysicalAction(
        options, "Remove the flame source. The recent FFT window may delay "
                 "CLEARED.");
    cursor = recorder.cursor();
    const auto removed = std::chrono::steady_clock::now();
    const auto cleared = waitForFire(recorder, cursor, options.fireSensor,
                                     "CLEARED", options.eventTimeout);
    if (!cleared) {
        recordCase(result, report,
                   {"HIL-FIRE-003", options.fireSensor, "CLEARED", trial,
                    false, std::nullopt, std::nullopt, {}, "event timeout"},
                   options.fireSensor +
                       " did not report CLEARED before timeout");
        return false;
    }
    const long long clearLatency = elapsedMillis(removed, cleared->receivedAt);
    const bool clearAccepted = withinLimit(clearLatency,
                                           options.fireClearLimit);
    recordCase(result, report,
               {"HIL-FIRE-003", options.fireSensor, "CLEARED", trial,
                clearAccepted, clearLatency, cleared->energy,
                cleared->energy ? "fft_energy" : "",
                clearAccepted ? "state transition received"
                              : "latency limit exceeded"},
               options.fireSensor + " CLEARED latency=" +
                   std::to_string(clearLatency) + "ms" +
                   (clearAccepted ? "" : " exceeds acceptance limit"));
    return true;
}

bool writeRequestedReport(const Options& options,
                          const hardware_test::HardwareTestReport& report,
                          ResultReporter& result) {
    if (!options.reportDirectory) return true;
    try {
        const auto paths = report.writeTo(*options.reportDirectory);
        std::cout << "[REPORT] CSV=" << paths.csv << '\n'
                  << "[REPORT] Markdown=" << paths.markdown << '\n';
        return true;
    } catch (const std::exception& exception) {
        result.fail(std::string("hardware report write failed: ") +
                    exception.what());
        return false;
    }
}

int run(const Options& options) {
    if (!options.explicitDevice &&
        !enabledValue(envValue("RUN_HARDWARE_TESTS"))) {
        std::cout << "[SKIP] Set RUN_HARDWARE_TESTS=1 or pass --device to run "
                     "the physical sensor test.\n";
        return kSkipReturnCode;
    }
    if (options.device.empty()) {
        std::cerr << "[SKIP] SENSOR_UART_DEVICE/--device is not configured.\n";
        return kSkipReturnCode;
    }
    if (!std::filesystem::exists(options.device)) {
        std::cerr << "[SKIP] UART device does not exist: " << options.device << '\n';
        return kSkipReturnCode;
    }
    if (!options.allowRunningServer && piServerRunning()) {
        std::cerr << "[SKIP] pi-server is running and may own the same UART. "
                     "Stop it first or explicitly pass --allow-running-server.\n";
        return kSkipReturnCode;
    }

    const std::string runId = options.runId.empty()
        ? hardware_test::defaultRunId() : options.runId;
    hardware_test::HardwareTestReport report({
        runId,
        hardware_test::utcTimestamp(),
        VEDA_HARDWARE_TEST_GIT_COMMIT,
        options.device,
        options.baud,
        device::SensorLinkManager::modeName(options.mode),
        scenarioName(options.scenario),
        options.repeat,
        options.confirmActions ? "operator-confirmed-to-event"
                               : "prompt-to-event (includes operator response)",
        options.firmwareVersion,
        options.hardwareId});
    ResultReporter result;

    ObservationRecorder recorder;
    device::SensorLinkManager::Config linkConfig;
    linkConfig.mode = options.mode;
    linkConfig.uart = {options.device, options.baud, 100};
    linkConfig.reconnect_delay_ms = 250;
    device::SensorLinkManager link(
        linkConfig, [&](const std::string& line, const std::string& transport) {
            recorder.accept(line, transport);
        });
    if (!link.start()) {
        recordCase(result, report,
                   {"HIL-LINK-001", {}, "CONNECT", 1, false, std::nullopt,
                    std::nullopt, {}, "SensorLinkManager refused to start"},
                   "SensorLinkManager refused to start");
        (void)writeRequestedReport(options, report, result);
        return 1;
    }

    const auto connectDeadline = std::chrono::steady_clock::now() +
                                 options.connectTimeout;
    while (!link.connected() &&
           std::chrono::steady_clock::now() < connectDeadline)
        std::this_thread::sleep_for(20ms);
    if (!link.connected()) {
        link.stop();
        recordCase(result, report,
                   {"HIL-LINK-001", {}, "CONNECT", 1, false, std::nullopt,
                    std::nullopt, {}, "UART connection timeout"},
                   "UART did not connect before timeout: " + options.device);
        (void)writeRequestedReport(options, report, result);
        return 1;
    }

    recordCase(result, report,
               {"HIL-LINK-001", {}, "CONNECT", 1, true, std::nullopt,
                std::nullopt, {}, "sensor link connected"},
               "sensor link connected mode=" +
                   device::SensorLinkManager::modeName(options.mode) +
                   " device=" + options.device +
                   " baud=" + std::to_string(options.baud));

    if (options.scenario == Scenario::Observe) {
        std::cout << "[ACTION] Observing physical sensor traffic for "
                  << options.observeDuration.count() << " seconds.\n";
        std::this_thread::sleep_for(options.observeDuration);
    } else {
        for (int trial = 1; trial <= options.repeat; ++trial) {
            if (options.scenario == Scenario::All ||
                options.scenario == Scenario::Hall) {
                for (const auto& sensorId : options.hallSensors) {
                    (void)runHallScenario(options, recorder, result, report,
                                          sensorId, trial);
                }
            }
            if (options.scenario == Scenario::All ||
                options.scenario == Scenario::Fire) {
                (void)runFireScenario(options, recorder, result, report, trial);
            }
        }
    }

    link.stop();

    const bool parsedMessages = recorder.parsedCount() != 0;
    recordCase(result, report,
               {"HIL-PROTO-001", {}, "VALID_MESSAGES", 1, parsedMessages,
                std::nullopt, static_cast<double>(recorder.parsedCount()),
                "messages", parsedMessages ? "valid messages received"
                                           : "no valid messages received"},
               parsedMessages
                   ? "valid messages received=" +
                       std::to_string(recorder.parsedCount())
                   : "no valid SENSOR/FIRE message was received");

    const bool sequenceAccepted = recorder.sequenceRegressions() == 0;
    recordCase(result, report,
               {"HIL-PROTO-002", {}, "SEQUENCE", 1, sequenceAccepted,
                std::nullopt,
                static_cast<double>(recorder.sequenceRegressions()),
                "regressions",
                sequenceAccepted ? "no sequence regression observed"
                                 : "sequence regression observed"},
               sequenceAccepted
                   ? "no sequence regression observed"
                   : "sequence regressions=" +
                       std::to_string(recorder.sequenceRegressions()));

    if (recorder.sequenceDuplicates() != 0)
        std::cout << "[WARN] duplicate sequences observed="
                  << recorder.sequenceDuplicates() << '\n';

    const bool rawAccepted = options.allowUnparsed || recorder.invalidCount() == 0;
    if (recorder.invalidCount() != 0 && options.allowUnparsed)
        std::cout << "[WARN] allowed unparsed lines=" << recorder.invalidCount()
                  << '\n';
    recordCase(result, report,
               {"HIL-PROTO-003", {}, "RAW_PROTOCOL", 1, rawAccepted,
                std::nullopt, static_cast<double>(recorder.invalidCount()),
                "lines",
                recorder.invalidCount() == 0 ? "all input parsed"
                                             : "unparsed input observed"},
               rawAccepted
                   ? "unparsed sensor lines accepted=" +
                       std::to_string(recorder.invalidCount())
                   : "unparsed sensor lines=" +
                       std::to_string(recorder.invalidCount()));

    (void)writeRequestedReport(options, report, result);

    std::cout << "\nRESULT: " << result.passed() << " passed, "
              << result.failed() << " failed\n";
    return result.failed() == 0 ? 0 : 1;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        bool showHelp = false;
        const Options options = parseOptions(argc, argv, showHelp);
        if (showHelp) {
            printUsage(argv[0]);
            return 0;
        }
        return run(options);
    } catch (const std::exception& exception) {
        std::cerr << "SensorLinkHardwareAcceptanceTest: " << exception.what()
                  << '\n';
        return 2;
    }
}

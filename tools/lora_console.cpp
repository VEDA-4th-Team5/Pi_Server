#include "device/LoRaDriver.hpp"
#include "device/UartDriver.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct Options {
    std::string device{"/dev/serial0"};
    int baud{115200};
    std::uint16_t defaultAddress{0x0001};
    std::uint8_t channel{0x1E};
};

struct StreamStats {
    std::optional<std::uint32_t> firstSequence;
    std::optional<std::uint32_t> lastSequence;
    std::uint64_t frames{};
    std::uint64_t lost{};
    std::uint64_t resets{};
};

struct ConsoleStats {
    std::chrono::steady_clock::time_point startedAt{
        std::chrono::steady_clock::now()};
    std::uint64_t receivedBytes{};
    std::uint64_t receivedFrames{};
    std::uint64_t rejectedFrames{};
    std::uint64_t transmittedBytes{};
    std::uint64_t transmittedFrames{};
    std::map<std::string, StreamStats> streams;
    std::map<std::string, std::string> hallStates;
    std::map<std::string, std::string> flameStates;
    mutable std::mutex mutex;
};

std::atomic_bool running{true};
std::mutex consoleMutex;

void stopHandler(int) { running.store(false); }

template <typename Integer>
Integer parseInteger(const std::string_view text, const std::string& name) {
    int base = 10;
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' &&
        (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    if (digits.empty()) {
        throw std::invalid_argument(name + " must be an integer");
    }
    Integer value{};
    const auto result = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, base);
    if (result.ec != std::errc{} ||
        result.ptr != digits.data() + digits.size()) {
        throw std::invalid_argument(name + " must be an integer");
    }
    return value;
}

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program
        << " [--device PATH] [--baud RATE] [--address VALUE] [--channel VALUE]\n"
        << "\n"
        << "Defaults: --device /dev/serial0 --baud 115200 --address 1"
           " --channel 30\n";
}

Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--device") {
            options.device = value;
        } else if (argument == "--baud") {
            options.baud = parseInteger<int>(value, "baud");
        } else if (argument == "--address") {
            options.defaultAddress =
                parseInteger<std::uint16_t>(value, "address");
        } else if (argument == "--channel") {
            const auto channel = parseInteger<unsigned int>(value, "channel");
            if (channel > 255U) {
                throw std::invalid_argument("channel must be between 0 and 255");
            }
            options.channel = static_cast<std::uint8_t>(channel);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.device.empty()) {
        throw std::invalid_argument("device path is empty");
    }
    if (options.baud <= 0) {
        throw std::invalid_argument("baud must be positive");
    }
    return options;
}

std::string payloadText(const device::LoRaFrame& frame) {
    return {frame.payload.begin(), frame.payload.end()};
}

std::string frameTypeName(const device::LoRaMessageType type) {
    switch (type) {
    case device::LoRaMessageType::SensorEvent:
        return "SENSOR";
    case device::LoRaMessageType::AlertCommand:
        return "ALERT";
    case device::LoRaMessageType::Heartbeat:
        return "HEARTBEAT";
    }
    return "UNKNOWN";
}

std::string streamKey(const std::optional<std::string>& node) {
    return node.value_or("legacy");
}

void noteSequence(StreamStats& stats, const std::uint32_t sequence) {
    ++stats.frames;
    if (!stats.firstSequence) stats.firstSequence = sequence;
    if (stats.lastSequence) {
        if (sequence > *stats.lastSequence) {
            stats.lost += sequence - *stats.lastSequence - 1U;
        } else if (*stats.lastSequence > sequence &&
                   *stats.lastSequence - sequence > 1000U) {
            ++stats.resets;
            stats.firstSequence = sequence;
        }
    }
    stats.lastSequence = sequence;
}

std::string describeSensorFrame(const device::LoRaFrame& frame,
                                ConsoleStats& stats) {
    const std::string payload = payloadText(frame);
    sensor::SensorProtocolParser parser;
    std::string error;
    const auto now = std::chrono::system_clock::now();

    if (sensor::SensorProtocolParser::isFireLine(payload)) {
        const auto message = parser.parseFire(payload, now, &error);
        if (!message) return payload + " [parse_error=" + error + "]";
        const std::string node = streamKey(message->node);
        const std::string state = sensor::toString(message->state);
        {
            std::lock_guard lock(stats.mutex);
            stats.flameStates[node + "/" + message->sensorId] = state;
            noteSequence(stats.streams[node], frame.sequence);
        }
        std::ostringstream output;
        output << node << ' ' << message->sensorId << ' ' << state;
        if (message->sequence) output << " payload_seq=" << *message->sequence;
        if (message->energy) output << " energy=" << *message->energy;
        return output.str();
    }

    const auto message = parser.parse(payload, now, &error);
    if (!message) return payload + " [parse_error=" + error + "]";
    const std::string node = streamKey(message->node);
    const std::string state =
        message->state == parking::ParkingSensorState::Occupied
            ? "OCCUPIED"
            : "VACANT";
    {
        std::lock_guard lock(stats.mutex);
        stats.hallStates[node + "/" + message->sensorId] = state;
        noteSequence(stats.streams[node], frame.sequence);
    }
    std::ostringstream output;
    output << node << ' ' << message->sensorId << ' ' << state;
    if (message->sequence) output << " payload_seq=" << *message->sequence;
    return output.str();
}

std::string describeFrame(const device::LoRaFrame& frame,
                          ConsoleStats& stats) {
    if (frame.type == device::LoRaMessageType::SensorEvent) {
        return describeSensorFrame(frame, stats);
    }
    {
        std::lock_guard lock(stats.mutex);
        noteSequence(stats.streams["unknown"], frame.sequence);
    }
    return payloadText(frame);
}

void printReceivedFrame(const device::LoRaFrame& frame,
                        ConsoleStats& stats) {
    const std::string description = describeFrame(frame, stats);
    std::lock_guard lock(consoleMutex);
    std::cout << "\n[RX] version=" << static_cast<unsigned int>(frame.version)
              << " type=" << frameTypeName(frame.type)
              << " frame_seq=" << frame.sequence << ' ' << description
              << "\nlora> " << std::flush;
}

void readerLoop(device::UartDriver& uart, device::LoRaDriver& lora,
                ConsoleStats& stats) {
    std::array<std::uint8_t, 512> buffer{};
    while (running.load()) {
        std::string error;
        const int count = uart.readSome(buffer, &error);
        if (count < 0) {
            std::lock_guard lock(consoleMutex);
            std::cerr << "\n[ERROR] " << error << '\n';
            running.store(false);
            return;
        }
        if (count == 0) continue;

        const auto rejectedBefore = lora.rejectedFrames();
        const auto frames = lora.consume(std::span<const std::uint8_t>(
            buffer.data(), static_cast<std::size_t>(count)));
        const auto rejectedAfter = lora.rejectedFrames();
        {
            std::lock_guard lock(stats.mutex);
            stats.receivedBytes += static_cast<std::uint64_t>(count);
            stats.receivedFrames += frames.size();
            stats.rejectedFrames += rejectedAfter - rejectedBefore;
        }
        for (const auto& frame : frames) printReceivedFrame(frame, stats);
    }
}

device::LoRaDestination destinationForSensor(const std::string& sensorId,
                                             const Options& options) {
    std::uint16_t address = options.defaultAddress;
    if (sensorId == "HALL01" || sensorId == "HALL02") address = 0x0001;
    if (sensorId == "HALL03" || sensorId == "HALL04") address = 0x0002;
    return {
        static_cast<std::uint8_t>((address >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(address & 0xFFU),
        options.channel};
}

bool sendPayload(device::LoRaDriver& lora,
                 const device::LoRaDestination destination,
                 const std::uint32_t sequence, const std::string& payload,
                 ConsoleStats& stats) {
    device::LoRaFrame frame;
    frame.type = device::LoRaMessageType::AlertCommand;
    frame.sequence = sequence;
    frame.payload.assign(payload.begin(), payload.end());

    std::string error;
    if (!lora.sendTo(destination, frame, &error)) {
        std::lock_guard lock(consoleMutex);
        std::cerr << "[ERROR] transmit failed: " << error << '\n';
        return false;
    }
    const auto encodedBytes = device::LoRaDriver::encode(frame).size() + 3U;
    {
        std::lock_guard lock(stats.mutex);
        ++stats.transmittedFrames;
        stats.transmittedBytes += encodedBytes;
    }
    std::lock_guard lock(consoleMutex);
    std::cout << "[TX] address=0x" << std::hex << std::setw(4)
              << std::setfill('0')
              << ((static_cast<unsigned int>(destination.addressHigh) << 8U) |
                  destination.addressLow)
              << std::dec << std::setfill(' ')
              << " channel=" << static_cast<unsigned int>(destination.channel)
              << " frame_seq=" << sequence << ' ' << payload << '\n';
    return true;
}

void printReport(const ConsoleStats& stats) {
    std::lock_guard statsLock(stats.mutex);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stats.startedAt).count();
    std::lock_guard consoleLock(consoleMutex);
    std::cout << "\n=== LoRa session report ===\n"
              << "duration_seconds=" << std::fixed << std::setprecision(1)
              << seconds << '\n'
              << "rx_bytes=" << stats.receivedBytes
              << " rx_frames=" << stats.receivedFrames
              << " rejected_frames=" << stats.rejectedFrames << '\n'
              << "tx_bytes=" << stats.transmittedBytes
              << " tx_frames=" << stats.transmittedFrames << '\n';
    for (const auto& [node, stream] : stats.streams) {
        std::cout << "stream=" << node << " frames=" << stream.frames
                  << " first_seq="
                  << (stream.firstSequence
                          ? std::to_string(*stream.firstSequence)
                          : "-")
                  << " last_seq="
                  << (stream.lastSequence
                          ? std::to_string(*stream.lastSequence)
                          : "-")
                  << " lost=" << stream.lost
                  << " resets=" << stream.resets << '\n';
    }
    for (const auto& [sensor, state] : stats.hallStates) {
        std::cout << "hall=" << sensor << " state=" << state << '\n';
    }
    for (const auto& [sensor, state] : stats.flameStates) {
        std::cout << "flame=" << sensor << " state=" << state << '\n';
    }
}

void printHelp() {
    std::lock_guard lock(consoleMutex);
    std::cout
        << "Commands:\n"
        << "  1 | 2 | 3 | 4   Toggle HALL01..HALL04 LED\n"
        << "  on N / off N    Set the selected Hall LED\n"
        << "  raw TEXT        Send an AlertCommand payload\n"
        << "  s               Print session statistics\n"
        << "  h               Print this help\n"
        << "  q               Quit\n";
}

std::pair<std::string, std::string> splitCommand(const std::string& line) {
    const auto separator = line.find_first_of(" \t");
    if (separator == std::string::npos) return {line, {}};
    const auto argument = line.find_first_not_of(" \t", separator);
    return {line.substr(0, separator),
            argument == std::string::npos ? std::string{} : line.substr(argument)};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        device::UartDriver uart({options.device, options.baud, 100});
        std::string error;
        if (!uart.connect(&error)) throw std::runtime_error(error);
        device::LoRaDriver lora(uart);
        ConsoleStats stats;

        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        std::thread reader(readerLoop, std::ref(uart), std::ref(lora),
                           std::ref(stats));

        std::cout << "LoRa console " << options.device << " @ " << options.baud
                  << " default_address=0x" << std::hex << std::setw(4)
                  << std::setfill('0') << options.defaultAddress << std::dec
                  << std::setfill(' ')
                  << " channel=" << static_cast<unsigned int>(options.channel)
                  << '\n';
        printHelp();

        std::map<int, bool> ledStates{{1, false}, {2, false}, {3, false},
                                      {4, false}};
        std::uint32_t transmitSequence{};
        std::string line;
        while (running.load()) {
            {
                std::lock_guard lock(consoleMutex);
                std::cout << "lora> " << std::flush;
            }
            if (!std::getline(std::cin, line)) break;
            const auto [command, argument] = splitCommand(line);
            if (command.empty()) continue;
            if (command == "q" || command == "quit" || command == "exit") {
                break;
            }
            if (command == "h" || command == "help") {
                printHelp();
                continue;
            }
            if (command == "s") {
                printReport(stats);
                continue;
            }

            int hallNumber = 0;
            bool state = false;
            bool alertCommand = false;
            if (command.size() == 1 && command[0] >= '1' && command[0] <= '4') {
                hallNumber = command[0] - '0';
                state = !ledStates[hallNumber];
                alertCommand = true;
            } else if ((command == "on" || command == "off") &&
                       !argument.empty()) {
                hallNumber = parseInteger<int>(argument, "Hall number");
                if (hallNumber < 1 || hallNumber > 4) {
                    throw std::invalid_argument("Hall number must be 1..4");
                }
                state = command == "on";
                alertCommand = true;
            }

            if (alertCommand) {
                ledStates[hallNumber] = state;
                const std::string sensor =
                    "HALL0" + std::to_string(hallNumber);
                const std::uint32_t sequence = ++transmitSequence;
                const std::string payload =
                    "ALERT:" + sensor + ":LED:" +
                    (state ? "ON:" : "OFF:") + std::to_string(sequence);
                sendPayload(lora, destinationForSensor(sensor, options),
                            sequence, payload, stats);
                continue;
            }
            if (command == "raw" && !argument.empty()) {
                const std::uint32_t sequence = ++transmitSequence;
                sendPayload(lora, destinationForSensor({}, options), sequence,
                            argument, stats);
                continue;
            }

            std::lock_guard lock(consoleMutex);
            std::cout << "Unknown command. Enter h for help.\n";
        }

        running.store(false);
        if (reader.joinable()) reader.join();
        printReport(stats);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "lora-console: " << exception.what() << '\n';
        return 1;
    }
}

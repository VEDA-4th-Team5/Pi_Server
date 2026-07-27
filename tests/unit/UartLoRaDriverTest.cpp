#include "device/LoRaDriver.hpp"
#include "device/SensorLinkManager.hpp"
#include "device/UartDriver.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class PseudoTerminal {
public:
    PseudoTerminal() {
        int slave = -1;
        std::array<char, 256> name{};
        if (openpty(&master_, &slave, name.data(), nullptr, nullptr) != 0)
            throw std::runtime_error(std::string("openpty failed: ") +
                                     std::strerror(errno));
        path_ = name.data();
        ::close(slave);
    }
    ~PseudoTerminal() {
        if (master_ >= 0) ::close(master_);
    }
    PseudoTerminal(const PseudoTerminal&) = delete;
    PseudoTerminal& operator=(const PseudoTerminal&) = delete;

    const std::string& path() const { return path_; }

    void writeBytes(const std::vector<std::uint8_t>& bytes) const {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::write(master_, bytes.data() + offset,
                                          bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("PTY write failed");
            offset += static_cast<std::size_t>(count);
        }
    }

    void writeText(const std::string& value) const {
        writeBytes(std::vector<std::uint8_t>(value.begin(), value.end()));
    }

    std::vector<std::uint8_t> readBytes(const std::size_t expected) const {
        std::vector<std::uint8_t> result;
        result.reserve(expected);
        while (result.size() < expected) {
            pollfd descriptor{master_, POLLIN, 0};
            require(::poll(&descriptor, 1, 1000) > 0, "PTY read timeout");
            std::array<std::uint8_t, 256> buffer{};
            const ssize_t count = ::read(master_, buffer.data(), buffer.size());
            require(count > 0, "PTY read failed");
            result.insert(result.end(), buffer.begin(),
                          buffer.begin() + count);
        }
        return result;
    }

private:
    int master_{-1};
    std::string path_;
};

device::UartDriver::Config uartConfig(const std::string& path) {
    return {path, 115200, 100};
}

void testUartTransport() {
    PseudoTerminal terminal;
    device::UartDriver uart(uartConfig(terminal.path()));
    std::string error;
    require(uart.connect(&error), "UART connect failed: " + error);

    terminal.writeText("abc");
    std::array<std::uint8_t, 16> input{};
    const int count = uart.readSome(input, &error);
    require(count == 3, "UART read byte count mismatch");
    require(std::string(input.begin(), input.begin() + count) == "abc",
            "UART read content mismatch");

    const std::string output = "ALERT:EV01:ON\n";
    require(uart.writeAll(std::span<const std::uint8_t>(
                              reinterpret_cast<const std::uint8_t*>(output.data()),
                              output.size()),
                          &error),
            "UART write failed: " + error);
    const auto received = terminal.readBytes(output.size());
    require(std::string(received.begin(), received.end()) == output,
            "UART write content mismatch");
}

device::LoRaFrame sensorFrame(const std::uint32_t sequence,
                              const std::string& text) {
    device::LoRaFrame frame;
    frame.type = device::LoRaMessageType::SensorEvent;
    frame.sequence = sequence;
    frame.payload.assign(text.begin(), text.end());
    return frame;
}

void testLoRaFraming() {
    device::UartDriver unused({"/dev/null", 115200, 10});
    device::LoRaDriver decoder(unused);
    const auto first = device::LoRaDriver::encode(
        sensorFrame(7, "SENSOR:HALL01:OCCUPIED:7"));
    auto frames = decoder.consume(
        std::span<const std::uint8_t>(first).first(5));
    require(frames.empty(), "partial LoRa frame was emitted early");
    frames = decoder.consume(
        std::span<const std::uint8_t>(first).subspan(5));
    require(frames.size() == 1 && frames[0].sequence == 7,
            "partial LoRa frame was not restored");

    auto corrupt = device::LoRaDriver::encode(
        sensorFrame(8, "SENSOR:HALL01:VACANT:8"));
    corrupt[10] ^= 0x01U;
    const auto valid = device::LoRaDriver::encode(
        sensorFrame(9, "SENSOR:HALL02:OCCUPIED:9"));
    corrupt.insert(corrupt.end(), valid.begin(), valid.end());
    frames = decoder.consume(corrupt);
    require(frames.size() == 1 && frames[0].sequence == 9,
            "LoRa decoder did not resynchronize after bad CRC");
    require(decoder.rejectedFrames() >= 1,
            "bad LoRa CRC was not counted");
}

void waitConnected(device::SensorLinkManager& manager) {
    for (int attempt = 0; attempt < 100 && !manager.connected(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(manager.connected(), "sensor link did not connect to PTY");
}

bool waitForEventCode(std::mutex& mutex,
                      const std::vector<event::SystemEventCode>& codes,
                      const event::SystemEventCode expected) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock(mutex);
            if (std::find(codes.begin(), codes.end(), expected) != codes.end())
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void testSensorLinkErrorReporting() {
    std::mutex event_mutex;
    std::vector<event::SystemEventCode> codes;
    event::SystemEventReporter::Config reporter_config;
    reporter_config.duplicate_window = std::chrono::milliseconds(100);
    reporter_config.sink_retry_delay = std::chrono::milliseconds(5);
    event::SystemEventReporter reporter(
        [&](const event::SystemEvent& system_event, const std::string&) {
            std::lock_guard lock(event_mutex);
            codes.push_back(system_event.code);
            return true;
        }, reporter_config);
    require(reporter.start(), "sensor error reporter did not start");

    device::SensorLinkManager::Config invalid_config;
    invalid_config.mode = device::SensorLinkMode::UartLine;
    invalid_config.uart = uartConfig("/dev/parking-missing-uart");
    invalid_config.reconnect_delay_ms = 10;
    device::SensorLinkManager invalid_link(
        invalid_config, [](const std::string&, const std::string&) {}, &reporter);
    require(invalid_link.start(), "invalid UART link worker did not start");
    require(waitForEventCode(event_mutex, codes,
                             event::SystemEventCode::UartOpenFailed),
            "UART open failure was not reported");
    invalid_link.stop();

    PseudoTerminal terminal;
    device::SensorLinkManager::Config lora_config;
    lora_config.mode = device::SensorLinkMode::LoRaFrame;
    lora_config.uart = uartConfig(terminal.path());
    lora_config.reconnect_delay_ms = 10;
    device::SensorLinkManager lora_link(
        lora_config, [](const std::string&, const std::string&) {}, &reporter);
    require(lora_link.start(), "LoRa error link worker did not start");
    waitConnected(lora_link);
    auto corrupt = device::LoRaDriver::encode(
        sensorFrame(77, "SENSOR:HALL01:OCCUPIED:77"));
    corrupt.back() ^= 0x01U;
    terminal.writeBytes(corrupt);
    require(waitForEventCode(event_mutex, codes,
                             event::SystemEventCode::LoRaFrameRejected),
            "LoRa rejected frame was not reported");
    lora_link.stop();
    reporter.stop();
}

void testSensorLink(const device::SensorLinkMode mode) {
    PseudoTerminal terminal;
    std::mutex mutex;
    std::condition_variable condition;
    std::string received_line;
    std::string received_transport;

    device::SensorLinkManager::Config config;
    config.mode = mode;
    config.uart = uartConfig(terminal.path());
    config.reconnect_delay_ms = 20;
    device::SensorLinkManager manager(
        config,
        [&](const std::string& line, const std::string& transport) {
            std::lock_guard lock(mutex);
            received_line = line;
            received_transport = transport;
            condition.notify_one();
        });
    require(manager.start(), "sensor link start failed");
    waitConnected(manager);

    const std::string expected = "SENSOR:HALL01:OCCUPIED:11";
    if (mode == device::SensorLinkMode::UartLine) {
        terminal.writeText(expected.substr(0, 8));
        terminal.writeText(expected.substr(8) + "\n");
    } else {
        const auto encoded = device::LoRaDriver::encode(sensorFrame(11, expected));
        terminal.writeBytes(std::vector<std::uint8_t>(encoded.begin(),
                                                      encoded.begin() + 4));
        terminal.writeBytes(std::vector<std::uint8_t>(encoded.begin() + 4,
                                                      encoded.end()));
    }

    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(2), [&] {
                    return !received_line.empty();
                }),
                "sensor link callback timeout");
        require(received_line == expected, "sensor link payload mismatch");
        require(received_transport ==
                    (mode == device::SensorLinkMode::UartLine ? "uart" : "lora"),
                "sensor link transport mismatch");
    }

    const std::string command = "ALERT:EV01:ON";
    std::string error;
    require(manager.sendAlertCommand(command, 12, &error),
            "alert command send failed: " + error);
    if (mode == device::SensorLinkMode::UartLine) {
        const auto bytes = terminal.readBytes(command.size() + 1);
        require(std::string(bytes.begin(), bytes.end()) == command + "\n",
                "UART alert command mismatch");
    } else {
        const auto bytes = terminal.readBytes(12 + command.size());
        device::UartDriver unused({"/dev/null", 115200, 10});
        device::LoRaDriver decoder(unused);
        const auto frames = decoder.consume(bytes);
        require(frames.size() == 1 &&
                    frames[0].type == device::LoRaMessageType::AlertCommand &&
                    frames[0].sequence == 12 &&
                    std::string(frames[0].payload.begin(), frames[0].payload.end()) ==
                        command,
                "LoRa alert command mismatch");
    }
    manager.stop();
}

}  // namespace

int main() {
    try {
        testUartTransport();
        testLoRaFraming();
        testSensorLink(device::SensorLinkMode::UartLine);
        testSensorLink(device::SensorLinkMode::LoRaFrame);
        testSensorLinkErrorReporting();
        std::cout << "UART/LoRa driver tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "UART/LoRa driver tests failed: " << exception.what() << '\n';
        return 1;
    }
}

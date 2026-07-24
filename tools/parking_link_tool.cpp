#include "device/LoRaDriver.hpp"
#include "device/UartDriver.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

std::atomic_bool running{true};

void stopHandler(int) { running.store(false); }

device::LoRaMessageType parseType(const std::string& value) {
    if (value == "sensor") return device::LoRaMessageType::SensorEvent;
    if (value == "alert") return device::LoRaMessageType::AlertCommand;
    if (value == "heartbeat") return device::LoRaMessageType::Heartbeat;
    throw std::invalid_argument("type must be sensor, alert, or heartbeat");
}

device::LoRaFrame makeFrame(const std::string& type,
                            const std::string& sequence,
                            const std::string& payload) {
    device::LoRaFrame frame;
    frame.type = parseType(type);
    frame.sequence = static_cast<std::uint32_t>(std::stoul(sequence));
    frame.payload.assign(payload.begin(), payload.end());
    return frame;
}

void printFrame(const device::LoRaFrame& frame) {
    std::cout << "type=" << static_cast<int>(frame.type)
              << " sequence=" << frame.sequence
              << " payload="
              << std::string(frame.payload.begin(), frame.payload.end()) << '\n';
}

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " encode <sensor|alert|heartbeat> <seq> <payload>\n"
        << "  " << program << " send <device> <baud> <type> <seq> <payload>\n"
        << "  " << program << " listen <device> <baud>\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(argv[0]);
            return 2;
        }
        const std::string command = argv[1];
        if (command == "encode" && argc == 5) {
            const auto encoded = device::LoRaDriver::encode(
                makeFrame(argv[2], argv[3], argv[4]));
            for (const std::uint8_t byte : encoded)
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(byte);
            std::cout << '\n';
            return 0;
        }
        if (command != "send" && command != "listen") {
            usage(argv[0]);
            return 2;
        }
        if ((command == "send" && argc != 7) ||
            (command == "listen" && argc != 4)) {
            usage(argv[0]);
            return 2;
        }

        device::UartDriver uart({argv[2], std::stoi(argv[3]), 250});
        std::string error;
        if (!uart.connect(&error)) throw std::runtime_error(error);
        device::LoRaDriver lora(uart);
        if (command == "send") {
            if (!lora.send(makeFrame(argv[4], argv[5], argv[6]), &error))
                throw std::runtime_error(error);
            std::cout << "frame sent\n";
            return 0;
        }

        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        std::array<std::uint8_t, 512> buffer{};
        while (running.load()) {
            const int count = uart.readSome(buffer, &error);
            if (count < 0) throw std::runtime_error(error);
            if (count == 0) continue;
            for (const auto& frame : lora.consume(
                     std::span<const std::uint8_t>(buffer.data(),
                                                   static_cast<std::size_t>(count))))
                printFrame(frame);
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "parking-link-tool: " << exception.what() << '\n';
        return 1;
    }
}

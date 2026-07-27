#include "device/LoRaDriver.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace device {
namespace {

void appendU16(std::vector<std::uint8_t>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void appendU32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::uint16_t readU16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}

std::uint32_t readU32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) |
           static_cast<std::uint32_t>(input[3]);
}

}  // namespace

std::uint16_t LoRaDriver::crc16Ccitt(
    const std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

std::vector<std::uint8_t> LoRaDriver::encode(const LoRaFrame& frame) {
    if (frame.payload.size() > kMaxPayload)
        throw std::invalid_argument("LoRa payload exceeds 512 bytes");

    std::vector<std::uint8_t> encoded;
    encoded.reserve(12 + frame.payload.size());
    encoded.push_back(kSof0);
    encoded.push_back(kSof1);
    encoded.push_back(kVersion);
    encoded.push_back(static_cast<std::uint8_t>(frame.type));
    appendU32(encoded, frame.sequence);
    appendU16(encoded, static_cast<std::uint16_t>(frame.payload.size()));
    encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
    const std::uint16_t crc = crc16Ccitt(
        std::span<const std::uint8_t>(encoded).subspan(2));
    appendU16(encoded, crc);
    return encoded;
}

std::vector<LoRaFrame> LoRaDriver::consume(
    const std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<LoRaFrame> frames;
    constexpr std::array<std::uint8_t, 2> marker{kSof0, kSof1};
    while (true) {
        const auto sof = std::search(buffer_.begin(), buffer_.end(),
                                     marker.begin(), marker.end());
        if (sof == buffer_.end()) {
            if (!buffer_.empty() && buffer_.back() == kSof0)
                buffer_.erase(buffer_.begin(), buffer_.end() - 1);
            else
                buffer_.clear();
            break;
        }
        buffer_.erase(buffer_.begin(), sof);
        if (buffer_.size() < 10) break;
        if (buffer_[2] != kVersion) {
            ++rejected_frames_;
            buffer_.erase(buffer_.begin());
            continue;
        }
        const std::size_t payload_size = readU16(buffer_.data() + 8);
        if (payload_size > kMaxPayload) {
            ++rejected_frames_;
            buffer_.erase(buffer_.begin());
            continue;
        }
        const std::size_t frame_size = 12 + payload_size;
        if (buffer_.size() < frame_size) break;
        const std::uint16_t expected = readU16(buffer_.data() + 10 + payload_size);
        const std::uint16_t actual = crc16Ccitt(
            std::span<const std::uint8_t>(buffer_).subspan(2, 8 + payload_size));
        if (expected != actual) {
            ++rejected_frames_;
            buffer_.erase(buffer_.begin());
            continue;
        }
        LoRaFrame frame;
        frame.type = static_cast<LoRaMessageType>(buffer_[3]);
        frame.sequence = readU32(buffer_.data() + 4);
        frame.payload.assign(buffer_.begin() + 10,
                             buffer_.begin() + 10 + payload_size);
        frames.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(), buffer_.begin() + frame_size);
    }
    return frames;
}

bool LoRaDriver::send(const LoRaFrame& frame, std::string* error) {
    try {
        const auto encoded = encode(frame);
        return uart_.writeAll(encoded, error);
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

}  // namespace device

#pragma once

#include "device/UartDriver.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace device {

/** @brief UART 투명 전송 LoRa 모뎀 위의 프로젝트 frame 종류다. */
enum class LoRaMessageType : std::uint8_t {
    SensorEvent = 0x01,
    AlertCommand = 0x02,
    Heartbeat = 0x03
};

struct LoRaFrame {
    LoRaMessageType type{LoRaMessageType::SensorEvent};
    std::uint32_t sequence{};
    std::vector<std::uint8_t> payload;
};

/**
 * @brief SOF/길이/CRC16을 적용하는 LoRa framing 계층이다.
 *
 * 무선 주파수·SF·BW 설정은 모뎀 책임이며 이 클래스는 UART byte stream에서
 * 프로젝트 frame 경계를 복원한다.
 */
class LoRaDriver {
public:
    static constexpr std::uint8_t kSof0 = 0xAA;
    static constexpr std::uint8_t kSof1 = 0x55;
    static constexpr std::uint8_t kVersion = 0x01;
    static constexpr std::size_t kMaxPayload = 512;

    explicit LoRaDriver(UartDriver& uart) : uart_(uart) {}

    static std::vector<std::uint8_t> encode(const LoRaFrame& frame);
    std::vector<LoRaFrame> consume(std::span<const std::uint8_t> bytes);
    bool send(const LoRaFrame& frame, std::string* error = nullptr);

    [[nodiscard]] std::uint64_t rejectedFrames() const {
        return rejected_frames_;
    }
    static std::uint16_t crc16Ccitt(std::span<const std::uint8_t> bytes);

private:
    UartDriver& uart_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t rejected_frames_{};
};

}  // namespace device

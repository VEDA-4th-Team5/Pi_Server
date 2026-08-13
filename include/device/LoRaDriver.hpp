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
    std::uint8_t version{};
    LoRaMessageType type{LoRaMessageType::SensorEvent};
    std::uint32_t sequence{};
    std::vector<std::uint8_t> payload;
};

/**
 * @brief 고정점(fixed-point) 모드에서 하행 frame 앞에 붙는 LoRa 모듈 주소 헤더다.
 *
 * 모듈이 이 3 byte를 자신의 주소 설정으로 해석해 무선으로 내보내므로, 프로젝트
 * frame 바로 앞에 붙여 한 번에 write해야 한다. 빠뜨리면 모듈이 project frame의
 * 앞 3 byte(`AA 55 0x`)를 주소로 오인해 하행이 나가지 않는다.
 */
struct LoRaDestination {
    std::uint8_t addressHigh{};
    std::uint8_t addressLow{};
    std::uint8_t channel{};
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
    // 하행 encode()는 계속 v1.0(0x01)을 찍는다. STM 쪽이 하행에서 0x01/0x02를
    // 모두 받도록 이미 고쳐졌으므로, STM과 Pi를 같은 순간에 배포하지 않아도 된다.
    static constexpr std::uint8_t kVersion = 0x01;
    // 상행 consume()은 v1.0(노드 ID 없음)과 v1.1(노드 ID·화재 energy 추가) 프레임을
    // 모두 받아들인다. "보낼 때는 좁게, 받을 때는 넓게" 원칙.
    static constexpr std::uint8_t kVersionLegacy = 0x01;
    static constexpr std::uint8_t kVersionNodeAware = 0x02;
    static constexpr std::size_t kMaxPayload = 512;

    explicit LoRaDriver(UartDriver& uart) : uart_(uart) {}

    static std::vector<std::uint8_t> encode(const LoRaFrame& frame);
    std::vector<LoRaFrame> consume(std::span<const std::uint8_t> bytes);
    bool send(const LoRaFrame& frame, std::string* error = nullptr);
    // 고정점 모드 목적지 노드로 하행 frame을 보낸다. AlertCommand처럼 목적지가
    // 있는 하행에 쓴다.
    bool sendTo(const LoRaDestination& destination, const LoRaFrame& frame,
               std::string* error = nullptr);

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

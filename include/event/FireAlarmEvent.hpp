#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace event {

// Core 계층이 다루는 화재 후보 신호다. 어떤 전송로(UART/LoRa)로 왔는지,
// 어떤 프레임 규격이었는지는 여기서 알 필요가 없다.
struct FireSignal {
    std::string sensorId;
    bool detected{false};
    std::chrono::system_clock::time_point occurredAt{
        std::chrono::system_clock::now()};
    std::optional<std::uint64_t> sourceSequence;
    std::string sourceTransport{"unknown"};

    // 관제실에 근거로 함께 전달할 수신 원문.
    std::string rawPayload;
};

}  // namespace event

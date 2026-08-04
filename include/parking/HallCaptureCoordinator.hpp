#pragma once

#include "parking/HallCaptureTypes.hpp"
#include "parking/HallOcrPolicy.hpp"
#include "parking/ParkingSlotManager.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace parking {

/**
 * @brief 현재 SQLite 정수 세션과 30/60초 촬영·OCR 상태를 연결한다.
 *
 * DB 세션 생성과 종료는 HallParkingService가 담당한다. 이 객체는 전이의
 * sessionId를 이미 생성된 SQLite ID로 받아 중복 PARKING_SESSION 생성을 막는다.
 */
class HallCaptureCoordinator {
public:
    explicit HallCaptureCoordinator(HallCapturePorts ports,
                                    int maxOcrAttempts = 2);

    void onTransition(const ParkingTransitionResult& transition);
    [[nodiscard]] CaptureImageResult onCaptureImage(
        const CapturedImage& image);
    void onOcrOutcome(const HallOcrOutcome& outcome);
    [[nodiscard]] std::size_t trackedSessions() const;

private:
    struct Binding {
        std::int64_t sessionId{-1};
        std::string slotId;
        std::optional<CapturedImage> heldImage;
    };

    static std::optional<std::int64_t> parseSessionId(
        const std::string& value) noexcept;

    mutable std::mutex mutex_;
    HallCapturePorts ports_;
    HallOcrPolicy policy_;
    std::unordered_map<std::string, Binding> bindings_;
};

}  // namespace parking

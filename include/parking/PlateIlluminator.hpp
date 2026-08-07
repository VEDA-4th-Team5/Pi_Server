#pragma once

#include "parking/CaptureRequest.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace parking {

// 시각 구간 [startHour, endHour) 안에 있으면 야간으로 본다. start == end 는 야간이
// 없는 설정이다. 일출/일몰 계산은 하지 않으며 관리자가 계절마다 구간을 조정한다.
[[nodiscard]] bool isNightWindow(int hour, int startHour, int endHour) noexcept;

[[nodiscard]] bool isNightWindow(std::chrono::system_clock::time_point now,
                                 int startHour, int endHour) noexcept;

struct PlateIlluminatorConfig {
    bool enabled{false};
    int nightStartHour{19};
    int nightEndHour{6};
    std::chrono::milliseconds settleDelay{200};
};

// STM32로 보낼 AlertCommand payload와 전송 sequence를 전달한다.
// 전송 계층(UART/LoRa)은 모른다.
using LedCommandSender =
    std::function<bool(const std::string& payload, std::uint32_t sequence)>;

/**
 * @brief 예약된 촬영의 노출 순간에만 번호판 조명 LED를 켜는 정책이다.
 *
 * 상시 점등은 카메라 노출을 왜곡하고 눈부심·전력 부담을 키우므로 촬영 순간에만
 * 켠다. 야간 시간대이거나, 직전 촬영에서 번호판을 읽지 못해 보정이 필요한
 * 세션이면 켠다. 이미 번호판을 확보한 세션은 남은 촬영이 증거 저장용이므로 켜지
 * 않는다.
 *
 * 점등 구간은 STM32 페일세이프 타이머보다 짧아야 하므로, 파일 저장이나 DB 기록이
 * 아니라 실제 노출 호출만 Scope 로 감싼다.
 */
class PlateIlluminator {
public:
    /**
     * @brief 노출 구간 동안만 LED를 켜두는 RAII 가드다.
     *
     * 소멸할 때 소등하므로 예외가 나가도 LED가 켜진 채 남지 않는다. 조명이
     * 필요없어 점등하지 않은 경우에는 아무것도 하지 않는 빈 가드가 된다.
     */
    class Scope {
    public:
        Scope() noexcept = default;
        Scope(PlateIlluminator& owner, std::string sensorId,
              std::uint32_t sequence);
        ~Scope();

        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&& other) noexcept;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        [[nodiscard]] bool lit() const noexcept { return owner_ != nullptr; }
        [[nodiscard]] std::uint32_t sequence() const noexcept {
            return sequence_;
        }

    private:
        void release() noexcept;

        PlateIlluminator* owner_{};
        std::string sensorId_;
        std::uint32_t sequence_{};
    };

    PlateIlluminator(PlateIlluminatorConfig config, LedCommandSender sender);

    // 노출 직전에 호출한다. 조명이 필요하면 LED를 켜고 정착 지연만큼 기다린 뒤
    // 가드를 돌려준다. 반환값의 수명이 곧 점등 구간이다.
    [[nodiscard]] Scope illuminate(const CaptureRequest& request,
                                   std::chrono::system_clock::time_point now);
    [[nodiscard]] Scope illuminate(const CaptureRequest& request);

    // Gemini가 번호판을 읽지 못한 세션은 남은 예약 촬영에서 시간대와 무관하게
    // 켠다. 요청 자체가 실패했거나 OCR을 시도조차 못한 경우는 조명으로 해결되는
    // 문제가 아니므로 여기에 넣지 않는다.
    void markPlateUnreadable(std::int64_t sessionId);

    // 번호판을 확보한 세션은 남은 촬영이 증거 저장용이라 조명이 필요없다.
    void markResolved(std::int64_t sessionId);

    void forget(std::int64_t sessionId);
    void forget(const std::string& sessionId);

private:
    enum class SessionOcr {
        PlateUnreadable,
        Resolved
    };

    [[nodiscard]] bool needsLight(const CaptureRequest& request,
                                  std::chrono::system_clock::time_point now);

    bool send(const std::string& sensorId, const char* state,
              std::uint32_t sequence);

    void note(std::int64_t sessionId, SessionOcr state);

    PlateIlluminatorConfig config_;
    LedCommandSender sender_;
    std::atomic<std::uint32_t> sequence_{0};
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, SessionOcr> session_ocr_;
};

}  // namespace parking

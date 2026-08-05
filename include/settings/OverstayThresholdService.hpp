#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace database { class EventDatabase; }

namespace settings {

struct ThresholdUpdateResult {
    bool success{};
    int previousSeconds{};
    int thresholdSeconds{};
    std::string error;
};

/** @brief 장기점유 판정·증거 촬영의 단일 기준값을 DB와 메모리에 보관한다. */
class OverstayThresholdService {
public:
    static constexpr int kDefaultSeconds = 3600;
    static constexpr int kMinimumSeconds = 60;
    static constexpr int kMaximumSeconds = 86400;
    static constexpr const char* kSettingKey = "overstay_threshold_seconds";
    using ApplyCallback = std::function<void(std::chrono::milliseconds)>;

    OverstayThresholdService(database::EventDatabase& database,
                             int bootstrap_seconds = kDefaultSeconds);

    /** @brief DB 값을 로드하며 없으면 bootstrap 기본값을 영구 저장한다. */
    bool initialize();
    [[nodiscard]] int thresholdSeconds() const noexcept;
    [[nodiscard]] static bool isValid(int seconds) noexcept;
    /** @brief DB 저장 성공 후에만 메모리와 런타임 타이머에 새 값을 적용한다. */
    ThresholdUpdateResult update(int seconds);
    /** @brief 타이머·증거 worker 재예약 callback을 서버 조립 단계에서 주입한다. */
    void setApplyCallback(ApplyCallback callback);

private:
    database::EventDatabase& database_;
    int bootstrap_seconds_;
    std::atomic<int> threshold_seconds_{kDefaultSeconds};
    std::mutex update_mutex_;
    ApplyCallback apply_callback_;
};

}  // namespace settings

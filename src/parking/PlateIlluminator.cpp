#include "parking/PlateIlluminator.hpp"

#include "util/Logger.hpp"

#include <charconv>
#include <ctime>
#include <optional>
#include <thread>
#include <utility>

namespace parking {
namespace {

std::optional<std::int64_t> parseSessionId(const std::string& value) noexcept {
    std::int64_t parsed{-1};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed < 0) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace

bool isNightWindow(const int hour, const int startHour,
                   const int endHour) noexcept {
    if (hour < 0 || hour > 23) return false;
    if (startHour == endHour) return false;
    // 19시~6시처럼 자정을 넘는 구간은 두 조각의 합집합이다.
    if (startHour > endHour) return hour >= startHour || hour < endHour;
    return hour >= startHour && hour < endHour;
}

bool isNightWindow(const std::chrono::system_clock::time_point now,
                   const int startHour, const int endHour) noexcept {
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&raw, &local);
    return isNightWindow(local.tm_hour, startHour, endHour);
}

PlateIlluminator::Scope::Scope(PlateIlluminator& owner, std::string sensorId,
                               const std::uint32_t sequence)
    : owner_(&owner), sensorId_(std::move(sensorId)), sequence_(sequence) {}

PlateIlluminator::Scope::~Scope() { release(); }

PlateIlluminator::Scope::Scope(Scope&& other) noexcept
    : owner_(other.owner_), sensorId_(std::move(other.sensorId_)),
      sequence_(other.sequence_) {
    other.owner_ = nullptr;
}

PlateIlluminator::Scope& PlateIlluminator::Scope::operator=(
    Scope&& other) noexcept {
    if (this == &other) return *this;
    release();
    owner_ = other.owner_;
    sensorId_ = std::move(other.sensorId_);
    sequence_ = other.sequence_;
    other.owner_ = nullptr;
    return *this;
}

void PlateIlluminator::Scope::release() noexcept {
    if (owner_ == nullptr) return;
    PlateIlluminator* const owner = owner_;
    owner_ = nullptr;
    try {
        // 실패해도 STM32 페일세이프 타이머가 소등하므로 재시도하지 않는다.
        owner->send(sensorId_, "OFF", sequence_);
    } catch (...) {
        // 소멸자에서 예외를 밖으로 내보내지 않는다.
    }
}

PlateIlluminator::PlateIlluminator(PlateIlluminatorConfig config,
                                   LedCommandSender sender)
    : config_(config), sender_(std::move(sender)) {}

bool PlateIlluminator::needsLight(
    const CaptureRequest& request,
    const std::chrono::system_clock::time_point now) {
    const auto sessionId = parseSessionId(request.sessionId);
    if (sessionId) {
        std::lock_guard lock(mutex_);
        const auto found = session_ocr_.find(*sessionId);
        if (found != session_ocr_.end()) {
            // 번호판을 이미 확보했으면 남은 촬영은 증거 저장용이라 켜지 않는다.
            if (found->second == SessionOcr::Resolved) return false;
            return true;
        }
    }
    return isNightWindow(now, config_.nightStartHour, config_.nightEndHour);
}

bool PlateIlluminator::send(const std::string& sensorId, const char* state,
                            const std::uint32_t sequence) {
    const std::string payload =
        "ALERT:" + sensorId + ":LED:" + state + ":" + std::to_string(sequence);
    if (sender_ && sender_(payload, sequence)) return true;
    util::logWarn("plate LED command send failed: " + payload);
    return false;
}

PlateIlluminator::Scope PlateIlluminator::illuminate(
    const CaptureRequest& request,
    const std::chrono::system_clock::time_point now) {
    if (!config_.enabled || !sender_ || request.sensorId.empty()) return {};
    if (!needsLight(request, now)) return {};

    const std::uint32_t sequence = ++sequence_;
    if (!send(request.sensorId, "ON", sequence)) return {};
    util::logLine("PLATE_LED", "on sensor=" + request.sensorId + " session=" +
                                   request.sessionId + " seq=" +
                                   std::to_string(sequence));
    if (config_.settleDelay.count() > 0)
        std::this_thread::sleep_for(config_.settleDelay);
    return Scope(*this, request.sensorId, sequence);
}

PlateIlluminator::Scope PlateIlluminator::illuminate(
    const CaptureRequest& request) {
    return illuminate(request, std::chrono::system_clock::now());
}

void PlateIlluminator::note(const std::int64_t sessionId,
                            const SessionOcr state) {
    if (sessionId < 0) return;
    std::lock_guard lock(mutex_);
    session_ocr_[sessionId] = state;
}

void PlateIlluminator::markPlateUnreadable(const std::int64_t sessionId) {
    note(sessionId, SessionOcr::PlateUnreadable);
}

void PlateIlluminator::markResolved(const std::int64_t sessionId) {
    note(sessionId, SessionOcr::Resolved);
}

void PlateIlluminator::forget(const std::int64_t sessionId) {
    std::lock_guard lock(mutex_);
    session_ocr_.erase(sessionId);
}

void PlateIlluminator::forget(const std::string& sessionId) {
    if (const auto parsed = parseSessionId(sessionId)) forget(*parsed);
}

}  // namespace parking

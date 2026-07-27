#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace event {

/** @brief 센서·통신 계층에서 사용하는 안정적인 이벤트 발생 주체다. */
enum class SystemEventSource {
    HallSensor,
    Uart,
    LoRa,
    Mqtt,
    Rtsp,
    Database
};

/** @brief 운영자가 판단할 수 있는 오류 심각도다. */
enum class SystemEventSeverity {
    Warning,
    Error,
    Critical
};

/** @brief 문자열 로그 대신 코드로 분류하는 센서·통신 오류 및 복구 종류다. */
enum class SystemEventCode {
    SensorMessageInvalid,
    SensorNotMapped,
    SensorSequenceRejected,
    SensorHandlerFailed,
    UartOpenFailed,
    UartReadFailed,
    UartWriteFailed,
    UartBufferOverflow,
    UartRecovered,
    LoRaFrameRejected,
    LoRaRecovered
};

/** @brief 모듈에서 reporter로 넘기는 transport-neutral 운영 이벤트다. */
struct SystemEvent {
    SystemEventSource source{SystemEventSource::HallSensor};
    SystemEventCode code{SystemEventCode::SensorMessageInvalid};
    SystemEventSeverity severity{SystemEventSeverity::Warning};
    std::string slot_id;
    std::string transport;
    std::string device;
    std::string message;
    std::uint32_t retry_count{};
    std::uint64_t suppressed_count{};
    std::uint64_t dropped_count{};
    bool recovered{};
};

[[nodiscard]] std::string toString(SystemEventSource source);
[[nodiscard]] std::string toString(SystemEventCode code);
[[nodiscard]] std::string toString(SystemEventSeverity severity);
[[nodiscard]] std::string serializeSystemEvent(const SystemEvent& event);

/**
 * @brief 오류 발생 스레드와 DB 저장을 분리하는 bounded 비동기 reporter다.
 *
 * report()는 noexcept이며 짧게 queue에 넣고 반환한다. 동일 key 오류는 설정 시간 동안
 * 억제하고, sink 실패 시 reporter 자신을 재호출하지 않은 채 queue에서 재시도한다.
 */
class SystemEventReporter {
public:
    using Sink = std::function<bool(const SystemEvent&, const std::string&)>;

    struct Config {
        std::size_t queue_capacity{100};
        std::chrono::milliseconds duplicate_window{std::chrono::seconds(30)};
        std::chrono::milliseconds sink_retry_delay{std::chrono::seconds(1)};
    };

    explicit SystemEventReporter(Sink sink);
    SystemEventReporter(Sink sink, Config config);
    ~SystemEventReporter();

    SystemEventReporter(const SystemEventReporter&) = delete;
    SystemEventReporter& operator=(const SystemEventReporter&) = delete;

    bool start();
    void stop() noexcept;
    void report(SystemEvent event) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t queuedCount() const noexcept;

private:
    struct DuplicateState {
        std::chrono::steady_clock::time_point last_enqueued;
        std::uint64_t suppressed{};
    };

    static std::string deduplicationKey(const SystemEvent& event);
    void clearRecoveredStateLocked(const SystemEvent& event);
    void enqueueLocked(SystemEvent event);
    void run() noexcept;
    bool persist(const SystemEvent& event) noexcept;

    Sink sink_;
    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SystemEvent> queue_;
    std::unordered_map<std::string, DuplicateState> duplicate_states_;
    std::uint64_t dropped_events_{};
    bool running_{};
    bool stopping_{};
    std::thread worker_;
};

}  // namespace event

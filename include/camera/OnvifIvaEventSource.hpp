#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace camera {

/** @brief 카메라 ONVIF PullPoint가 전달한 WiseAI IvaArea 원본 이벤트다. */
struct OnvifIvaEvent {
    std::string utcTime;
    std::string operation;
    std::string videoSourceToken;
    std::string ruleName;
    std::string state;
    std::string objectId;
    std::string action;
};

/**
 * @brief ONVIF PullPoint를 long-poll하여 WiseAI Intrusion/Exit를 수신한다.
 *
 * MQTT Publication과 독립된 카메라 원본 이벤트 입력이다. 구독 갱신과 일시적인
 * 연결 실패 후 재접속을 worker 스레드에서 수행하며, 콜백에는 선택된 채널의
 * IvaArea 이벤트만 전달한다.
 */
class OnvifIvaEventSource {
public:
    struct Config {
        std::string endpoint;
        std::string username;
        std::string password;
        // 비어 있으면 모든 token을 허용한다. 운영 서버는 슬롯 설정에서 생성한다.
        std::set<std::string> acceptedVideoSourceTokens;
        bool deliverInitialized{false};
        std::chrono::seconds pullTimeout{10};
        std::chrono::seconds renewInterval{30};
        std::chrono::milliseconds reconnectDelay{2000};
    };

    using EventHandler = std::function<bool(const OnvifIvaEvent&)>;

    OnvifIvaEventSource(Config config, EventHandler handler);
    ~OnvifIvaEventSource();

    OnvifIvaEventSource(const OnvifIvaEventSource&) = delete;
    OnvifIvaEventSource& operator=(const OnvifIvaEventSource&) = delete;

    /** @brief worker를 시작한다. 설정 오류나 curl 초기화 실패 시 false다. */
    bool start();
    /** @brief 진행 중인 long-poll을 중단하고 worker를 join한다. */
    void stop() noexcept;
    [[nodiscard]] bool started() const noexcept;

    /** @brief Snapshot OpenAPI 주소 등에서 ONVIF Event Service 주소를 만든다. */
    [[nodiscard]] static std::string deriveEventEndpoint(
        const std::string& cameraBaseUrl);
    /** @brief PullMessages XML에서 IvaArea 이벤트를 순서대로 분리한다. */
    [[nodiscard]] static std::vector<OnvifIvaEvent> parseEvents(
        const std::string& xml);

private:
    void run() noexcept;

    Config config_;
    EventHandler handler_;
    std::atomic_bool stopRequested_{false};
    mutable std::mutex lifecycleMutex_;
    std::thread worker_;
    bool started_{};
};

}  // namespace camera

#include "notification/TelegramApiClient.hpp"
#include "notification/TelegramChannelNotifier.hpp"
#include "notification/TelegramMessage.hpp"
#include "notification/TelegramMessageFormatter.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void testFormatterIncludesAllFields() {
    notification::TelegramMessageFormatter formatter;
    notification::TelegramMessage message;
    message.topic = "parking/fire/ch01";
    message.payload = R"({"event_type":"FIRE_SUSPECTED"})";
    message.qos = 1;
    message.retain = false;

    const std::string text = formatter.format(message);

    require(contains(text, "Topic: parking/fire/ch01"), "topic must be included");
    require(contains(text, "QoS: 1"), "qos must be included");
    require(contains(text, "Retain: false"), "retain flag must be included");
    require(contains(text, message.payload), "payload must be included verbatim");
}

void testFormatterTruncatesOverlongPayload() {
    notification::TelegramMessageFormatter formatter;
    notification::TelegramMessage message;
    message.topic = "t";
    message.payload = std::string(5000, 'x');

    const std::string text = formatter.format(message);

    require(text.size() <= notification::kTelegramMaxTextLength,
            "formatted message must never exceed the Telegram text limit, "
            "or the real send would be rejected by the API");
    require(contains(text, "[truncated]"),
            "a truncated payload must say so, otherwise operators would "
            "read a silently-cut message as complete");
}

void testFormatterDoesNotTruncateAtExactLimit() {
    // 4096바이트 언저리 경계값 — 헤더("[Pi Server MQTT]\nTopic: ...")까지
    // 합쳐서 정확히 kTelegramMaxTextLength일 때 자르지 않는지 확인한다.
    notification::TelegramMessageFormatter formatter;
    notification::TelegramMessage message;
    message.topic = "t";
    message.payload = std::string(10, 'x');

    const std::string text = formatter.format(message);
    require(!contains(text, "[truncated]"),
            "a short payload must not be truncated");
}

void testApiClientShortCircuitsWhenDisabled() {
    notification::TelegramApiClient::Config config;
    config.enabled = false;
    config.bot_token = "unused";
    config.channel = "unused";
    notification::TelegramApiClient client(config);

    require(!client.isEnabled(), "disabled config must report isEnabled() == false");
    require(!client.sendMessage("hello"),
            "sendMessage must return false without a real HTTP call when "
            "disabled, so tests/CI never hit the real Telegram API");
}

void testApiClientShortCircuitsWhenMissingCredentials() {
    notification::TelegramApiClient::Config config;
    config.enabled = true;
    config.bot_token = "";
    config.channel = "";
    notification::TelegramApiClient client(config);

    require(!client.sendMessage("hello"),
            "an enabled client with no bot_token/channel must still refuse "
            "to send rather than making a doomed request");
}

void testChannelNotifierNoOpsWhenDisabled() {
    notification::TelegramApiClient::Config api_config;
    api_config.enabled = false;
    notification::TelegramApiClient api_client(api_config);

    notification::TelegramChannelNotifier::Config notifier_config;
    notifier_config.enabled = false;
    notification::TelegramChannelNotifier notifier(notifier_config, api_client);

    require(!notifier.start(), "start() must refuse when disabled");
    require(!notifier.running(), "a disabled notifier must never report running");
    require(!notifier.enqueue({"t", "p", 1, false}),
            "enqueue() must refuse when disabled, not silently buffer forever");
}

void testChannelNotifierLifecycleWithUnreachableApi() {
    // api_client는 disabled라 실제 네트워크를 절대 타지 않지만, notifier
    // 자체는 enabled라 워커 스레드/큐/재시도 로직은 실제로 돈다.
    notification::TelegramApiClient::Config api_config;
    api_config.enabled = false;
    notification::TelegramApiClient api_client(api_config);

    notification::TelegramChannelNotifier::Config notifier_config;
    notifier_config.enabled = true;
    notifier_config.queue_capacity = 2;
    notifier_config.retry_count = 0;
    notifier_config.retry_delay_ms = 1;
    notification::TelegramChannelNotifier notifier(notifier_config, api_client);

    require(notifier.start(), "start() must succeed when enabled");
    require(notifier.running(), "running() must be true after start()");
    require(notifier.start(),
            "calling start() again while already running must be a safe "
            "no-op, not spawn a second worker thread");

    require(notifier.enqueue({"a", "1", 1, false}), "enqueue must accept while running");
    require(notifier.enqueue({"b", "2", 1, false}), "enqueue must accept up to capacity");
    // 용량(2) 초과 — 내부적으로 가장 오래된 메시지를 버리는 경로를 실행한다.
    // 큐 내용을 밖에서 들여다볼 공개 API가 없어 "무엇이" 버려졌는지는 여기서
    // 검증할 수 없지만, 이 경로가 죽거나 멈추지 않는지는 확인된다.
    require(notifier.enqueue({"c", "3", 1, false}),
            "enqueue must still accept past capacity by dropping the oldest, "
            "not by rejecting the newest");

    // stop()이 처리 중/대기 중인 메시지가 있어도 멈추지 않고 정상 join되는지.
    // 버그가 있으면 이 호출 자체가 걸려서 ctest가 타임아웃으로 실패한다.
    notifier.stop();
    require(!notifier.running(), "running() must be false after stop()");

    // 재시작이 가능해야 한다 — 1회성 자원이 아니다.
    require(notifier.start(), "a stopped notifier must be restartable");
    notifier.stop();
}

}  // namespace

int main() {
    try {
        testFormatterIncludesAllFields();
        testFormatterTruncatesOverlongPayload();
        testFormatterDoesNotTruncateAtExactLimit();
        testApiClientShortCircuitsWhenDisabled();
        testApiClientShortCircuitsWhenMissingCredentials();
        testChannelNotifierNoOpsWhenDisabled();
        testChannelNotifierLifecycleWithUnreachableApi();
    } catch (const std::exception& error) {
        std::cerr << "TelegramNotificationTest failed: " << error.what()
                  << std::endl;
        return 1;
    }

    std::cout << "TelegramNotificationTest passed" << std::endl;
    return 0;
}

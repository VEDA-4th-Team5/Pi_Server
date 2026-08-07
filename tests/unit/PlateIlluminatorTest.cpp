#include "parking/PlateIlluminator.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::CaptureRequest requestFor(const std::string& sessionId) {
    parking::CaptureRequest request;
    request.sessionId = sessionId;
    request.slotId = "A01";
    request.sensorId = "HALL01";
    return request;
}

// 야간 판단과 무관하게 강제로 켜거나 끄기 위한 고정 시각이다.
std::chrono::system_clock::time_point atHour(const int hour) {
    std::tm local{};
    local.tm_year = 126;  // 2026
    local.tm_mon = 0;
    local.tm_mday = 15;
    local.tm_hour = hour;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

parking::PlateIlluminatorConfig testConfig() {
    parking::PlateIlluminatorConfig config;
    config.enabled = true;
    config.nightStartHour = 19;
    config.nightEndHour = 6;
    config.settleDelay = std::chrono::milliseconds(0);
    return config;
}

// 전송된 payload를 순서대로 기록하는 시험용 조명이다.
class Recorder {
public:
    explicit Recorder(parking::PlateIlluminatorConfig config = testConfig(),
                      const bool sendSucceeds = true)
        : illuminator_(config, [this, sendSucceeds](const std::string& payload,
                                                    std::uint32_t) {
              sent_.push_back(payload);
              return sendSucceeds;
          }) {}

    parking::PlateIlluminator& operator*() { return illuminator_; }
    parking::PlateIlluminator* operator->() { return &illuminator_; }
    [[nodiscard]] const std::vector<std::string>& sent() const { return sent_; }

private:
    std::vector<std::string> sent_;
    parking::PlateIlluminator illuminator_;
};

void nightWindowWrapsMidnight() {
    require(parking::isNightWindow(20, 19, 6), "20h is inside 19-6");
    require(parking::isNightWindow(19, 19, 6), "start hour is inclusive");
    require(parking::isNightWindow(0, 19, 6), "midnight is inside 19-6");
    require(parking::isNightWindow(5, 19, 6), "5h is inside 19-6");
    require(!parking::isNightWindow(6, 19, 6), "end hour is exclusive");
    require(!parking::isNightWindow(12, 19, 6), "noon is daytime");
    require(!parking::isNightWindow(18, 19, 6), "18h is before the window");
}

void nightWindowWithinSameDay() {
    require(parking::isNightWindow(3, 1, 5), "3h is inside 1-5");
    require(!parking::isNightWindow(0, 1, 5), "0h is before 1-5");
    require(!parking::isNightWindow(5, 1, 5), "end hour is exclusive");
}

void degenerateWindowNeverLights() {
    require(!parking::isNightWindow(0, 0, 0), "start == end disables night");
    require(!parking::isNightWindow(12, 12, 12), "start == end disables night");
    require(!parking::isNightWindow(-1, 19, 6), "invalid hour is not night");
    require(!parking::isNightWindow(24, 19, 6), "invalid hour is not night");
}

void daytimeCaptureSkipsLed() {
    Recorder recorder;
    require(!recorder->illuminate(requestFor("42"), atHour(13)).lit(),
            "daytime capture must not light the LED");
    require(recorder.sent().empty(), "daytime capture must send no command");
}

void nightCapturePairsOnAndOff() {
    Recorder recorder;
    std::uint32_t sequence{};
    {
        const auto lamp = recorder->illuminate(requestFor("42"), atHour(22));
        require(lamp.lit(), "night capture must light the LED");
        sequence = lamp.sequence();
        require(recorder.sent().size() == 1,
                "the LED must stay on while the guard is alive");
    }
    const auto& sent = recorder.sent();
    require(sent.size() == 2, "one capture must send exactly ON then OFF");
    require(sent[0] == "ALERT:HALL01:LED:ON:" + std::to_string(sequence),
            "ON payload must carry sensor id and sequence: " + sent[0]);
    require(sent[1] == "ALERT:HALL01:LED:OFF:" + std::to_string(sequence),
            "OFF must reuse the ON sequence: " + sent[1]);
}

// 촬영이 예외를 던져도 LED가 켜진 채 남으면 안 된다.
void exceptionDuringExposureStillTurnsOff() {
    Recorder recorder;
    try {
        const auto lamp = recorder->illuminate(requestFor("42"), atHour(22));
        require(lamp.lit(), "night capture must light the LED");
        throw std::runtime_error("capture blew up");
    } catch (const std::runtime_error&) {
        // 촬영 실패는 여기서 흡수한다.
    }
    const auto& sent = recorder.sent();
    require(sent.size() == 2, "an exception must still emit OFF");
    require(sent[1].rfind("ALERT:HALL01:LED:OFF:", 0) == 0,
            "the trailing command must be OFF: " + sent[1]);
}

void unreadablePlateLightsNextDaytimeCapture() {
    Recorder recorder;
    const auto request = requestFor("42");
    require(!recorder->illuminate(request, atHour(13)).lit(),
            "first daytime capture stays dark");

    recorder->markPlateUnreadable(42);
    require(recorder->illuminate(request, atHour(13)).lit(),
            "unreadable plate must light the next daytime capture");

    // 다른 세션은 영향을 받지 않는다.
    require(!recorder->illuminate(requestFor("43"), atHour(13)).lit(),
            "the flag must be scoped to its own session");

    recorder->forget(std::string("42"));
    require(!recorder->illuminate(request, atHour(13)).lit(),
            "a closed session must stop forcing the LED");
}

// 30초 촬영에서 번호판을 얻으면 60초 촬영은 증거 저장용이라 조명이 필요없다.
void resolvedSessionSkipsRemainingNightCaptures() {
    Recorder recorder;
    const auto request = requestFor("42");
    require(recorder->illuminate(request, atHour(22)).lit(),
            "the first night capture must light the LED");

    recorder->markResolved(42);
    require(!recorder->illuminate(request, atHour(22)).lit(),
            "a resolved session must not light remaining night captures");

    // 아직 번호판을 못 얻은 세션은 그대로 켠다.
    require(recorder->illuminate(requestFor("43"), atHour(22)).lit(),
            "an unresolved session still lights at night");
}

// 읽지 못해 켜기로 한 세션이 뒤이어 인식되면 더는 켜지 않는다.
void resolutionOverridesEarlierFailure() {
    Recorder recorder;
    const auto request = requestFor("42");
    recorder->markPlateUnreadable(42);
    require(recorder->illuminate(request, atHour(13)).lit(),
            "unreadable plate forces the LED in daytime");
    recorder->markResolved(42);
    require(!recorder->illuminate(request, atHour(13)).lit(),
            "a later success must clear the forced LED");
}

void disabledIlluminatorStaysSilent() {
    parking::PlateIlluminatorConfig config = testConfig();
    config.enabled = false;
    Recorder recorder(config);
    recorder->markPlateUnreadable(42);
    require(!recorder->illuminate(requestFor("42"), atHour(22)).lit(),
            "disabled illuminator must never light");
    require(recorder.sent().empty(), "disabled illuminator must send nothing");
}

void failedSendReportsNoBeam() {
    Recorder recorder(testConfig(), false);
    const auto lamp = recorder->illuminate(requestFor("42"), atHour(22));
    require(!lamp.lit(), "a rejected ON must not claim the LED is lit");
    require(recorder.sent().size() == 1,
            "a rejected ON must not be followed by a stray OFF");
}

void missingSensorIdSkipsLed() {
    Recorder recorder;
    parking::CaptureRequest request = requestFor("42");
    request.sensorId.clear();
    require(!recorder->illuminate(request, atHour(22)).lit(),
            "an unaddressable LED must not be commanded");
    require(recorder.sent().empty(), "no sensor id means no command");
}

// 가드를 옮겨도 소등은 정확히 한 번만 일어나야 한다.
void movedScopeTurnsOffExactlyOnce() {
    Recorder recorder;
    {
        auto lamp = recorder->illuminate(requestFor("42"), atHour(22));
        require(lamp.lit(), "night capture must light the LED");
        const auto moved = std::move(lamp);
        require(moved.lit(), "the moved-to guard owns the LED");
        require(!lamp.lit(), "the moved-from guard must release ownership");
    }
    require(recorder.sent().size() == 2,
            "a moved guard must still emit exactly one OFF");
}

}  // namespace

int main() {
    try {
        nightWindowWrapsMidnight();
        nightWindowWithinSameDay();
        degenerateWindowNeverLights();
        daytimeCaptureSkipsLed();
        nightCapturePairsOnAndOff();
        exceptionDuringExposureStillTurnsOff();
        unreadablePlateLightsNextDaytimeCapture();
        resolvedSessionSkipsRemainingNightCaptures();
        resolutionOverridesEarlierFailure();
        disabledIlluminatorStaysSilent();
        failedSendReportsNoBeam();
        missingSensorIdSkipsLed();
        movedScopeTurnsOffExactlyOnce();
        std::cout << "PlateIlluminatorTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "PlateIlluminatorTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

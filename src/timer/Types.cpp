#include "parking_timer/Types.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace parking_timer {

/**
 * @brief 차량 분류 열거형을 로그와 화면에 사용할 문자열로 변환한다.
 *
 * @param[in] category 변환할 차량 분류 값.
 * @return 열거형의 수명과 무관하게 유효한 정적 문자열 포인터.
 * @note 알 수 없는 열거형 값도 안전하게 `UNKNOWN`으로 처리하며 예외를 던지지 않는다.
 */
const char* toString(const VehicleCategory category) noexcept {
    switch (category) {
    case VehicleCategory::Ev:
        return "EV";
    case VehicleCategory::Phev:
        return "PHEV";
    case VehicleCategory::NonEv:
        return "NON_EV";
    case VehicleCategory::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

/**
 * @brief 현재 시스템 시각을 밀리초 정밀도의 ISO-8601 UTC 문자열로 만든다.
 *
 * @return `YYYY-MM-DDTHH:MM:SS.mmmZ` 형식의 UTC 시각 문자열.
 * @note 이 시각은 DB 기록과 관제 표시용이다. 타이머 경과시간 판정은 시스템 시각
 *       변경의 영향을 받지 않도록 `steady_clock`을 별도로 사용한다.
 */
std::string utcNow() {
    using namespace std::chrono;
    // 사람이 읽는 wall-clock과 밀리초 부분을 같은 순간에서 얻는다.
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % seconds{1};
    const std::time_t raw_time = system_clock::to_time_t(now);

    std::tm utc{};
    // gmtime 계열의 플랫폼별 thread-safe 버전을 선택한다.
#if defined(_WIN32)
    gmtime_s(&utc, &raw_time);
#else
    gmtime_r(&raw_time, &utc);
#endif

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << millis.count() << 'Z';
    return output.str();
}

}  // namespace parking_timer

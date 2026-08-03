#pragma once
#include <chrono>
#include <string>

namespace util {
/** @brief 지정한 system_clock 시각을 로컬 ISO-8601 문자열로 변환한다. */
std::string isoString(std::chrono::system_clock::time_point value);
/** @brief 이벤트 payload와 DB에 사용할 ISO 형식 현재 시각을 반환한다. */
std::string nowIsoString();
/** @brief 파일명에 안전한 현재 시각 문자열을 반환한다. */
std::string nowStringForFilename();

// 센서가 알려준 발생 시각처럼 "지금"이 아닌 시각을 같은 형식으로 찍을 때 쓴다.
std::string isoString(std::chrono::system_clock::time_point time_point);
}

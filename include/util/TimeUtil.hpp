#pragma once
#include <chrono>
#include <string>

namespace util {
std::string nowIsoString();
std::string nowStringForFilename();

// 센서가 알려준 발생 시각처럼 "지금"이 아닌 시각을 같은 형식으로 찍을 때 쓴다.
std::string isoString(std::chrono::system_clock::time_point time_point);
}

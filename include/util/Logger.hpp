#pragma once
#include <string>

namespace util {
void logLine(const std::string& level, const std::string& message);
void logInfo(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);

/**
 * @brief 해당 태그가 현재 로그 설정에서 켜져 있는지 확인한다.
 *
 * 출력 자체는 logLine 이 알아서 걸러내므로 보통은 쓸 일이 없다.
 * 메시지를 조립하는 비용이 아까울 때(경로 결합, DB 조회 등) 먼저 확인한다.
 */
bool logEnabled(const std::string& tag);
}

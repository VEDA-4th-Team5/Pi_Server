#include "parking_timer/RuntimeConfig.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace parking_timer {
namespace {

/**
 * @brief 문자열 양 끝의 공백과 개행 문자를 제거한다.
 *
 * @param[in] value 정리할 문자열. 함수 내부에서 이동·수정할 수 있도록 값으로 받는다.
 * @return 앞뒤 공백이 제거된 문자열. 공백뿐이면 빈 문자열.
 */
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/**
 * @brief 초 단위 설정 문자열을 양수 밀리초 duration으로 변환한다.
 *
 * @param[in] value 숫자로 해석할 설정값.
 * @param[in] source 오류 메시지에 표시할 설정 출처 또는 키 이름.
 * @return 입력 초를 나타내는 `std::chrono::milliseconds` 값.
 * @throws std::runtime_error 정수가 아니거나 0 이하인 경우.
 */
std::chrono::milliseconds parseTimeout(const std::string& value,
                                       const std::string_view source) {
    std::size_t consumed{};
    long long seconds{};
    try {
        seconds = std::stoll(value, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(source) + " must be a positive integer");
    }
    if (consumed != value.size() || seconds <= 0) {
        throw std::runtime_error(std::string(source) + " must be a positive integer");
    }
    return std::chrono::seconds{seconds};
}

/**
 * @brief 하나의 `KEY=VALUE` 설정을 런타임 설정 객체에 반영한다.
 *
 * @param[in,out] config 갱신할 런타임 설정.
 * @param[in] key 설정 키.
 * @param[in] value 설정 값.
 * @param[in] source 오류 메시지에 표시할 파일 또는 환경 출처.
 * @throws std::runtime_error timeout이 잘못됐거나 DB 경로가 빈 문자열인 경우.
 * @note 알 수 없는 키는 향후 설정 확장을 위해 현재는 무시한다.
 */
void applyValue(RuntimeConfig& config,
                const std::string& key,
                const std::string& value,
                const std::string_view source) {
    if (key == "PARKING_TIMEOUT_SECONDS") {
        config.parking_timeout = parseTimeout(value, source);
    } else if (key == "DATABASE_PATH") {
        if (value.empty()) {
            throw std::runtime_error(std::string(source) + " DATABASE_PATH is empty");
        }
        config.database_path = value;
    }
}

}  // namespace

/**
 * @brief `KEY=VALUE` 형식의 설정 파일을 읽어 런타임 설정을 만든다.
 *
 * @param[in] file 읽을 설정 파일 경로.
 * @return 파일 값이 반영된 `RuntimeConfig`.
 * @throws std::runtime_error 파일을 열 수 없거나 유효하지 않은 줄/값이 있는 경우.
 */
RuntimeConfig RuntimeConfig::load(const std::filesystem::path& file) {
    RuntimeConfig config;
    std::ifstream input(file);
    if (!input) {
        throw std::runtime_error("cannot open config file: " + file.string());
    }

    std::string line;
    std::size_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(std::move(line));
        // 빈 줄과 주석은 설정으로 해석하지 않는다.
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto delimiter = line.find('=');
        if (delimiter == std::string::npos) {
            throw std::runtime_error("invalid config line " + std::to_string(line_number));
        }
        const auto key = trim(line.substr(0, delimiter));
        const auto value = trim(line.substr(delimiter + 1));
        applyValue(config, key, value, file.string());
    }
    return config;
}

/**
 * @brief 지원하는 환경변수로 현재 런타임 설정을 덮어쓴다.
 *
 * @throws std::runtime_error `PARKING_TIMEOUT_SECONDS`가 양수가 아니거나
 *         `DATABASE_PATH`가 빈 문자열인 경우.
 * @note 설정 파일을 읽은 뒤 호출하면 파일보다 환경변수가 높은 우선순위를 갖는다.
 */
void RuntimeConfig::applyEnvironment() {
    if (const char* timeout = std::getenv("PARKING_TIMEOUT_SECONDS")) {
        parking_timeout = parseTimeout(timeout, "PARKING_TIMEOUT_SECONDS");
    }
    if (const char* database = std::getenv("DATABASE_PATH")) {
        if (*database == '\0') {
            throw std::runtime_error("DATABASE_PATH is empty");
        }
        database_path = database;
    }
}

}  // namespace parking_timer

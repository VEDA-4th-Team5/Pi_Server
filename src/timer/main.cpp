#include "parking_timer/EventDatabase.hpp"
#include "parking_timer/EventManager.hpp"
#include "parking_timer/ParkingSlotManager.hpp"
#include "parking_timer/RuntimeConfig.hpp"
#include "parking_timer/Types.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef PARKING_TIMER_SOURCE_DIR
#define PARKING_TIMER_SOURCE_DIR "."
#endif

namespace {

using parking_timer::EventDatabase;
using parking_timer::ParkingSlotManager;

/**
 * @brief 명령행에서 선택적으로 지정할 실행 옵션 모음.
 */
struct Options {
    std::optional<std::filesystem::path> config_file;
    std::optional<std::filesystem::path> sql_directory;
    std::optional<std::filesystem::path> database_path;
    std::optional<std::chrono::milliseconds> timeout;
    bool demo{};
    bool reset_logs{};
    bool help{};
};

/**
 * @brief CLI 문자열을 양의 정수로 엄격하게 변환한다.
 *
 * @param[in] value 변환할 문자열.
 * @param[in] option 오류 메시지에 표시할 옵션 또는 명령 이름.
 * @return 변환된 1 이상의 정수.
 * @throws std::invalid_argument 숫자가 아니거나 후행 문자가 있거나 0 이하인 경우.
 */
long long parsePositiveInteger(const std::string& value, const std::string& option) {
    std::size_t consumed{};
    long long parsed{};
    try {
        parsed = std::stoll(value, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(option + " requires a positive integer");
    }
    if (consumed != value.size() || parsed <= 0) {
        throw std::invalid_argument(option + " requires a positive integer");
    }
    return parsed;
}

/**
 * @brief 프로그램 명령행 인자를 구조화된 옵션으로 해석한다.
 *
 * @param[in] argc `main()`에서 받은 인자 개수.
 * @param[in] argv `main()`에서 받은 NUL 종료 인자 배열.
 * @return 파싱된 선택 옵션.
 * @throws std::invalid_argument 옵션 값이 빠졌거나 잘못됐거나 알 수 없는 옵션인 경우.
 */
Options parseArguments(const int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        // 값을 요구하는 옵션은 다음 argv를 소비하며, 없으면 현재 옵션 이름으로 진단한다.
        auto requireValue = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(argument + " requires a value");
            }
            return argv[++index];
        };

        if (argument == "--config") {
            options.config_file = requireValue();
        } else if (argument == "--sql-dir") {
            options.sql_directory = requireValue();
        } else if (argument == "--db") {
            options.database_path = requireValue();
        } else if (argument == "--timeout-seconds") {
            options.timeout = std::chrono::seconds{
                parsePositiveInteger(requireValue(), argument)};
        } else if (argument == "--demo") {
            options.demo = true;
        } else if (argument == "--reset-logs") {
            options.reset_logs = true;
        } else if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

/**
 * @brief 명시 경로 또는 우선순위 후보 목록에서 리소스를 찾는다.
 *
 * @param[in] explicit_path CLI로 직접 지정한 선택 경로.
 * @param[in] candidates 명시 경로가 없을 때 순서대로 검사할 후보 경로.
 * @param[in] description 오류 메시지에 표시할 리소스 설명.
 * @return 존재가 확인된 경로.
 * @throws std::runtime_error 명시 경로가 없거나 어떤 후보도 존재하지 않는 경우.
 * @throws std::filesystem::filesystem_error 파일시스템 검사 자체가 실패한 경우.
 * @note 잘못된 명시 경로는 조용히 fallback하지 않는다. 명시 경로가 없을 때만 후보의
 *       앞쪽부터 선택하므로 벡터 순서가 곧 탐색 우선순위다.
 */
std::filesystem::path findResource(
    const std::optional<std::filesystem::path>& explicit_path,
    const std::vector<std::filesystem::path>& candidates,
    const std::string& description) {
    if (explicit_path.has_value()) {
        if (!std::filesystem::exists(*explicit_path)) {
            throw std::runtime_error(description + " does not exist: " +
                                     explicit_path->string());
        }
        return *explicit_path;
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot locate " + description);
}

/**
 * @brief 현재 실행 중인 바이너리가 위치한 디렉터리를 구한다.
 *
 * @param[in] argv0 `/proc/self/exe`를 사용할 수 없을 때 fallback할 프로그램 경로.
 * @return 실행 파일의 부모 디렉터리.
 * @throws std::filesystem::filesystem_error fallback 절대경로 계산이 실패한 경우.
 * @note Linux에서는 현재 작업 디렉터리나 PATH 호출 방식과 무관한 `/proc/self/exe`를 우선한다.
 */
std::filesystem::path executableDirectory(const char* argv0) {
#if defined(__linux__)
    std::error_code error;
    const auto proc_path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !proc_path.empty()) {
        return proc_path.parent_path();
    }
#endif
    // /proc가 없는 플랫폼에서는 argv[0]가 가리키는 경로로 fallback한다.
    return std::filesystem::absolute(argv0).parent_path();
}

/**
 * @brief 프로그램 명령행 옵션 도움말을 출력한다.
 *
 * @param[in] executable 도움말 첫 줄에 표시할 실행 파일 이름.
 */
void printUsage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n\n"
        << "Options:\n"
        << "  --config PATH          timer.conf path\n"
        << "  --sql-dir PATH         directory containing schema.sql and seed.sql\n"
        << "  --db PATH              SQLite database path\n"
        << "  --timeout-seconds N    demo/operation timeout (default: 60)\n"
        << "  --reset-logs           delete timer-owned PARKING_SESSION rows before starting\n"
        << "  --demo                 run an automatic entry/exit/timeout scenario\n"
        << "  -h, --help             show this help\n";
}

/**
 * @brief nullable 문자열을 표 출력용 값으로 변환한다.
 *
 * @param[in] value 표시할 optional 문자열.
 * @return 값이 있으면 해당 문자열, SQL NULL에 대응하면 `-`.
 */
std::string displayOptional(const std::optional<std::string>& value) {
    return value.value_or("-");
}

/**
 * @brief 현재 `timer_log` 내용을 사람이 읽을 수 있는 표로 출력한다.
 *
 * @param[in] database 조회할 이벤트 DB.
 * @throws std::runtime_error DB 조회가 실패한 경우.
 */
void printLogs(const EventDatabase& database) {
    const auto logs = database.listLogs();
    if (logs.empty()) {
        std::cout << "PARKING_SESSION is empty.\n";
        return;
    }
    std::cout << "id | zone | car_number | status | parked_at | violation_at | "
                 "departed_at | canceled\n";
    for (const auto& log : logs) {
        std::cout << log.id << " | " << log.zone_id << " | " << log.car_number << " | "
                  << log.status << " | " << log.parked_at << " | "
                  << displayOptional(log.violation_at) << " | "
                  << displayOptional(log.departed_at) << " | "
                  << (log.is_canceled ? "1" : "0") << '\n';
    }
}

/**
 * @brief 테스트 차량 마스터의 차량번호와 종류를 출력한다.
 *
 * @param[in] database 조회할 이벤트 DB.
 * @throws std::runtime_error DB 조회가 실패한 경우.
 */
void printVehicles(const EventDatabase& database) {
    for (const auto& [car_number, vehicle_type] : database.listVehicles()) {
        std::cout << car_number << " | " << vehicle_type << '\n';
    }
}

/**
 * @brief 대화형 시뮬레이터가 지원하는 명령 목록을 출력한다.
 */
void printInteractiveHelp() {
    std::cout
        << "Commands:\n"
        << "  entry <zone_id> <car_number> [image_path]  OFF -> ON entry event\n"
        << "  exit <zone_id>                            ON -> OFF exit event\n"
        << "  wait <seconds>                            keep process alive while timers run\n"
        << "  logs                                      show PARKING_SESSION rows\n"
        << "  vehicles                                  show VEHICLE rows\n"
        << "  status                                    show queued timer count\n"
        << "  help                                      show commands\n"
        << "  quit                                      exit\n";
}

/**
 * @brief 표준 입력에서 홀센서/번호판 이벤트를 흉내 내는 REPL을 실행한다.
 *
 * @param[in,out] slots 입차·출차 명령을 전달할 주차구역 관리자.
 * @param[in] database 로그와 차량 마스터를 출력할 DB.
 * @note 개별 명령의 예외는 오류 메시지로 바꾸고 다음 명령을 계속 받는다. EOF 또는
 *       `quit` 명령이 들어오면 반환한다.
 */
void runInteractive(ParkingSlotManager& slots, EventDatabase& database) {
    printInteractiveHelp();
    std::string line;
    while (std::cout << "timer> " && std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) {
            continue;
        }
        // 잘못된 한 명령이 전체 타이머 프로세스를 종료시키지 않도록 명령 단위로 예외를 잡는다.
        try {
            if (command == "entry" || command == "arrive") {
                int zone_id{};
                std::string car_number;
                std::string image_path;
                if (!(input >> zone_id >> car_number)) {
                    std::cout << "usage: entry <zone_id> <car_number> [image_path]\n";
                    continue;
                }
                input >> image_path;
                const auto result = slots.handleEntry(zone_id, car_number, image_path);
                std::cout << "entry: " << result.message << " ("
                          << parking_timer::toString(result.category) << ")";
                if (result.log_id.has_value()) {
                    std::cout << ", log_id=" << *result.log_id;
                }
                std::cout << '\n';
            } else if (command == "exit" || command == "leave") {
                int zone_id{};
                if (!(input >> zone_id)) {
                    std::cout << "usage: exit <zone_id>\n";
                    continue;
                }
                slots.handleExit(zone_id);
            } else if (command == "wait") {
                std::string seconds_text;
                if (!(input >> seconds_text)) {
                    std::cout << "usage: wait <seconds>\n";
                    continue;
                }
                const auto seconds = parsePositiveInteger(seconds_text, "wait");
                std::this_thread::sleep_for(std::chrono::seconds{seconds});
            } else if (command == "logs") {
                printLogs(database);
            } else if (command == "vehicles") {
                printVehicles(database);
            } else if (command == "status") {
                std::cout << "queued timers (including lazy-canceled nodes): "
                          << slots.pendingTimerCount() << '\n';
            } else if (command == "help") {
                printInteractiveHelp();
            } else if (command == "quit" || command == "q" || command == "exit-program") {
                break;
            } else {
                std::cout << "unknown command; type 'help'\n";
            }
        } catch (const std::exception& error) {
            std::cout << "command failed: " << error.what() << '\n';
        }
    }
}

/**
 * @brief EV 만료와 PHEV 조기 출차를 함께 보여주는 자동 시나리오를 실행한다.
 *
 * @param[in,out] slots 입차·출차와 타이머를 처리할 주차구역 관리자.
 * @param[in] database 마지막 상태를 출력할 DB.
 * @param[in] timeout 데모에서 사용할 장기점유 제한시간.
 * @throws std::invalid_argument 데모 입력 또는 timeout이 유효하지 않은 경우.
 * @throws std::runtime_error DB나 타이머 작업이 실패한 경우.
 */
void runDemo(ParkingSlotManager& slots,
             EventDatabase& database,
             const std::chrono::milliseconds timeout) {
    std::cout << "[demo] EV enters zone 1; PHEV enters zone 2.\n";
    slots.handleEntry(1, "123가4567", "snapshots/demo_ev_parked.jpg");
    slots.handleEntry(2, "234나5678", "snapshots/demo_phev_parked.jpg");

    std::cout << "[demo] A gasoline vehicle is classified and rejected from the timer.\n";
    slots.handleEntry(3, "345다6789");

    // PHEV는 전체 제한시간의 1/4 지점에 출차시켜 lazy cancellation을 눈으로 확인한다.
    const auto early_exit_delay = std::max(std::chrono::milliseconds{50}, timeout / 4);
    std::this_thread::sleep_for(early_exit_delay);
    std::cout << "[demo] PHEV exits zone 2 before its deadline.\n";
    slots.handleExit(2);

    std::cout << "[demo] Waiting for the zone 1 EV deadline...\n";
    std::this_thread::sleep_for(timeout - early_exit_delay + std::chrono::milliseconds{200});
    slots.handleExit(1);

    std::cout << "[demo] Final database rows:\n";
    printLogs(database);
}

}  // namespace

/**
 * @brief 설정·SQLite·타이머 관리자를 초기화하고 자동 데모 또는 REPL을 실행한다.
 *
 * @param[in] argc 명령행 인자 개수.
 * @param[in] argv 명령행 인자 배열.
 * @return 정상 종료와 도움말 출력은 0, 처리된 치명적 오류는 1.
 */
int main(int argc, char* argv[]) {
    try {
        const auto options = parseArguments(argc, argv);
        if (options.help) {
            printUsage(argv[0]);
            return 0;
        }

        const auto executable_directory = executableDirectory(argv[0]);
        const auto source_directory = std::filesystem::path{PARKING_TIMER_SOURCE_DIR};
        // 리소스 우선순위: CLI 명시 → 실행 파일 옆 → 현재 디렉터리 → 빌드 시 소스 경로.
        const auto config_file = findResource(
            options.config_file,
            {executable_directory / "config/timer.conf",
             std::filesystem::current_path() / "config/timer.conf",
             source_directory / "config/timer.conf"},
            "timer configuration");
        const auto sql_directory = findResource(
            options.sql_directory,
            {executable_directory / "db", std::filesystem::current_path() / "db",
             source_directory / "db"},
            "SQL directory");

        // 설정 우선순위: 구조체 기본값 < 설정 파일 < 환경변수 < CLI 옵션.
        auto config = parking_timer::RuntimeConfig::load(config_file);
        config.applyEnvironment();
        if (options.database_path.has_value()) {
            config.database_path = *options.database_path;
        }
        if (options.timeout.has_value()) {
            config.parking_timeout = *options.timeout;
        }

        // schema/seed 초기화가 끝난 뒤 worker를 만들어 만료 callback이 미완성 DB를 보지 않게 한다.
        EventDatabase database(config.database_path);
        database.initialize(sql_directory / "schema.sql", sql_directory / "seed.sql");
        if (options.reset_logs) {
            database.clearTimerLogs();
        }

        parking_timer::EventManager events;
        ParkingSlotManager slots(database, events, config.parking_timeout);
        std::cout << "database=" << config.database_path << ", timeout="
                  << std::chrono::duration_cast<std::chrono::seconds>(
                         config.parking_timeout)
                         .count()
                  << "s\n";

        if (options.demo) {
            runDemo(slots, database, config.parking_timeout);
        } else {
            runInteractive(slots, database);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}

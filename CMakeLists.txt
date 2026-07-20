cmake_minimum_required(VERSION 3.16)
project(pi_server LANGUAGES C CXX)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
find_package(Threads REQUIRED)

file(GLOB_RECURSE SERVER_CPP CONFIGURE_DEPENDS src/*.cpp)
# 타이머는 별도 타깃(parking-timer-core / parking-timer)으로 빌드하므로 제외.
list(FILTER SERVER_CPP EXCLUDE REGEX "src/timer/")
add_executable(pi-server ${SERVER_CPP} src/database/db_manager.c)
target_include_directories(pi-server PRIVATE include)
target_compile_options(pi-server PRIVATE -Wall -Wextra)
target_link_libraries(pi-server PRIVATE
    PkgConfig::OPENCV PkgConfig::AV PkgConfig::CURL
    PkgConfig::HTTPLIB mosquitto sqlite3 Threads::Threads)

add_executable(gemini-ocr-test
    tests/integration/GeminiOcrTest.cpp
    src/ocr/GeminiOcrClient.cpp
    src/ocr/PlateImageEnhancer.cpp
    src/ocr/PlateNormalizer.cpp)
target_include_directories(gemini-ocr-test PRIVATE include)
target_compile_options(gemini-ocr-test PRIVATE -Wall -Wextra)
target_link_libraries(gemini-ocr-test PRIVATE
    PkgConfig::OPENCV PkgConfig::CURL Threads::Threads)

add_executable(db-manager-test
    tests/integration/DbManagerTest.c
    src/database/db_manager.c)
target_include_directories(db-manager-test PRIVATE include)
target_compile_options(db-manager-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(db-manager-test PRIVATE sqlite3)

set(SENSOR_PARKING_SOURCES
    src/sensor/SensorProtocolParser.cpp
    src/sensor/ParkingSensorEventAdapter.cpp
    src/parking/ParkingSlotConfig.cpp
    src/parking/SensorSlotIndex.cpp
    src/parking/ParkingSensorSequenceGuard.cpp
    src/parking/ParkingOccupancySession.cpp
    src/parking/ParkingSlotManager.cpp
    src/parking/ActiveParkingSessionIndex.cpp)

add_executable(parking-domain-test
    tests/unit/ParkingDomainTest.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(parking-domain-test PRIVATE include)
target_compile_options(parking-domain-test PRIVATE -Wall -Wextra)

add_executable(sensor-parking-pipeline-test
    tests/unit/SensorParkingPipelineTest.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(sensor-parking-pipeline-test PRIVATE include)
target_compile_options(sensor-parking-pipeline-test PRIVATE -Wall -Wextra)

add_test(NAME parking-domain-test
    COMMAND parking-domain-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)
add_test(NAME sensor-parking-pipeline-test
    COMMAND sensor-parking-pipeline-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)

# ---- parking timer (timer_ 프로젝트 통합분) ----
add_library(parking-timer-core STATIC
    src/timer/EventDatabase.cpp
    src/timer/EventManager.cpp
    src/timer/ParkingSlotManager.cpp
    src/timer/RuntimeConfig.cpp
    src/timer/TimerManager.cpp
    src/timer/Types.cpp)
target_include_directories(parking-timer-core PUBLIC include)
target_compile_options(parking-timer-core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(parking-timer-core PROPERTIES CXX_STANDARD 20)
target_link_libraries(parking-timer-core PUBLIC sqlite3 Threads::Threads)

add_executable(parking-timer src/timer/main.cpp)
target_link_libraries(parking-timer PRIVATE parking-timer-core)
target_compile_options(parking-timer PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(parking-timer PROPERTIES CXX_STANDARD 20)
target_compile_definitions(parking-timer PRIVATE
    PARKING_TIMER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# 실행/테스트가 빌드 디렉터리에서도 리소스를 찾을 수 있게 복사.
foreach(resource db/schema.sql db/seed.sql config/timer.conf)
    configure_file(${resource} ${resource} COPYONLY)
endforeach()

add_executable(parking-timer-tests tests/timer/timer_tests.cpp)
target_link_libraries(parking-timer-tests PRIVATE parking-timer-core)
target_compile_options(parking-timer-tests PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(parking-timer-tests PROPERTIES CXX_STANDARD 20)
target_compile_definitions(parking-timer-tests PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME parking-timer-tests COMMAND parking-timer-tests)

add_executable(http-api-test
    tests/integration/HttpApiTest.cpp
    src/http/ParkingHttpServer.cpp
    src/database/EventDatabase.cpp
    src/database/db_manager.c
    src/util/Logger.cpp)
target_include_directories(http-api-test PRIVATE include)
target_compile_options(http-api-test PRIVATE -Wall -Wextra)
target_link_libraries(http-api-test PRIVATE
    PkgConfig::HTTPLIB sqlite3 Threads::Threads)
add_test(NAME http-api-test COMMAND http-api-test)

cmake_minimum_required(VERSION 3.16)
project(pi_server LANGUAGES C CXX)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(Threads REQUIRED)

file(GLOB_RECURSE SERVER_CPP CONFIGURE_DEPENDS src/*.cpp)
# 타이머는 별도 타깃(parking-timer-core / parking-timer)으로 빌드하므로 제외.
list(FILTER SERVER_CPP EXCLUDE REGEX "src/timer/")
add_executable(pi-server ${SERVER_CPP} src/database/db_manager.c)
target_include_directories(pi-server PRIVATE include include/uapi)
target_compile_options(pi-server PRIVATE
    -Wall -Wextra -ffunction-sections -fdata-sections)
target_compile_definitions(pi-server PRIVATE
    VEDA_RUNTIME_SQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db")
target_link_libraries(pi-server PRIVATE
    PkgConfig::OPENCV PkgConfig::AV PkgConfig::CURL
    PkgConfig::HTTPLIB PkgConfig::SODIUM OpenSSL::Crypto
    mosquitto sqlite3 Threads::Threads)
target_link_options(pi-server PRIVATE -Wl,--gc-sections)
if(NOT CMAKE_NM)
    message(FATAL_ERROR "nm is required for the production occupancy gate")
endif()
add_custom_command(TARGET pi-server POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DNM_TOOL=${CMAKE_NM}
        -DBINARY_PATH=$<TARGET_FILE:pi-server>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssertNoLegacyOccupancySymbols.cmake
    VERBATIM)

add_executable(gemini-ocr-test
    tests/integration/GeminiOcrTest.cpp
    src/ocr/GeminiOcrClient.cpp
    src/ocr/PlateImageEnhancer.cpp
    src/ocr/PlateNormalizer.cpp)
target_include_directories(gemini-ocr-test PRIVATE include)
target_compile_options(gemini-ocr-test PRIVATE -Wall -Wextra)
target_link_libraries(gemini-ocr-test PRIVATE
    PkgConfig::OPENCV PkgConfig::CURL Threads::Threads)

# 외부 Gemini 호출 없이 HTTP 상태/JSON/MIME/fallback 계약을 검증한다.
add_executable(gemini-ocr-client-test
    tests/unit/GeminiOcrClientTest.cpp
    src/ocr/GeminiOcrClient.cpp)
target_include_directories(gemini-ocr-client-test PRIVATE include)
target_compile_options(gemini-ocr-client-test PRIVATE -Wall -Wextra)
target_link_libraries(gemini-ocr-client-test PRIVATE PkgConfig::CURL)
add_test(NAME gemini-ocr-client-test COMMAND gemini-ocr-client-test)

add_executable(app-config-test
    tests/unit/AppConfigTest.cpp
    src/app/AppConfig.cpp)
target_include_directories(app-config-test PRIVATE include)
target_compile_options(app-config-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME app-config-test COMMAND app-config-test)

add_executable(entrance-bestshot-coordinator-test
    tests/unit/EntranceBestShotCoordinatorTest.cpp
    src/entrance/EntranceBestShotCoordinator.cpp)
target_include_directories(entrance-bestshot-coordinator-test PRIVATE include)
target_compile_options(entrance-bestshot-coordinator-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(entrance-bestshot-coordinator-test PRIVATE Threads::Threads)
add_test(NAME entrance-bestshot-coordinator-test
    COMMAND entrance-bestshot-coordinator-test)

add_executable(entrance-image-deduplicator-test
    tests/unit/EntranceImageDeduplicatorTest.cpp
    src/entrance/EntranceImageDeduplicator.cpp)
target_include_directories(entrance-image-deduplicator-test PRIVATE include)
target_compile_options(entrance-image-deduplicator-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(entrance-image-deduplicator-test PRIVATE
    PkgConfig::OPENCV Threads::Threads)
add_test(NAME entrance-image-deduplicator-test
    COMMAND entrance-image-deduplicator-test)

add_executable(plate-matcher-test
    tests/unit/PlateMatcherTest.cpp
    src/ocr/PlateMatcher.cpp)
target_include_directories(plate-matcher-test PRIVATE include)
target_compile_options(plate-matcher-test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME plate-matcher-test COMMAND plate-matcher-test)

add_executable(entrance-vehicle-service-test
    tests/integration/EntranceVehicleServiceTest.cpp
    src/entrance/EntranceBestShotCoordinator.cpp
    src/entrance/EntranceImageDeduplicator.cpp
    src/entrance/EntranceVehicleService.cpp
    src/util/Logger.cpp)
target_include_directories(entrance-vehicle-service-test PRIVATE include)
target_compile_options(entrance-vehicle-service-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(entrance-vehicle-service-test PRIVATE
    PkgConfig::OPENCV Threads::Threads)
add_test(NAME entrance-vehicle-service-test
    COMMAND entrance-vehicle-service-test)

add_executable(entrance-database-test
    tests/integration/EntranceDatabaseTest.cpp)
target_include_directories(entrance-database-test PRIVATE include)
target_compile_options(entrance-database-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_definitions(entrance-database-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
target_link_libraries(entrance-database-test PRIVATE
    parking-timer-core sqlite3 Threads::Threads)
add_test(NAME entrance-database-test COMMAND entrance-database-test)

add_executable(entrance-ocr-worker-test
    tests/integration/EntranceOcrWorkerTest.cpp
    src/ocr/OcrWorker.cpp
    src/ocr/GeminiOcrClient.cpp
    src/ocr/PlateImageEnhancer.cpp
    src/ocr/PlateNormalizer.cpp
    src/util/TimeUtil.cpp)
target_include_directories(entrance-ocr-worker-test PRIVATE include)
target_compile_options(entrance-ocr-worker-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(entrance-ocr-worker-test PRIVATE
    parking-timer-core PkgConfig::OPENCV PkgConfig::CURL Threads::Threads)
add_test(NAME entrance-ocr-worker-test COMMAND entrance-ocr-worker-test)

add_executable(entrance-ev-worker-test
    tests/integration/EntranceEvWorkerTest.cpp
    src/entrance/EntranceEvWorker.cpp
    src/util/Logger.cpp)
target_include_directories(entrance-ev-worker-test PRIVATE include)
target_compile_options(entrance-ev-worker-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(entrance-ev-worker-test PRIVATE Threads::Threads)
add_test(NAME entrance-ev-worker-test COMMAND entrance-ev-worker-test)

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
    src/parking/ParkingOccupancyConfirmationGate.cpp
    src/parking/ParkingOccupancySession.cpp
    src/parking/ParkingSlotManager.cpp
    src/parking/ParkingSessionWorker.cpp
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

add_executable(parking-session-worker-test
    tests/unit/ParkingSessionWorkerTest.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(parking-session-worker-test PRIVATE include)
target_compile_options(parking-session-worker-test PRIVATE -Wall -Wextra)

# T0+30s/T0+60s 촬영 스케줄러 정책 단위 테스트 (EVDA-135). 순수 로직만 링크하며
# 스레드 런타임(CaptureSchedulerRuntime)은 pi-server 타깃에서만 빌드한다.
add_executable(capture-scheduler-test
    tests/unit/CaptureSchedulerTest.cpp
    src/parking/CaptureScheduler.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(capture-scheduler-test PRIVATE include)
target_compile_options(capture-scheduler-test PRIVATE -Wall -Wextra)

# OCCUPIED 확정(T0) 유예시간 정책 단위 테스트 (EVDA-135 파생 결정).
add_executable(parking-occupancy-confirmation-gate-test
    tests/unit/ParkingOccupancyConfirmationGateTest.cpp
    src/parking/ParkingOccupancyConfirmationGate.cpp)
target_include_directories(parking-occupancy-confirmation-gate-test PRIVATE include)
target_compile_options(parking-occupancy-confirmation-gate-test PRIVATE -Wall -Wextra)

add_executable(hall-ocr-policy-test
    tests/unit/HallOcrPolicyTest.cpp
    src/parking/HallOcrPolicy.cpp)
target_include_directories(hall-ocr-policy-test PRIVATE include)
target_compile_options(hall-ocr-policy-test PRIVATE -Wall -Wextra)
add_test(NAME hall-ocr-policy-test COMMAND hall-ocr-policy-test)

# 야간 번호판 조명 LED 점등 정책 단위 테스트. UART/LoRa 전송은 주입한 sender로
# 대체하므로 실제 링크 없이 검증한다.
add_executable(plate-illuminator-test
    tests/unit/PlateIlluminatorTest.cpp
    src/parking/PlateIlluminator.cpp
    src/util/Logger.cpp)
target_include_directories(plate-illuminator-test PRIVATE include)
target_compile_options(plate-illuminator-test PRIVATE -Wall -Wextra)
target_link_libraries(plate-illuminator-test PRIVATE Threads::Threads)
add_test(NAME plate-illuminator-test COMMAND plate-illuminator-test)

add_executable(hall-capture-coordinator-test
    tests/unit/HallCaptureCoordinatorTest.cpp
    src/parking/HallCaptureCoordinator.cpp
    src/parking/HallOcrPolicy.cpp
    src/util/Logger.cpp)
target_include_directories(hall-capture-coordinator-test PRIVATE include)
target_compile_options(hall-capture-coordinator-test PRIVATE -Wall -Wextra)
target_link_libraries(hall-capture-coordinator-test PRIVATE Threads::Threads)
add_test(NAME hall-capture-coordinator-test
    COMMAND hall-capture-coordinator-test)

add_executable(camera-iva-event-test
    tests/unit/CameraIvaEventTest.cpp
    src/event/CameraEventParser.cpp
    src/event/IvaEventResolver.cpp
    src/event/IvaSlotOccupancyAggregator.cpp
    src/event/IvaOccupancyCoordinator.cpp
    src/util/TimeUtil.cpp)
target_include_directories(camera-iva-event-test PRIVATE include)
target_compile_options(camera-iva-event-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME camera-iva-event-test COMMAND camera-iva-event-test)

# ONVIF PullPoint XML 분리와 CH1/CH3 슬롯 매핑을 카메라 없이 검증한다.
add_executable(onvif-iva-event-source-test
    tests/unit/OnvifIvaEventSourceTest.cpp
    src/camera/OnvifIvaEventSource.cpp
    src/event/OnvifIvaEventAdapter.cpp
    src/event/IvaEventResolver.cpp
    src/util/TimeUtil.cpp
    src/util/Logger.cpp)
target_include_directories(onvif-iva-event-source-test PRIVATE include)
target_compile_options(onvif-iva-event-source-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(onvif-iva-event-source-test PRIVATE
    PkgConfig::CURL OpenSSL::Crypto Threads::Threads)
add_test(NAME onvif-iva-event-source-test
    COMMAND onvif-iva-event-source-test)

add_executable(fire-alarm-test
    tests/unit/FireAlarmTest.cpp
    src/event/FireAlarmService.cpp
    src/event/FirePersistence.cpp
    src/event/EventPayloadBuilder.cpp
    src/sensor/SensorProtocolParser.cpp
    src/util/Logger.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp)
target_include_directories(fire-alarm-test PRIVATE include)
target_compile_options(fire-alarm-test PRIVATE -Wall -Wextra)
target_link_libraries(fire-alarm-test PRIVATE parking-timer-core)
add_test(NAME fire-alarm-test COMMAND fire-alarm-test)

add_executable(fire-delivery-integration-test
    tests/integration/FireDeliveryIntegrationTest.cpp
    src/event/FireAlarmService.cpp
    src/event/FireDeliveryCoordinator.cpp
    src/event/EventPayloadBuilder.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp)
target_include_directories(fire-delivery-integration-test PRIVATE include)
target_compile_options(fire-delivery-integration-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(fire-delivery-integration-test PRIVATE
    parking-timer-core Threads::Threads)
add_test(NAME fire-delivery-integration-test
    COMMAND fire-delivery-integration-test)

add_executable(fire-broker-restart-process-test
    tests/integration/FireBrokerRestartProcessTest.cpp
    src/event/FireAlarmService.cpp
    src/event/FireDeliveryCoordinator.cpp
    src/event/EventPayloadBuilder.cpp
    src/mqtt/MqttEndpoint.cpp
    src/mqtt/MosquittoTransport.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp)
target_include_directories(fire-broker-restart-process-test PRIVATE include)
target_compile_options(fire-broker-restart-process-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(fire-broker-restart-process-test PRIVATE
    parking-timer-core mosquitto Threads::Threads)
add_test(NAME fire-broker-restart-process-test
    COMMAND fire-broker-restart-process-test)

add_executable(mqtt-tracked-transport-test
    tests/unit/MqttTrackedTransportTest.cpp
    src/mqtt/MqttEndpoint.cpp)
target_include_directories(mqtt-tracked-transport-test PRIVATE include)
target_compile_options(mqtt-tracked-transport-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(mqtt-tracked-transport-test PRIVATE Threads::Threads)
add_test(NAME mqtt-tracked-transport-test
    COMMAND mqtt-tracked-transport-test)

# STM32 UART 통신만 단독으로 확인하기 위한 경량 실행 파일.
# 카메라/MQTT/DB 없이 운영 main과 동일한 device::SensorLinkManager를 사용한다.
add_executable(uart-fire-listener
    tests/integration/UartFireListenerTool.cpp
    src/device/UartDriver.cpp
    src/device/LoRaDriver.cpp
    src/device/SensorLinkManager.cpp
    src/event/SystemEventReporter.cpp
    src/sensor/SensorProtocolParser.cpp
    src/sensor/FireSensorMessage.cpp
    src/util/StringUtil.cpp
    src/util/Logger.cpp)
target_include_directories(uart-fire-listener PRIVATE include)
target_compile_options(uart-fire-listener PRIVATE -Wall -Wextra)
target_link_libraries(uart-fire-listener PRIVATE Threads::Threads util)

add_test(NAME parking-domain-test
    COMMAND parking-domain-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)
add_test(NAME parking-production-config-test
    COMMAND parking-domain-test
            ${CMAKE_SOURCE_DIR}/config/parking_slots.json
            --validate-production)
add_test(NAME sensor-parking-pipeline-test
    COMMAND sensor-parking-pipeline-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)
add_test(NAME parking-session-worker-test
    COMMAND parking-session-worker-test)
add_test(NAME capture-scheduler-test
    COMMAND capture-scheduler-test)
add_test(NAME parking-occupancy-confirmation-gate-test
    COMMAND parking-occupancy-confirmation-gate-test)

add_executable(uart-lora-driver-test
    tests/unit/UartLoRaDriverTest.cpp
    src/device/UartDriver.cpp
    src/device/LoRaDriver.cpp
    src/device/SensorLinkManager.cpp
    src/sensor/SensorProtocolParser.cpp
    src/event/SystemEventReporter.cpp
    src/util/StringUtil.cpp
    src/util/Logger.cpp)
target_include_directories(uart-lora-driver-test PRIVATE include)
target_compile_options(uart-lora-driver-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(uart-lora-driver-test PRIVATE Threads::Threads util)
add_test(NAME uart-lora-driver-test COMMAND uart-lora-driver-test)

# HIL 원시 결과를 CSV로 보존하고 Confluence/포트폴리오용 Markdown 통계를 만든다.
add_library(hardware-test-report STATIC
    tests/hardware/HardwareTestReport.cpp)
target_include_directories(hardware-test-report PUBLIC tests/hardware)
target_compile_options(hardware-test-report PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(hardware-test-report-test
    tests/unit/HardwareTestReportTest.cpp)
target_include_directories(hardware-test-report-test PRIVATE tests/hardware)
target_compile_options(hardware-test-report-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(hardware-test-report-test PRIVATE hardware-test-report)
add_test(NAME hardware-test-report-test COMMAND hardware-test-report-test)

# 실제 STM32/Hall/Flame 장치를 사람이 조작하며 UART/LoRa 상태 전이와
# 지연시간을 판정하는 반자동 HIL 테스트다. 일반 CTest에서는 77로 SKIP하며
# RUN_HARDWARE_TESTS=1을 명시했을 때만 물리 UART를 연다.
add_executable(sensor-link-hardware-test
    tests/hardware/SensorLinkHardwareAcceptanceTest.cpp
    src/device/UartDriver.cpp
    src/device/LoRaDriver.cpp
    src/device/SensorLinkManager.cpp
    src/sensor/SensorProtocolParser.cpp
    src/sensor/FireSensorMessage.cpp
    src/event/SystemEventReporter.cpp
    src/util/StringUtil.cpp
    src/util/Logger.cpp)
target_include_directories(sensor-link-hardware-test PRIVATE include)
target_compile_options(sensor-link-hardware-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(sensor-link-hardware-test PRIVATE Threads::Threads util)
target_link_libraries(sensor-link-hardware-test PRIVATE hardware-test-report)

find_package(Git QUIET)
set(VEDA_HARDWARE_TEST_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE VEDA_GIT_REV_PARSE_RESULT
        OUTPUT_VARIABLE VEDA_HARDWARE_TEST_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT VEDA_GIT_REV_PARSE_RESULT EQUAL 0)
        set(VEDA_HARDWARE_TEST_GIT_COMMIT "unknown")
    endif()
endif()
target_compile_definitions(sensor-link-hardware-test PRIVATE
    VEDA_HARDWARE_TEST_GIT_COMMIT="${VEDA_HARDWARE_TEST_GIT_COMMIT}")
add_test(NAME sensor-link-hardware-test COMMAND sensor-link-hardware-test)
set_tests_properties(sensor-link-hardware-test PROPERTIES
    LABELS hardware
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

add_executable(system-event-reporter-test
    tests/unit/SystemEventReporterTest.cpp
    src/event/SystemEventReporter.cpp
    src/util/Logger.cpp
    src/util/StringUtil.cpp)
target_include_directories(system-event-reporter-test PRIVATE include)
target_compile_options(system-event-reporter-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(system-event-reporter-test PRIVATE Threads::Threads)
add_test(NAME system-event-reporter-test COMMAND system-event-reporter-test)

add_executable(parking-link-tool
    tools/parking_link_tool.cpp
    src/device/UartDriver.cpp
    src/device/LoRaDriver.cpp)
target_include_directories(parking-link-tool PRIVATE include)
target_compile_options(parking-link-tool PRIVATE -Wall -Wextra -Wpedantic)

# 운영 드라이버로 LoRa v1.0/v1.1 수신, CRC/sequence 통계 및 LED 명령을
# 대화형으로 확인하는 headless 진단 콘솔이다.
add_executable(lora-console
    tools/lora_console.cpp
    src/device/UartDriver.cpp
    src/device/LoRaDriver.cpp
    src/sensor/SensorProtocolParser.cpp
    src/sensor/FireSensorMessage.cpp)
target_include_directories(lora-console PRIVATE include)
target_compile_options(lora-console PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lora-console PRIVATE Threads::Threads)

# 카메라 ONVIF PullPoint에서 WiseAI IvaArea의 ObjectId/Action을 직접 확인한다.
# MQTT Publication/Event Rule과 무관한 카메라 원본 이벤트 진단 전용 도구다.
add_executable(watch-onvif-iva-events
    tools/watch_onvif_iva_events.cpp
    src/camera/OnvifIvaEventSource.cpp
    src/util/Logger.cpp)
target_include_directories(watch-onvif-iva-events PRIVATE include)
target_compile_options(watch-onvif-iva-events PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(watch-onvif-iva-events PRIVATE
    PkgConfig::CURL OpenSSL::Crypto Threads::Threads)

# /dev/parking_alert 문자 디바이스 제어 도구. 커널 모듈은 명시적 타깃으로만 빌드한다.
add_executable(parking-alert-ctl tools/parking_alert_ctl.c)
target_include_directories(parking-alert-ctl PRIVATE include/uapi)
target_compile_options(parking-alert-ctl PRIVATE -Wall -Wextra -Wpedantic)

add_library(parking-alert-adapter STATIC src/device/LinuxDriverAdapter.cpp)
target_include_directories(parking-alert-adapter PUBLIC include include/uapi)
target_compile_options(parking-alert-adapter PRIVATE -Wall -Wextra -Wpedantic)

add_executable(parking-alert-controller-test
    tests/unit/ParkingAlertControllerTest.cpp
    src/device/ParkingAlertController.cpp)
target_include_directories(parking-alert-controller-test PRIVATE include)
target_compile_options(parking-alert-controller-test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME parking-alert-controller-test COMMAND parking-alert-controller-test)

add_custom_target(parking-alert-module
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${CMAKE_SOURCE_DIR}/driver/parking_alert
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building /dev/parking_alert Linux kernel module")

# ---- parking timer (timer_ 프로젝트 통합분) ----
add_library(parking-timer-core STATIC
    src/database/EventDatabase.cpp
    src/database/EventDatabaseAuth.cpp
    src/database/EventDatabaseCorrelation.cpp
    src/database/EventDatabaseEntrance.cpp
    src/database/EventDatabaseFire.cpp
    src/database/EventDatabaseOccupancy.cpp
    src/database/EventDatabaseTimer.cpp
    src/database/SessionTransitionStore.cpp
    src/database/db_manager.c
    src/ocr/PlateMatcher.cpp
    src/event/FirePersistence.cpp
    src/parking/SlotTransitionActor.cpp
    src/parking/SlotTransitionTypes.cpp
    src/util/Logger.cpp
    src/timer/EventManager.cpp
    src/timer/ParkingSlotManager.cpp
    src/timer/RuntimeConfig.cpp
    src/timer/TimerManager.cpp
    src/timer/Types.cpp)
target_include_directories(parking-timer-core PUBLIC include)
target_compile_options(parking-timer-core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -ffunction-sections -fdata-sections)
target_link_libraries(parking-timer-core PUBLIC sqlite3 Threads::Threads)

add_executable(app-user
    tools/app_user.cpp
    src/auth/AuthService.cpp)
target_include_directories(app-user PRIVATE include)
target_compile_options(app-user PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(app-user PRIVATE
    parking-timer-core PkgConfig::SODIUM sqlite3 Threads::Threads)

# 메인 서버에서도 동일한 타이머 코어를 사용한다. parking-timer 독립 실행 파일도 유지한다.
target_link_libraries(pi-server PRIVATE parking-timer-core)

option(VEDA_BUILD_LEGACY_PARKING_TIMER_CLI
    "Build the development-only legacy parking mutation CLI" OFF)
if(VEDA_BUILD_LEGACY_PARKING_TIMER_CLI)
    add_executable(parking-timer EXCLUDE_FROM_ALL src/timer/main.cpp)
    target_link_libraries(parking-timer PRIVATE parking-timer-core)
    target_compile_options(parking-timer PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_definitions(parking-timer PRIVATE
        PARKING_TIMER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# 실행/테스트가 빌드 디렉터리에서도 리소스를 찾을 수 있게 복사.
foreach(resource db/schema.sql db/seed.sql config/timer.conf)
    configure_file(${resource} ${resource} COPYONLY)
endforeach()

add_executable(parking-timer-tests tests/timer/timer_tests.cpp)
target_link_libraries(parking-timer-tests PRIVATE parking-timer-core)
target_compile_options(parking-timer-tests PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_compile_definitions(parking-timer-tests PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME parking-timer-tests COMMAND parking-timer-tests)

add_executable(slot-transition-actor-test
    tests/integration/SlotTransitionActorTest.cpp)
target_link_libraries(slot-transition-actor-test PRIVATE parking-timer-core)
target_compile_options(slot-transition-actor-test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_compile_definitions(slot-transition-actor-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME slot-transition-actor-test COMMAND slot-transition-actor-test)

# 카메라/UART 없이 홀센서 입력부터 OCR 대역과 장기점유 타이머까지 잇는 통합 테스트.
add_executable(hall-timer-integration-test
    tests/integration/HallTimerIntegrationTest.cpp
    src/sensor/HallParkingService.cpp
    src/event/IvaSlotOccupancyAggregator.cpp
    src/event/IvaOccupancyCoordinator.cpp
    src/parking/EvidenceCaptureWorker.cpp
    src/snapshot/SnapshotStorage.cpp
    src/event/SystemEventReporter.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(hall-timer-integration-test PRIVATE include)
target_compile_options(hall-timer-integration-test PRIVATE -Wall -Wextra)
target_link_libraries(hall-timer-integration-test PRIVATE
    parking-timer-core PkgConfig::OPENCV)
target_compile_definitions(hall-timer-integration-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME hall-timer-integration-test
    COMMAND hall-timer-integration-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)

add_executable(camera-iva-occupancy-integration-test
    tests/integration/CameraIvaOccupancyIntegrationTest.cpp
    src/sensor/HallParkingService.cpp
    src/event/IvaSlotOccupancyAggregator.cpp
    src/event/IvaOccupancyCoordinator.cpp
    src/parking/EvidenceCaptureWorker.cpp
    src/snapshot/SnapshotStorage.cpp
    src/event/SystemEventReporter.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp
    ${SENSOR_PARKING_SOURCES})
target_include_directories(camera-iva-occupancy-integration-test PRIVATE include)
target_compile_options(camera-iva-occupancy-integration-test PRIVATE
    -Wall -Wextra)
target_link_libraries(camera-iva-occupancy-integration-test PRIVATE
    parking-timer-core PkgConfig::OPENCV)
target_compile_definitions(camera-iva-occupancy-integration-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME camera-iva-occupancy-integration-test
    COMMAND camera-iva-occupancy-integration-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)

add_executable(evidence-capture-worker-test
    tests/integration/EvidenceCaptureWorkerTest.cpp
    src/parking/EvidenceCaptureWorker.cpp
    src/snapshot/SnapshotStorage.cpp
    src/util/TimeUtil.cpp)
target_include_directories(evidence-capture-worker-test PRIVATE include)
target_compile_options(evidence-capture-worker-test PRIVATE -Wall -Wextra)
target_link_libraries(evidence-capture-worker-test PRIVATE
    parking-timer-core PkgConfig::OPENCV Threads::Threads)
target_compile_definitions(evidence-capture-worker-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME evidence-capture-worker-test COMMAND evidence-capture-worker-test)

add_executable(hall-capture-pipeline-test
    tests/integration/HallCapturePipelineTest.cpp
    src/camera/CameraSnapshotApiClient.cpp
    src/parking/HallCaptureExecutor.cpp
    src/parking/HallCaptureCoordinator.cpp
    src/parking/HallOcrPolicy.cpp
    src/parking/PlateIlluminator.cpp
    src/snapshot/SnapshotStorage.cpp
    src/util/TimeUtil.cpp)
target_include_directories(hall-capture-pipeline-test PRIVATE include)
target_compile_options(hall-capture-pipeline-test PRIVATE -Wall -Wextra)
target_link_libraries(hall-capture-pipeline-test PRIVATE
    parking-timer-core PkgConfig::OPENCV PkgConfig::CURL Threads::Threads)
target_compile_definitions(hall-capture-pipeline-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME hall-capture-pipeline-test COMMAND hall-capture-pipeline-test)

add_executable(camera-snapshot-api-client-test
    tests/integration/CameraSnapshotApiClientTest.cpp
    src/camera/CameraSnapshotApiClient.cpp
    src/util/Logger.cpp)
target_include_directories(camera-snapshot-api-client-test PRIVATE include)
target_compile_options(camera-snapshot-api-client-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(camera-snapshot-api-client-test PRIVATE
    PkgConfig::CURL PkgConfig::HTTPLIB Threads::Threads)
add_test(NAME camera-snapshot-api-client-test
    COMMAND camera-snapshot-api-client-test)

add_executable(http-api-test
    tests/integration/HttpApiTest.cpp
    src/app/RuntimeShutdown.cpp
    src/auth/AuthService.cpp
    src/http/ParkingHttpServer.cpp
    src/mqtt/MqttEndpoint.cpp
    src/settings/OverstayThresholdService.cpp
    src/settings/ParkingRoiSettingsService.cpp)
target_include_directories(http-api-test PRIVATE include)
target_compile_options(http-api-test PRIVATE -Wall -Wextra)
target_link_libraries(http-api-test PRIVATE
    parking-timer-core PkgConfig::HTTPLIB PkgConfig::SODIUM
    sqlite3 Threads::Threads)
add_test(NAME http-api-test COMMAND http-api-test)

add_executable(auth-service-test
    tests/integration/AuthServiceTest.cpp
    src/auth/AuthService.cpp)
target_include_directories(auth-service-test PRIVATE include)
target_compile_options(auth-service-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(auth-service-test PRIVATE
    parking-timer-core PkgConfig::SODIUM sqlite3 Threads::Threads)
add_test(NAME auth-service-test COMMAND auth-service-test)

add_executable(callback-lifecycle-test
    tests/unit/CallbackLifecycleTest.cpp
    src/app/RuntimeShutdown.cpp
    src/mqtt/MqttEndpoint.cpp
    src/util/Logger.cpp)
target_include_directories(callback-lifecycle-test PRIVATE include)
target_compile_options(callback-lifecycle-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(callback-lifecycle-test PRIVATE Threads::Threads)
add_test(NAME callback-lifecycle-test COMMAND callback-lifecycle-test)

add_executable(telegram-message-formatter-test
    tests/unit/TelegramMessageFormatterTest.cpp
    src/notification/TelegramMessageFormatter.cpp)
target_include_directories(telegram-message-formatter-test PRIVATE include)
target_compile_options(telegram-message-formatter-test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME telegram-message-formatter-test
    COMMAND telegram-message-formatter-test)

# 실제 MqttEventBridge 조립 경로에서 target-before-start와 stop/join 이후
# callback 차단을 검증한다. main만 제외하고 운영 source를 그대로 링크한다.
set(MQTT_BRIDGE_LIFECYCLE_SOURCES ${SERVER_CPP})
list(FILTER MQTT_BRIDGE_LIFECYCLE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
add_executable(mqtt-bridge-lifecycle-test
    tests/integration/MqttBridgeLifecycleTest.cpp
    ${MQTT_BRIDGE_LIFECYCLE_SOURCES}
    src/database/db_manager.c)
target_include_directories(mqtt-bridge-lifecycle-test PRIVATE include include/uapi)
target_compile_options(mqtt-bridge-lifecycle-test PRIVATE -Wall -Wextra)
target_compile_definitions(mqtt-bridge-lifecycle-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
target_link_libraries(mqtt-bridge-lifecycle-test PRIVATE
    parking-timer-core PkgConfig::OPENCV PkgConfig::AV PkgConfig::CURL
    PkgConfig::HTTPLIB PkgConfig::SODIUM OpenSSL::Crypto
    mosquitto sqlite3 Threads::Threads)
add_test(NAME mqtt-bridge-lifecycle-test COMMAND mqtt-bridge-lifecycle-test)

add_executable(parking-correlation-integration-test
    tests/integration/ParkingCorrelationIntegrationTest.cpp
    src/parking/ParkingTriggerCoordinator.cpp)
target_include_directories(parking-correlation-integration-test PRIVATE include)
target_compile_options(parking-correlation-integration-test PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_definitions(parking-correlation-integration-test PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
target_link_libraries(parking-correlation-integration-test PRIVATE
    parking-timer-core sqlite3 Threads::Threads)
add_test(NAME parking-correlation-integration-test
    COMMAND parking-correlation-integration-test)

add_executable(code-doc-generator EXCLUDE_FROM_ALL
    tools/code_doc_generator.cpp)
target_compile_features(code-doc-generator PRIVATE cxx_std_20)
target_compile_options(code-doc-generator PRIVATE -Wall -Wextra -Wpedantic)

# 아키텍처 Markdown과 C/C++ 파일·함수 목록을 합쳐 Markdown/PDF를 생성한다.
add_custom_target(docs
    COMMAND ${CMAKE_SOURCE_DIR}/tools/build_docs.sh
            $<TARGET_FILE:code-doc-generator>
    DEPENDS code-doc-generator
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating Pi Server Markdown and PDF code guide")

cmake_minimum_required(VERSION 3.16)
project(pi_server LANGUAGES C CXX)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
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
target_include_directories(pi-server PRIVATE include include/uapi)
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
    src/util/TimeUtil.cpp)
target_include_directories(camera-iva-event-test PRIVATE include)
target_compile_options(camera-iva-event-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME camera-iva-event-test COMMAND camera-iva-event-test)

add_executable(fire-alarm-test
    tests/unit/FireAlarmTest.cpp
    src/event/FireAlarmManager.cpp
    src/event/EventPayloadBuilder.cpp
    src/sensor/FireSensorMessage.cpp
    src/sensor/SensorProtocolParser.cpp
    src/util/Logger.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp)
target_include_directories(fire-alarm-test PRIVATE include)
target_compile_options(fire-alarm-test PRIVATE -Wall -Wextra)
add_test(NAME fire-alarm-test COMMAND fire-alarm-test)

# STM32 UART 통신만 단독으로 확인하기 위한 경량 실행 파일.
# 카메라/MQTT/DB 없이 src/sensor 코드만 빌드한다. gemini-ocr-test, db-manager-test,
# http-api-test 와 같은 자리 — src/ 는 프로덕션 코드만, 수동 진단 도구는
# tests/integration/ 에 모은다.
add_executable(uart-fire-listener
    tests/integration/UartFireListenerTool.cpp
    src/sensor/SensorLinkManager.cpp
    src/sensor/SensorProtocolParser.cpp
    src/sensor/FireSensorMessage.cpp
    src/util/Logger.cpp)
target_include_directories(uart-fire-listener PRIVATE include)
target_compile_options(uart-fire-listener PRIVATE -Wall -Wextra)
target_link_libraries(uart-fire-listener PRIVATE Threads::Threads)

# STM32 UART -> FireAlarmManager -> 실제 MQTT 브로커까지 카메라/DB/OCR 없이
# 검증하는 도구. main.cpp 는 건드리지 않고, 여기서 검증된 배선을 그대로
# main.cpp 로 옮기는 방식으로 통합한다.
add_executable(fire-pipeline-tool
    tests/integration/FirePipelineTool.cpp
    src/app/AppConfig.cpp
    src/event/FireAlarmManager.cpp
    src/event/EventPayloadBuilder.cpp
    src/sensor/SensorLinkManager.cpp
    src/sensor/SensorProtocolParser.cpp
    src/sensor/FireSensorMessage.cpp
    src/util/Logger.cpp
    src/util/StringUtil.cpp
    src/util/TimeUtil.cpp)
target_include_directories(fire-pipeline-tool PRIVATE include)
target_compile_options(fire-pipeline-tool PRIVATE -Wall -Wextra)
target_link_libraries(fire-pipeline-tool PRIVATE mosquitto Threads::Threads)

add_test(NAME parking-domain-test
    COMMAND parking-domain-test
            ${CMAKE_SOURCE_DIR}/tests/fixtures/parking_slots.json)
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
    src/event/SystemEventReporter.cpp
    src/util/StringUtil.cpp
    src/util/Logger.cpp)
target_include_directories(uart-lora-driver-test PRIVATE include)
target_compile_options(uart-lora-driver-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(uart-lora-driver-test PRIVATE Threads::Threads util)
add_test(NAME uart-lora-driver-test COMMAND uart-lora-driver-test)

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

# 설치된 카메라 화면에서 주차면 ROI를 측정하는 독립 C++/OpenCV 도구.
# 운영 pi-server와 별도 프로세스이며, RTSP URL은 환경변수로만 전달한다.
add_executable(check_coordinates tools/check_coordinates.cpp)
target_compile_options(check_coordinates PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(check_coordinates PRIVATE
    PkgConfig::OPENCV PkgConfig::HTTPLIB Threads::Threads)
add_test(NAME check-coordinates-test
    COMMAND check_coordinates
            --image ${CMAKE_SOURCE_DIR}/tests/fixtures/roi_checker_test.ppm
            --slot EV01
            --rect 1,0,2,1
            --output ${CMAKE_CURRENT_BINARY_DIR}/roi_checker_test_preview.png)
set_tests_properties(check-coordinates-test PROPERTIES
    PASS_REGULAR_EXPRESSION
        "IVA_EV01_ROI_X=0.250000;IVA_EV01_ROI_WIDTH=0.500000")

# /dev/parking_alert 문자 디바이스 제어 도구. 커널 모듈은 명시적 타깃으로만 빌드한다.
add_executable(parking-alert-ctl tools/parking_alert_ctl.c)
target_include_directories(parking-alert-ctl PRIVATE include/uapi)
target_compile_options(parking-alert-ctl PRIVATE -Wall -Wextra -Wpedantic)

add_library(parking-alert-adapter STATIC src/device/LinuxDriverAdapter.cpp)
target_include_directories(parking-alert-adapter PUBLIC include include/uapi)
target_compile_options(parking-alert-adapter PRIVATE -Wall -Wextra -Wpedantic)

add_custom_target(parking-alert-module
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${CMAKE_SOURCE_DIR}/driver/parking_alert
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building /dev/parking_alert Linux kernel module")

# ---- parking timer (timer_ 프로젝트 통합분) ----
add_library(parking-timer-core STATIC
    src/database/EventDatabase.cpp
    src/database/EventDatabaseTimer.cpp
    src/database/db_manager.c
    src/util/Logger.cpp
    src/timer/EventManager.cpp
    src/timer/ParkingSlotManager.cpp
    src/timer/RuntimeConfig.cpp
    src/timer/TimerManager.cpp
    src/timer/Types.cpp)
target_include_directories(parking-timer-core PUBLIC include)
target_compile_options(parking-timer-core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(parking-timer-core PUBLIC sqlite3 Threads::Threads)

# 메인 서버에서도 동일한 타이머 코어를 사용한다. parking-timer 독립 실행 파일도 유지한다.
target_link_libraries(pi-server PRIVATE parking-timer-core)

add_executable(parking-timer src/timer/main.cpp)
target_link_libraries(parking-timer PRIVATE parking-timer-core)
target_compile_options(parking-timer PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
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
target_compile_definitions(parking-timer-tests PRIVATE
    PARKING_TIMER_TEST_SQL_DIR="${CMAKE_CURRENT_BINARY_DIR}/db")
add_test(NAME parking-timer-tests COMMAND parking-timer-tests)

# 카메라/UART 없이 홀센서 입력부터 OCR 대역과 장기점유 타이머까지 잇는 통합 테스트.
add_executable(hall-timer-integration-test
    tests/integration/HallTimerIntegrationTest.cpp
    src/sensor/HallParkingService.cpp
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
    src/http/ParkingHttpServer.cpp
    src/settings/OverstayThresholdService.cpp)
target_include_directories(http-api-test PRIVATE include)
target_compile_options(http-api-test PRIVATE -Wall -Wextra)
target_link_libraries(http-api-test PRIVATE
    parking-timer-core PkgConfig::HTTPLIB sqlite3 Threads::Threads)
add_test(NAME http-api-test COMMAND http-api-test)

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

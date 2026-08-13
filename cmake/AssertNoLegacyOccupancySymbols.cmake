if(NOT DEFINED NM_TOOL OR NOT DEFINED BINARY_PATH)
    message(FATAL_ERROR "occupancy symbol gate requires NM_TOOL and BINARY_PATH")
endif()

execute_process(
    COMMAND "${NM_TOOL}" -C --defined-only "${BINARY_PATH}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed for ${BINARY_PATH}: ${nm_error}")
endif()

set(forbidden_symbols
    "database::EventDatabase::createEntryWithBestShot("
    "database::EventDatabase::createEntryWithSnapshot("
    "database::EventDatabase::createHallSession("
    "database::EventDatabase::insertParked("
    "database::EventDatabase::cancelUnscheduled("
    "database::EventDatabase::departActiveBySlot("
    "database::EventDatabase::clearTimerLogs("
    "parking_timer::ParkingSlotManager::handleEntry("
    "parking_timer::ParkingSlotManager::handleExit("
    "parking::ParkingSlotManager::handle("
    "parking::ParkingSessionWorker::onSensorEvent("
    "event::IvaSlotOccupancyAggregator::update("
    "db_update_slot_status"
    "db_create_parking_session"
    "db_end_parking_session")

foreach(symbol IN LISTS forbidden_symbols)
    string(FIND "${nm_output}" "${symbol}" found_at)
    if(NOT found_at EQUAL -1)
        message(FATAL_ERROR
            "production pi-server contains forbidden legacy occupancy symbol: ${symbol}")
    endif()
endforeach()

message(STATUS "production occupancy symbol gate passed: ${BINARY_PATH}")

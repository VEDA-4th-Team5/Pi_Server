#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::string configPath =
            argc >= 2
                ? argv[1]
                : "tests/fixtures/parking_slots.json";

        auto configs =
            parking::ParkingSlotConfigLoader::loadFromFile(
                configPath);

        parking::SensorSlotIndex index(configs);
        require(index.size() >= 1,
                "expected at least one enabled sensor mapping");

        sensor::SensorProtocolParser parser;
        sensor::ParkingSensorEventAdapter adapter(index);
        parking::ParkingSensorSequenceGuard sequenceGuard;
        parking::ParkingSlotManager manager(
            std::move(configs));

        const auto now =
            std::chrono::system_clock::now();

        std::string error;
        const auto occupiedMessage = parser.parse(
            "SENSOR:HALL02:OCCUPIED:10",
            now,
            &error);
        require(occupiedMessage.has_value(),
                "occupied message parse failed: " + error);

        const auto occupiedEvent = adapter.adapt(
            *occupiedMessage,
            &error);
        require(occupiedEvent.has_value(),
                "occupied event adapt failed: " + error);
        require(occupiedEvent->slotId == "EV02",
                "HALL02 was not mapped to EV02");
        require(sequenceGuard.accept(*occupiedEvent, &error),
                "first sequence was rejected");

        const auto started = manager.handle(*occupiedEvent);
        require(
            started.code ==
                parking::ParkingTransitionCode::SessionStarted,
            "occupied event did not start a session");

        const auto duplicateMessage = parser.parse(
            "SENSOR:HALL02:OCCUPIED:10",
            now,
            &error);
        require(duplicateMessage.has_value(),
                "duplicate message parse failed");
        const auto duplicateEvent = adapter.adapt(
            *duplicateMessage,
            &error);
        require(duplicateEvent.has_value(),
                "duplicate event adapt failed");
        require(!sequenceGuard.accept(*duplicateEvent, &error),
                "duplicate sequence was accepted");

        const auto vacantMessage = parser.parse(
            "SENSOR:HALL02:VACANT:11",
            now + std::chrono::seconds(2),
            &error);
        require(vacantMessage.has_value(),
                "vacant message parse failed: " + error);
        const auto vacantEvent = adapter.adapt(
            *vacantMessage,
            &error);
        require(vacantEvent.has_value(),
                "vacant event adapt failed: " + error);
        require(sequenceGuard.accept(*vacantEvent, &error),
                "newer sequence was rejected");

        const auto completed = manager.handle(*vacantEvent);
        require(
            completed.code ==
                parking::ParkingTransitionCode::SessionCompleted,
            "vacant event did not complete the session");
        require(completed.sessionId == started.sessionId,
                "session id changed between start and completion");

        const auto unknown = parser.parse(
            "SENSOR:unknown_sensor:OCCUPIED:1",
            now,
            &error);
        require(unknown.has_value(),
                "unknown sensor message should still parse");
        require(!adapter.adapt(*unknown, &error).has_value(),
                "unknown sensor was unexpectedly mapped");

        require(
            !parser.parse(
            "SENSOR:HALL02:MAYBE",
                now,
                &error).has_value(),
            "invalid sensor state was accepted");

        std::cout
            << "[PASS] parking sensor stage 2"
            << " mappings=" << index.size()
            << " session=" << started.sessionId
            << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "[FAIL] parking sensor stage 2: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}

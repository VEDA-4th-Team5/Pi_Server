#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void printResult(
    const std::string& label,
    const parking::ParkingTransitionResult& result) {
    std::cout
        << "[TEST] " << label
        << " code=" << parking::toString(result.code)
        << " slot=" << result.slotId;

    if (!result.sessionId.empty()) {
        std::cout << " session=" << result.sessionId;
    }

    std::cout << " changed="
              << (result.changed() ? "true" : "false")
              << '\n';
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

        if (argc >= 3 && std::string(argv[2]) == "--validate-production") {
            struct ExpectedSlot {
                const char* slotId;
                const char* zoneType;
                const char* sensorId;
                const char* videoSourceToken;
                const char* ruleName;
            };
            const ExpectedSlot expectedSlots[] = {
                {"EV01", "ev_charging", "",       "vs-0", "name1"},
                {"EV02", "ev_charging", "HALL02", "vs-0", "name2"},
                {"EV03", "ev_charging", "HALL01", "vs-0", "name3"},
                {"EV04", "ev_charging", "",       "vs-0", "name4"},
                {"P01",  "normal",      "",       "vs-2", "name5"},
                {"P02",  "normal",      "HALL03", "vs-2", "name6"},
                {"P03",  "normal",      "HALL04", "vs-2", "name7"},
                {"P04",  "normal",      "",       "vs-2", "name8"},
            };
            require(configs.size() == 8,
                    "production config must contain four EV and four normal slots");
            for (const auto& expected : expectedSlots) {
                const auto found = std::find_if(
                    configs.begin(), configs.end(),
                    [&expected](const auto& config) {
                        return config.slotId == expected.slotId;
                    });
                require(found != configs.end() && found->enabled,
                        std::string(expected.slotId) + " must be enabled");
                require(found->zoneType == expected.zoneType,
                        std::string(expected.slotId) +
                            " zone policy mismatch");
                require(found->sensorId == expected.sensorId,
                        std::string(expected.slotId) +
                            " sensor mapping mismatch");
                require(found->cameraBindings.size() == 1 &&
                            found->cameraBindings.front().enabled &&
                            found->cameraBindings.front().cameraId == "cam01" &&
                            found->cameraBindings.front().videoSourceToken ==
                                expected.videoSourceToken &&
                            found->cameraBindings.front().ruleName ==
                                expected.ruleName,
                        std::string(expected.slotId) +
                            " native WiseAI mapping mismatch");
            }

            parking::SensorSlotIndex sensorIndex(configs);
            require(sensorIndex.size() == 4,
                    "production config must expose four Hall mappings");
            sensor::SensorProtocolParser parser;
            sensor::ParkingSensorEventAdapter adapter(sensorIndex);
            struct ExpectedMessage {
                const char* raw;
                const char* slotId;
            };
            const ExpectedMessage expectedMessages[] = {
                {"SENSOR:STM1:HALL02:OCCUPIED:12", "EV02"},
                {"SENSOR:STM1:HALL01:VACANT:13",   "EV03"},
                {"SENSOR:STM2:HALL03:OCCUPIED:7", "P02"},
                {"SENSOR:STM2:HALL04:VACANT:8",   "P03"},
            };
            const auto receivedAt = std::chrono::system_clock::now();
            for (const auto& expected : expectedMessages) {
                std::string error;
                const auto message = parser.parse(
                    expected.raw, receivedAt, &error);
                require(message.has_value(),
                        std::string("real Hall message parse failed: ") +
                            error);
                const auto event = adapter.adapt(*message, &error);
                require(event.has_value() && event->slotId == expected.slotId,
                        std::string(expected.raw) + " mapping mismatch");
            }
            std::cout << "[PASS] production parking slots CH1 EV / CH3 normal\n";
            return EXIT_SUCCESS;
        }

        require(configs.size() == 4,
                "expected four configured slots");

        std::size_t enabledCount = 0;
        for (const auto& config : configs) {
            if (config.enabled) {
                ++enabledCount;
            }
        }

        require(enabledCount == 3,
                "expected three enabled test slots");

        parking::ParkingSlotManager manager(
            std::move(configs));

        require(manager.slotCount() == 4,
                "manager slot count mismatch");
        require(manager.activeSlotCount() == 3,
                "manager active slot count mismatch");

        const auto base =
            std::chrono::system_clock::now();

        const auto started = manager.handle({
            "EV01",
            "HALL01",
            parking::ParkingSensorState::Occupied,
            base,
            std::nullopt
        });
        printResult("slot_01 occupied", started);
        require(
            started.code ==
                parking::ParkingTransitionCode::SessionStarted,
            "slot_01 session was not started");
        require(!started.sessionId.empty(),
                "started session id is empty");

        const auto duplicateOccupied = manager.handle({
            "EV01",
            "HALL01",
            parking::ParkingSensorState::Occupied,
            base + 100ms,
            std::nullopt
        });
        printResult(
            "slot_01 duplicate occupied",
            duplicateOccupied);
        require(
            duplicateOccupied.code ==
                parking::ParkingTransitionCode::
                    DuplicateOccupiedIgnored,
            "duplicate occupied event was not ignored");
        require(
            duplicateOccupied.sessionId ==
                started.sessionId,
            "duplicate occupied event changed session id");

        const auto invalidVacant = manager.handle({
            "EV01",
            "HALL01",
            parking::ParkingSensorState::Vacant,
            base - 1ms,
            std::nullopt
        });
        printResult(
            "slot_01 invalid early vacant",
            invalidVacant);
        require(
            invalidVacant.code ==
                parking::ParkingTransitionCode::InvalidTimestamp,
            "invalid timestamp was not rejected");

        const auto completed = manager.handle({
            "EV01",
            "HALL01",
            parking::ParkingSensorState::Vacant,
            base + 5s,
            std::nullopt
        });
        printResult("slot_01 vacant", completed);
        require(
            completed.code ==
                parking::ParkingTransitionCode::SessionCompleted,
            "slot_01 session was not completed");
        require(
            completed.sessionId == started.sessionId,
            "completed session id does not match started id");
        require(
            completed.session.has_value() &&
                !completed.session->active(),
            "completed session state is not completed");

        const auto duplicateVacant = manager.handle({
            "EV01",
            "HALL01",
            parking::ParkingSensorState::Vacant,
            base + 6s,
            std::nullopt
        });
        printResult(
            "slot_01 duplicate vacant",
            duplicateVacant);
        require(
            duplicateVacant.code ==
                parking::ParkingTransitionCode::
                    DuplicateVacantIgnored,
            "duplicate vacant event was not ignored");

        const auto disabled = manager.handle({
            "EV04",
            "HALL04",
            parking::ParkingSensorState::Occupied,
            base,
            std::nullopt
        });
        printResult("slot_04 disabled", disabled);
        require(
            disabled.code ==
                parking::ParkingTransitionCode::DisabledSlot,
            "disabled slot event was not rejected");

        const auto sensorMismatch = manager.handle({
            "EV02",
            "wrong_sensor",
            parking::ParkingSensorState::Occupied,
            base,
            std::nullopt
        });
        printResult(
            "slot_02 sensor mismatch",
            sensorMismatch);
        require(
            sensorMismatch.code ==
                parking::ParkingTransitionCode::SensorMismatch,
            "sensor mismatch was not rejected");

        const auto unknown = manager.handle({
            "EV99",
            "HALL99",
            parking::ParkingSensorState::Occupied,
            base,
            std::nullopt
        });
        printResult("unknown slot", unknown);
        require(
            unknown.code ==
                parking::ParkingTransitionCode::UnknownSlot,
            "unknown slot was not rejected");

        const auto* slot01 =
            manager.findSlot("EV01");
        require(slot01 != nullptr,
                "slot_01 lookup failed");
        require(!slot01->occupied(),
                "slot_01 should be vacant after completion");

        const auto* slot04 =
            manager.findSlot("EV04");
        require(slot04 != nullptr,
                "slot_04 lookup failed");
        require(!slot04->config.enabled,
                "slot_04 should be disabled");

        std::cout
            << "[PASS] parking domain stage 1"
            << " configured_slots=" << manager.slotCount()
            << " enabled_slots=" << manager.activeSlotCount()
            << '\n';

        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "[FAIL] parking domain stage 1: "
            << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

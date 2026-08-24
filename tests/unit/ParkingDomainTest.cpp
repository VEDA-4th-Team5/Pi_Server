#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"

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
            require(configs.size() == 8,
                    "production config must contain EV01 through EV08");
            for (int index = 1; index <= 8; ++index) {
                std::ostringstream slotId;
                slotId << "EV" << std::setw(2) << std::setfill('0')
                       << index;
                const std::string expectedSlot = slotId.str();
                const std::string expectedSensor =
                    "HALL" + expectedSlot.substr(2);
                const std::string expectedRule =
                    "name" + std::to_string(index);
                const std::string expectedToken =
                    index <= 4 ? "vs-0" : "vs-2";
                const auto found = std::find_if(
                    configs.begin(), configs.end(),
                    [&expectedSlot](const auto& config) {
                        return config.slotId == expectedSlot;
                    });
                require(found != configs.end() && found->enabled,
                        expectedSlot + " must be enabled");
                require(found->sensorId == expectedSensor,
                        expectedSlot + " sensor mapping mismatch");
                require(found->cameraBindings.size() == 1 &&
                            found->cameraBindings.front().enabled &&
                            found->cameraBindings.front().cameraId == "cam01" &&
                            found->cameraBindings.front().videoSourceToken ==
                                expectedToken &&
                            found->cameraBindings.front().ruleName ==
                                expectedRule,
                        expectedSlot + " native WiseAI mapping mismatch");
            }
            std::cout << "[PASS] production parking slot config EV01~EV08\n";
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

#include "parking/ParkingSensorSequenceGuard.hpp"
#include "parking/ParkingSlotConfig.hpp"
#include "parking/ParkingSlotManager.hpp"
#include "parking/SensorSlotIndex.hpp"
#include "sensor/ParkingSensorEventAdapter.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <chrono>
#include <cmath>
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

        parking::ParkingSensorSequenceGuard epochGuard;
        const auto legacyHigh = adapter.adapt(
            *parser.parse("SENSOR:HALL02:VACANT:900", now, &error), &error);
        const auto firstV2 = adapter.adapt(
            *parser.parse("SENSOR2:HALL02:OCCUPIED:boot-b:1", now, &error),
            &error);
        require(legacyHigh && firstV2 &&
                    firstV2->sourceProtocolVersion ==
                        sensor::SensorProtocolVersion::BootEpochV2 &&
                    firstV2->sourceBootId == "boot-b",
                "SENSOR2 parser/adapter lost explicit epoch metadata");
        require(epochGuard.accept(*legacyHigh, &error) &&
                    epochGuard.accept(*firstV2, &error),
                "legacy high to first v2 low migration was rejected");
        const auto downgrade = adapter.adapt(
            *parser.parse("SENSOR:HALL02:VACANT:901", now, &error), &error);
        require(downgrade && !epochGuard.accept(*downgrade, &error),
                "legacy downgrade was accepted after v2 migration");
        const auto nextBoot = adapter.adapt(
            *parser.parse("SENSOR2:HALL02:VACANT:boot-c:1", now, &error),
            &error);
        const auto retiredBoot = adapter.adapt(
            *parser.parse("SENSOR2:HALL02:OCCUPIED:boot-b:2", now, &error),
            &error);
        require(nextBoot && retiredBoot &&
                    epochGuard.accept(*nextBoot, &error) &&
                    !epochGuard.accept(*retiredBoot, &error),
                "retired Hall boot ID was accepted");
        require(!parser.parse(
                    "SENSOR2:HALL02:OCCUPIED::1", now, &error),
                "SENSOR2 without boot ID was accepted");

        // LoRa 규격 v1.1: SENSOR:<node>:<sensor>:<state>:<seq>. 노드 ID는 슬롯
        // 매핑에 쓰이지 않지만(센서 ID가 이미 전역 고유) 파서가 필드를 정확히
        // 옮겨 담아야 한다.
        const auto nodeMessage = parser.parse(
            "SENSOR:STM1:HALL03:OCCUPIED:7", now, &error);
        require(nodeMessage.has_value(),
                "node-prefixed sensor message parse failed: " + error);
        require(nodeMessage->sensorId == "HALL03" &&
                    nodeMessage->node == "STM1" &&
                    nodeMessage->sequence == 7,
                "node-prefixed sensor fields mismatch");
        const auto nodeEvent = adapter.adapt(*nodeMessage, &error);
        require(nodeEvent.has_value() && nodeEvent->slotId == "EV03",
                "node-prefixed sensor was not mapped to EV03");

        // FIRE:<node>:<sensor>:<state>:<seq>:<energy>. energy는 표시용이라
        // adapter/도메인 로직에는 들어가지 않고 파서 산출물에만 남는다.
        const auto fireEnergyMessage = parser.parseFire(
            "FIRE:STM1:FLAME01:DETECTED:26:42.09", now, &error);
        require(fireEnergyMessage.has_value(),
                "node-prefixed fire message parse failed: " + error);
        require(fireEnergyMessage->sensorId == "FLAME01" &&
                    fireEnergyMessage->node == "STM1" &&
                    fireEnergyMessage->sequence == 26 &&
                    fireEnergyMessage->energy.has_value() &&
                    std::abs(*fireEnergyMessage->energy - 42.09) < 0.001,
                "node-prefixed fire fields mismatch");
        require(!parser.parseFire(
                    "FIRE:STM1:FLAME01:DETECTED:26", now, &error).has_value(),
                "node-prefixed fire without energy was accepted");

        // STM32가 재부팅하면 노드의 단일 seq 카운터가 1부터 다시 시작한다.
        // 직전 값보다 1000 넘게 줄어들면 stale이 아니라 재부팅으로 보고
        // 받아들여야 한다(작은 역행은 그대로 거부).
        parking::ParkingSensorSequenceGuard rebootGuard;
        const auto highBaseline = adapter.adapt(
            *parser.parse("SENSOR:HALL02:OCCUPIED:5000", now, &error), &error);
        require(highBaseline && rebootGuard.accept(*highBaseline, &error),
                "high sequence baseline was rejected");
        const auto smallRegression = adapter.adapt(
            *parser.parse("SENSOR:HALL02:VACANT:4999", now, &error), &error);
        require(smallRegression &&
                    !rebootGuard.accept(*smallRegression, &error),
                "small sequence regression should still be rejected as stale");
        const auto rebooted = adapter.adapt(
            *parser.parse("SENSOR:HALL02:OCCUPIED:2", now, &error), &error);
        require(rebooted && rebootGuard.accept(*rebooted, &error),
                "large sequence drop was not accepted as a node reboot");

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

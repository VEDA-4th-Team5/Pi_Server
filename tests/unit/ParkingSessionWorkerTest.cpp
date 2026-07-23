// Unit test for parking::ParkingSessionWorker (EVDA-134).
//
// Drives the hall-sensor occupancy state machine through the worker and
// asserts: OCCUPIED starts one session at T0, repeated OCCUPIED polls are
// idempotent (no new session / no changed T0), VACANT completes the same
// session, the active-session index reflects start/complete via a sink, and
// out-of-order sequence packets are dropped. No signal debounce is performed
// on the Pi (that is the STM32's job) — this test encodes that expectation.

#include "parking/ActiveParkingSessionIndex.hpp"
#include "parking/ParkingSessionWorker.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

parking::ParkingSlotConfig makeSlot(
    std::string slotId, std::string sensorId) {
    parking::ParkingSlotConfig config;
    config.slotId = std::move(slotId);
    config.enabled = true;
    config.zoneType = "ev_charging";
    config.sensorId = std::move(sensorId);
    return config;
}

parking::ParkingSensorEvent makeEvent(
    std::string slotId,
    std::string sensorId,
    parking::ParkingSensorState state,
    std::chrono::system_clock::time_point at) {
    parking::ParkingSensorEvent event;
    event.slotId = std::move(slotId);
    event.sensorId = std::move(sensorId);
    event.state = state;
    event.occurredAt = at;
    return event;
}

}  // namespace

int main() {
    using parking::ParkingSensorState;
    using parking::ParkingTransitionCode;

    try {
        std::vector<parking::ParkingSlotConfig> configs;
        configs.push_back(makeSlot("EV01", "HALL01"));
        configs.push_back(makeSlot("EV02", "HALL02"));

        parking::ParkingSessionWorker worker(configs);
        require(worker.slotCount() == 2, "expected 2 configured slots");

        parking::ActiveParkingSessionIndex activeIndex;
        std::vector<parking::ParkingTransitionResult> changes;

        worker.addSink(
            [&activeIndex](const parking::ParkingTransitionResult& t) {
                activeIndex.apply(t);
            });
        worker.addSink(
            [&changes](const parking::ParkingTransitionResult& t) {
                if (t.changed()) {
                    changes.push_back(t);
                }
            });

        const auto t0 = std::chrono::system_clock::now();

        // 1) 입차: 첫 OCCUPIED -> SessionStarted, T0 기록, 활성 index 등록.
        const auto started = worker.onSensorEvent(
            makeEvent("EV01", "HALL01", ParkingSensorState::Occupied, t0));
        require(started.has_value(), "occupied returned nullopt");
        require(started->code == ParkingTransitionCode::SessionStarted,
                "first occupied did not start a session");
        require(started->session.has_value(),
                "started transition is missing the session object");
        require(started->session->startedAt() == t0,
                "T0 was not recorded as the session start time");
        const std::string sessionId = started->sessionId;
        require(!sessionId.empty(), "started session id is empty");
        require(activeIndex.findActiveSessionId("EV01").value_or("") ==
                    sessionId,
                "active index not set on session start");

        // 2) 폴링 중복(시퀀스 없음): OCCUPIED 재수신 -> DuplicateOccupiedIgnored.
        //    세션과 T0 는 바뀌지 않는다(Pi 재디바운스 없음, 멱등 처리).
        const auto duplicate = worker.onSensorEvent(makeEvent(
            "EV01", "HALL01", ParkingSensorState::Occupied,
            t0 + std::chrono::seconds(1)));
        require(duplicate.has_value(), "duplicate occupied returned nullopt");
        require(duplicate->code ==
                    ParkingTransitionCode::DuplicateOccupiedIgnored,
                "repeated occupied poll was not treated idempotently");
        require(activeIndex.findActiveSessionId("EV01").value_or("") ==
                    sessionId,
                "session changed on a duplicate occupied poll");

        // 3) 다른 슬롯은 독립적으로 입차한다.
        const auto other = worker.onSensorEvent(makeEvent(
            "EV02", "HALL02", ParkingSensorState::Occupied,
            t0 + std::chrono::seconds(1)));
        require(other.has_value() &&
                    other->code == ParkingTransitionCode::SessionStarted,
                "second slot did not start its own session");
        require(activeIndex.size() == 2, "expected two active sessions");

        // 4) 출차: VACANT -> SessionCompleted, 같은 session_id, 활성 index 해제.
        const auto completed = worker.onSensorEvent(makeEvent(
            "EV01", "HALL01", ParkingSensorState::Vacant,
            t0 + std::chrono::seconds(5)));
        require(completed.has_value() &&
                    completed->code ==
                        ParkingTransitionCode::SessionCompleted,
                "vacant did not complete the session");
        require(completed->sessionId == sessionId,
                "session id changed between start and completion");
        require(!activeIndex.findActiveSessionId("EV01").has_value(),
                "active index not cleared on completion");

        // 5) 중복 VACANT -> DuplicateVacantIgnored (상태 불변).
        const auto vacantAgain = worker.onSensorEvent(makeEvent(
            "EV01", "HALL01", ParkingSensorState::Vacant,
            t0 + std::chrono::seconds(6)));
        require(vacantAgain.has_value() &&
                    vacantAgain->code ==
                        ParkingTransitionCode::DuplicateVacantIgnored,
                "repeated vacant poll was not treated idempotently");

        // 6) 시퀀스 가드: 최신 시퀀스는 통과, 더 오래된 시퀀스는 드롭(nullopt).
        auto seqNew = makeEvent("EV02", "HALL02",
                                ParkingSensorState::Occupied, t0);
        seqNew.sourceSequence = 10;
        const auto seqNewResult = worker.onSensorEvent(seqNew);
        require(seqNewResult.has_value(),
                "first sequenced packet was dropped");

        auto seqStale = makeEvent("EV02", "HALL02",
                                  ParkingSensorState::Occupied, t0);
        seqStale.sourceSequence = 9;
        const auto seqStaleResult = worker.onSensorEvent(seqStale);
        require(!seqStaleResult.has_value(),
                "stale/out-of-order sequence was not dropped");

        // 정확히 세 번의 '변화'가 있었다: EV01 시작, EV02 시작, EV01 완료.
        require(changes.size() == 3,
                "expected exactly 3 changed transitions, got " +
                    std::to_string(changes.size()));

        std::cout << "[PASS] parking session worker"
                  << " slots=" << worker.slotCount()
                  << " session=" << sessionId
                  << " changes=" << changes.size() << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] parking session worker: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

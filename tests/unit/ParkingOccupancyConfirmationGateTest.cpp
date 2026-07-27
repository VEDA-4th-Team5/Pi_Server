// Unit test for parking::ParkingOccupancyConfirmationGate.
//
// This is the OCCUPIED "확정(T0)" hysteresis policy discussed for EVDA-135:
// a short OCCUPIED<->VACANT flap during a re-parking maneuver must not start
// a session, but OCCUPIED held past the threshold must be confirmed exactly
// once. All timestamps are injected, so the whole policy is deterministic.
// The gate measures elapsed time on receivedMonotonic (steady_clock), so
// every event below carries a steady_clock offset in lockstep with its
// system_clock one -- see ParkingOccupancyConfirmationGate.hpp for why.

#include "parking/ParkingOccupancyConfirmationGate.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

parking::ParkingSensorEvent makeEvent(
    parking::ParkingSensorState state,
    std::chrono::system_clock::time_point systemBase,
    std::chrono::steady_clock::time_point monotonicBase,
    std::chrono::seconds offset) {
    parking::ParkingSensorEvent event;
    event.slotId = "EV01";
    event.sensorId = "HALL01";
    event.state = state;
    event.occurredAt = systemBase + offset;
    event.receivedMonotonic = monotonicBase + offset;
    return event;
}

using parking::ParkingOccupancyConfirmationGate;
using Decision = ParkingOccupancyConfirmationGate::Decision;
using parking::ParkingSensorState;

}  // namespace

int main() {
    try {
        const auto t0 = std::chrono::system_clock::now();
        const auto t0m = std::chrono::steady_clock::now();

        // 1) Held past the threshold -> confirmed exactly once.
        {
            ParkingOccupancyConfirmationGate gate(std::chrono::seconds(10));

            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(0)),
                        false) == Decision::Suppress,
                    "first sighting must not confirm immediately");

            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(9)),
                        false) == Decision::Suppress,
                    "must still be suppressed just under the threshold");

            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(10)),
                        false) == Decision::Forward,
                    "must confirm once the threshold is reached");
        }

        // 2) Short flap (re-parking maneuver): VACANT before the threshold
        //    discards the pending occupancy; the next OCCUPIED starts a new
        //    confirmation window from scratch.
        {
            ParkingOccupancyConfirmationGate gate(std::chrono::seconds(10));

            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(0)),
                        false) == Decision::Suppress,
                    "first sighting suppressed");
            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Vacant, t0, t0m,
                                 std::chrono::seconds(3)),
                        false) == Decision::Suppress,
                    "vacant on an unconfirmed slot must not surface a "
                    "completion (nothing was ever started)");

            // Re-entry restarts the window; holding only 9s from this new
            // start must not confirm even though 12s passed since the very
            // first OCCUPIED.
            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(4)),
                        false) == Decision::Suppress,
                    "re-entry restarts the confirmation window");
            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(13)),
                        false) == Decision::Suppress,
                    "only 9s since the restarted window; must not confirm");
            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(14)),
                        false) == Decision::Forward,
                    "10s since the restarted window; must confirm now");
        }

        // 3) Once already occupied (confirmed), repeated OCCUPIED polls pass
        //    straight through as heartbeats, and VACANT is a real departure.
        {
            ParkingOccupancyConfirmationGate gate(std::chrono::seconds(10));

            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                 std::chrono::seconds(0)),
                        true) == Decision::Forward,
                    "already-occupied heartbeat must pass through untouched");
            require(gate.evaluate(
                        makeEvent(ParkingSensorState::Vacant, t0, t0m,
                                 std::chrono::seconds(1)),
                        true) == Decision::Forward,
                    "departure from a confirmed slot must pass through");
        }

        // 4) Multiple rapid flaps never confirm as long as no single
        //    continuous OCCUPIED span reaches the threshold.
        {
            ParkingOccupancyConfirmationGate gate(std::chrono::seconds(10));
            for (int i = 0; i < 5; ++i) {
                const auto base = std::chrono::seconds(i * 3);
                require(gate.evaluate(
                            makeEvent(ParkingSensorState::Occupied, t0, t0m,
                                     base),
                            false) == Decision::Suppress,
                        "flap " + std::to_string(i) + " occupied suppressed");
                require(gate.evaluate(
                            makeEvent(ParkingSensorState::Vacant, t0, t0m,
                                     base + std::chrono::seconds(1)),
                            false) == Decision::Suppress,
                        "flap " + std::to_string(i) + " vacant suppressed");
            }
        }

        std::cout << "[PASS] parking occupancy confirmation gate\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] parking occupancy confirmation gate: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

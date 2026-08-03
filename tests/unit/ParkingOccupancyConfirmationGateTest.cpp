#include "parking/ParkingOccupancyConfirmationGate.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::ParkingSensorEvent eventAt(
    const parking::ParkingSensorState state,
    const std::chrono::system_clock::time_point wall,
    const std::chrono::steady_clock::time_point monotonic) {
    parking::ParkingSensorEvent event;
    event.slotId = "EV01";
    event.sensorId = "HALL01";
    event.state = state;
    event.occurredAt = wall;
    event.receivedMonotonic = monotonic;
    return event;
}

}  // namespace

int main() {
    try {
        using Decision =
            parking::ParkingOccupancyConfirmationGate::Decision;
        const auto wall = std::chrono::system_clock::now();
        const auto monotonic = std::chrono::steady_clock::now();

        parking::ParkingOccupancyConfirmationGate gate(10s);
        require(gate.evaluate(
                    eventAt(parking::ParkingSensorState::Occupied,
                            wall, monotonic), false) == Decision::Suppress,
                "first OCCUPIED was not held");
        require(gate.takeDue(monotonic + 9999ms, wall + 9999ms).empty(),
                "occupancy confirmed before threshold");
        auto confirmed = gate.takeDue(monotonic + 10s, wall - 1h);
        require(confirmed.size() == 1 &&
                    confirmed.front().state ==
                        parking::ParkingSensorState::Occupied &&
                    confirmed.front().occurredAt == wall - 1h,
                "deadline did not auto-confirm independently of wall clock");
        require(gate.takeDue(monotonic + 20s, wall).empty(),
                "confirmed candidate was emitted twice");

        parking::ParkingOccupancyConfirmationGate flapGate(10s);
        require(flapGate.evaluate(
                    eventAt(parking::ParkingSensorState::Occupied,
                            wall, monotonic), false) == Decision::Suppress,
                "flap OCCUPIED was not held");
        require(flapGate.evaluate(
                    eventAt(parking::ParkingSensorState::Vacant,
                            wall + 3s, monotonic + 3s), false) ==
                    Decision::Suppress,
                "unconfirmed VACANT was forwarded");
        require(flapGate.takeDue(monotonic + 20s, wall + 20s).empty(),
                "VACANT did not cancel pending confirmation");

        require(flapGate.evaluate(
                    eventAt(parking::ParkingSensorState::Occupied,
                            wall, monotonic), true) == Decision::Forward,
                "occupied heartbeat was not forwarded");
        require(flapGate.evaluate(
                    eventAt(parking::ParkingSensorState::Vacant,
                            wall, monotonic), true) == Decision::Forward,
                "confirmed departure was not forwarded");

        std::cout << "[PASS] occupancy gate auto-confirm/flap/steady-clock\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] occupancy confirmation gate: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

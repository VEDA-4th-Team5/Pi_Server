#include "device/ParkingAlertController.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool require(const bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> sets;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> clears;
    int clearAllCount{};
    device::ParkingAlertController controller(
        "EV01:0, EV02:1,EV04:3",
        device::ParkingAlertController::Backend{
          [&](const auto slot, const auto event) { sets.emplace_back(slot, event); },
          [&](const auto slot, const auto event) { clears.emplace_back(slot, event); },
          [&] { ++clearAllCount; }});

    bool ok = true;
    ok &= require(controller.mappedSlotCount() == 3, "slot mapping count mismatch");
    ok &= require(controller.initialize({{"EV02", 22}, {"UNKNOWN", 99}}),
                  "active alert restore failed");
    ok &= require(clearAllCount == 1 && sets.size() == 1 &&
                      sets[0] == std::pair<std::uint32_t, std::uint64_t>{1, 22},
                  "startup restore emitted unexpected commands");
    ok &= require(controller.handleEvent("NON_EV_ALERT", 30, "EV01") &&
                      controller.handleEvent("VIOLATION_TRIGGERED", 31, "EV04"),
                  "alert activation failed");
    ok &= require(controller.handleEvent("DEPARTURE", 31, "EV04"),
                  "departure clear failed");
    ok &= require(controller.handleEvent("ARRIVAL", 32, "EV01"),
                  "irrelevant event must be accepted without I/O");
    ok &= require(!controller.handleEvent("NON_EV_ALERT", 33, "EV03"),
                  "unmapped alert slot must be rejected");
    ok &= require(sets.size() == 3 && sets[1].first == 0 &&
                      sets[2].first == 3 && clears.size() == 1 &&
                      clears[0].first == 3,
                  "runtime commands did not match event policy");

    try {
        device::ParkingAlertController invalid(
            "EV01:0,EV02:0", device::ParkingAlertController::Backend{
                [](auto, auto) {}, [](auto, auto) {}, [] {}});
        ok &= require(false, "duplicate index mapping was accepted");
    } catch (const std::invalid_argument&) {
    }
    return ok ? 0 : 1;
}

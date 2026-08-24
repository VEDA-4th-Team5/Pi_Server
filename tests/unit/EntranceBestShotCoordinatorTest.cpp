#include "entrance/EntranceBestShotCoordinator.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

entrance::BestShotEvent event(const std::string& object,
                              const entrance::BestShotKind kind,
                              const std::int64_t now,
                              const std::string& camera = "cam01",
                              const std::string& channel = "ch02") {
    return {{camera, channel, object}, kind,
            "/stw-cgi/media.cgi/" + object, "rtsp://camera/profile1", now};
}

void repeatedObjectQueuesOnce() {
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    const auto first = coordinator.observe(
        event("123", entrance::BestShotKind::Plate, 1000));
    require(first.code == entrance::ObserveCode::EvQueued,
            "first plate did not reserve one entrance pipeline");
    for (int i = 0; i < 9; ++i) {
        const auto duplicate = coordinator.observe(
            event("123", entrance::BestShotKind::Plate, 1001 + i));
        require(duplicate.code == entrance::ObserveCode::DuplicateSuppressed,
                "repeated plate was not suppressed");
    }
    require(coordinator.duplicateCount({"cam01", "ch02", "123"}) == 9,
            "duplicate summary count is wrong");
}

void vehicleDoesNotCreateAWaitingObject() {
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    require(coordinator.observe(
                event("200", entrance::BestShotKind::Vehicle, 1000)).code ==
                entrance::ObserveCode::Ignored,
            "vehicle BestShot was not ignored");
    require(coordinator.trackedCount() == 0,
            "vehicle BestShot created a waiting object");
    require(coordinator.observe(
                event("200", entrance::BestShotKind::Plate, 1500)).code ==
                entrance::ObserveCode::EvQueued,
            "plate BestShot did not start processing independently");
}

void keyAndTtlBoundaries() {
    entrance::EntranceBestShotCoordinator coordinator(1s, 3);
    require(coordinator.observe(
                event("1", entrance::BestShotKind::Plate, 1000)).code ==
                entrance::ObserveCode::EvQueued,
            "first object rejected");
    require(coordinator.observe(
                event("1", entrance::BestShotKind::Plate, 1000, "cam02")).code ==
                entrance::ObserveCode::EvQueued,
            "different camera was incorrectly deduplicated");
    require(coordinator.observe(
                event("1", entrance::BestShotKind::Plate, 1000, "cam01", "ch03")).code ==
                entrance::ObserveCode::EvQueued,
            "different channel was incorrectly deduplicated");
    require(coordinator.observe(
                event("overflow", entrance::BestShotKind::Plate, 1000)).code ==
                entrance::ObserveCode::CapacityRejected,
            "bounded capacity was not enforced");
    const entrance::ObjectKey key{"cam01", "ch02", "1"};
    require(coordinator.markEvProcessing(key) &&
                coordinator.markOcrQueued(key) &&
                coordinator.markOcrProcessing(key) &&
                coordinator.beginFinalization(key) &&
                coordinator.complete(key),
            "ordered EV/OCR state transitions failed");
    (void)coordinator.sweep(2001);
    require(coordinator.observe(
                event("1", entrance::BestShotKind::Plate, 2002)).code ==
                entrance::ObserveCode::EvQueued,
            "same ObjectId was not reusable after TTL");
}

void concurrentDuplicatesHaveOneWinner() {
    entrance::EntranceBestShotCoordinator coordinator(60s, 64);
    std::vector<entrance::ObserveCode> results(20);
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < results.size(); ++index) {
        workers.emplace_back([&, index] {
            results[index] = coordinator.observe(
                event("concurrent", entrance::BestShotKind::Plate,
                      1000 + static_cast<std::int64_t>(index))).code;
        });
    }
    for (auto& worker : workers) worker.join();
    std::size_t queued{};
    for (const auto result : results)
        if (result == entrance::ObserveCode::EvQueued) ++queued;
    require(queued == 1, "concurrent duplicate created multiple EV owners");
}

}  // namespace

int main() {
    try {
        repeatedObjectQueuesOnce();
        vehicleDoesNotCreateAWaitingObject();
        keyAndTtlBoundaries();
        concurrentDuplicatesHaveOneWinner();
        std::cout << "EntranceBestShotCoordinatorTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceBestShotCoordinatorTest failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

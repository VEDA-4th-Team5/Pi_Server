#include "database/EventDatabase.hpp"
#include "event/FireAlarmService.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Arguments {
    std::string databasePath{"data/db/parking.db"};
    std::string command;
    std::optional<std::string> channelId;
};

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " [--db <path>] status [channel_id]\n"
        << "  " << program << " [--db <path>] clear <channel_id>\n";
}

std::optional<Arguments> parseArguments(const int argc, char** argv) {
    Arguments parsed;
    int index = 1;
    if (index < argc && std::string(argv[index]) == "--db") {
        if (++index >= argc) return std::nullopt;
        parsed.databasePath = argv[index++];
    }
    if (index >= argc) return std::nullopt;
    parsed.command = argv[index++];
    if (index < argc) parsed.channelId = argv[index++];
    if (index != argc) return std::nullopt;
    if (parsed.command == "status") return parsed;
    if (parsed.command == "clear" && parsed.channelId) return parsed;
    return std::nullopt;
}

void printState(const event::FireAlarmStateRecord& state) {
    std::cout << "channel=" << state.channelId
              << " sensor=" << state.sensorId
              << " lifecycle=" << event::toString(state.desiredLifecycle)
              << " revision=" << state.fireRevision
              << " event_id=" << state.lastEventId
              << " updated_at=" << state.updatedAt << '\n';
}

int showStatus(database::EventDatabase& database,
               const std::optional<std::string>& channel_id) {
    if (channel_id) {
        const auto state = database.getFireAlarmState(*channel_id);
        if (!state) {
            std::cerr << "Fire channel was not found: " << *channel_id << '\n';
            return 3;
        }
        printState(*state);
        return 0;
    }

    const auto states = database.listFireAlarmStates();
    if (states.empty()) {
        std::cerr << "No Fire channels are configured\n";
        return 3;
    }
    for (const auto& state : states) printState(state);
    return 0;
}

class ResultWaiter {
public:
    void set(const event::FireCommandResult& result) {
        std::lock_guard lock(mutex_);
        result_ = result;
        condition_.notify_all();
    }

    std::optional<event::FireCommandResult> wait() {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 5s, [&] { return result_.has_value(); }))
            return std::nullopt;
        return result_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<event::FireCommandResult> result_;
};

int clearAlarm(database::EventDatabase& database,
               const std::string& channel_id) {
    const auto current = database.getFireAlarmState(channel_id);
    if (!current) {
        std::cerr << "Fire channel was not found: " << channel_id << '\n';
        return 3;
    }
    if (current->desiredLifecycle == event::FireAlarmLifecycle::Resolved) {
        std::cout << "Fire alarm is already resolved\n";
        printState(*current);
        return 0;
    }
    const auto states = database.listFireAlarmStates();
    std::vector<event::FireChannelBinding> bindings;
    bindings.reserve(states.size());
    for (const auto& state : states) {
        bindings.push_back(
            {state.sensorId, state.channelId, state.retainedTopic});
    }

    ResultWaiter waiter;
    event::FireAlarmService::Config config;
    config.cameraId = "cam01";
    config.lifecycleTopicPrefix = "parking/v1/events";
    event::FireAlarmService service(
        database, std::move(config), std::move(bindings),
        [&waiter](const event::FireCommandResult& result) {
            waiter.set(result);
        });
    if (!service.initialize() || !service.start()) {
        std::cerr << "Fire service could not start: "
                  << service.configurationError() << '\n';
        return 5;
    }

    const auto admission = service.submitManualClear(channel_id);
    if (admission.status != event::FireCommandStatus::Queued) {
        std::cerr << "Fire clear was rejected: " << admission.error << '\n';
        service.closeIngress();
        service.stop();
        return 6;
    }

    const auto result = waiter.wait();
    service.closeIngress();
    const bool stopped = service.stop();
    if (!result) {
        std::cerr << "Timed out waiting for the Fire clear commit\n";
        return 7;
    }
    if (!stopped) {
        std::cerr << "Fire service did not stop cleanly\n";
        return 8;
    }
    if (result->status != event::FireCommandStatus::DurablyCommitted &&
        result->status != event::FireCommandStatus::Idempotent) {
        std::cerr << "Fire clear failed: " << result->error << '\n';
        return 9;
    }

    const auto updated = database.getFireAlarmState(channel_id);
    if (!updated ||
        updated->desiredLifecycle != event::FireAlarmLifecycle::Resolved) {
        std::cerr << "Fire state was not resolved after commit\n";
        return 10;
    }
    std::cout << "Fire alarm cleared. Restart pi-server to publish the "
                 "pending MQTT state.\n";
    printState(*updated);
    std::cout << "Warning: a later DETECTED sensor signal will open the alarm "
                 "again.\n";
    return 0;
}

}  // namespace

int main(const int argc, char** argv) {
    const auto arguments = parseArguments(argc, argv);
    if (!arguments) {
        usage(argv[0]);
        return 2;
    }

    database::EventDatabase database;
    if (!database.open(arguments->databasePath)) {
        std::cerr << "Could not open database: "
                  << arguments->databasePath << '\n';
        return 3;
    }

    if (arguments->command == "status")
        return showStatus(database, arguments->channelId);
    return clearAlarm(database, *arguments->channelId);
}

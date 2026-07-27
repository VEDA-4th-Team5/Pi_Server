#include "event/SystemEventReporter.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void testDeduplicationAndRecovery() {
    std::mutex mutex;
    std::vector<event::SystemEvent> persisted;
    std::vector<std::string> payloads;
    event::SystemEventReporter::Config config;
    config.queue_capacity = 8;
    config.duplicate_window = 80ms;
    config.sink_retry_delay = 10ms;
    event::SystemEventReporter reporter(
        [&](const event::SystemEvent& system_event, const std::string& payload) {
            std::lock_guard lock(mutex);
            persisted.push_back(system_event);
            payloads.push_back(payload);
            return true;
        }, config);
    require(reporter.start(), "reporter did not start");

    const event::SystemEvent failure{
        .source = event::SystemEventSource::Uart,
        .code = event::SystemEventCode::UartOpenFailed,
        .severity = event::SystemEventSeverity::Error,
        .slot_id = {},
        .transport = "uart-line",
        .device = "/dev/test-uart",
        .message = "device unavailable",
        .retry_count = 1,
    };
    reporter.report(failure);
    reporter.report(failure);
    reporter.report(failure);
    require(waitUntil([&] {
                std::lock_guard lock(mutex);
                return persisted.size() == 1;
            }, 500ms),
            "duplicate errors were not suppressed");

    std::this_thread::sleep_for(90ms);
    reporter.report(failure);
    require(waitUntil([&] {
                std::lock_guard lock(mutex);
                return persisted.size() == 2;
            }, 500ms),
            "error after duplicate window was not persisted");

    reporter.report({
        .source = event::SystemEventSource::Uart,
        .code = event::SystemEventCode::UartRecovered,
        .severity = event::SystemEventSeverity::Warning,
        .slot_id = {},
        .transport = "uart-line",
        .device = "/dev/test-uart",
        .message = "link recovered",
        .retry_count = 3,
        .recovered = true,
    });
    require(waitUntil([&] {
                std::lock_guard lock(mutex);
                return persisted.size() == 3;
            }, 500ms),
            "recovery event was not persisted");
    reporter.stop();

    std::lock_guard lock(mutex);
    require(persisted[1].suppressed_count == 2,
            "suppressed error count was not attached to next event");
    require(persisted[2].recovered, "recovered flag was lost");
    require(payloads[0].find("\"source\":\"UART\"") != std::string::npos,
            "serialized source is missing");
    require(payloads[0].find("\"retry_count\":1") != std::string::npos,
            "serialized retry count is missing");
}

void testSinkFailureIsRetried() {
    std::mutex mutex;
    int attempts = 0;
    event::SystemEventReporter::Config config;
    config.queue_capacity = 4;
    config.duplicate_window = 0ms;
    config.sink_retry_delay = 10ms;
    event::SystemEventReporter reporter(
        [&](const event::SystemEvent&, const std::string&) {
            std::lock_guard lock(mutex);
            ++attempts;
            return attempts >= 2;
        }, config);
    require(reporter.start(), "retry reporter did not start");
    reporter.report({
        .source = event::SystemEventSource::LoRa,
        .code = event::SystemEventCode::LoRaFrameRejected,
        .severity = event::SystemEventSeverity::Warning,
        .slot_id = {},
        .transport = "lora-frame",
        .device = {},
        .message = "CRC rejected",
    });
    require(waitUntil([&] {
                std::lock_guard lock(mutex);
                return attempts >= 2;
            }, 500ms),
            "failed sink event was not retried");
    reporter.stop();
}

void testThrowingSinkCannotEscapeWorker() {
    event::SystemEventReporter::Config config;
    config.sink_retry_delay = 5ms;
    event::SystemEventReporter reporter(
        [](const event::SystemEvent&, const std::string&) -> bool {
            throw std::runtime_error("mock sink failure");
        }, config);
    require(reporter.start(), "throwing sink reporter did not start");
    reporter.report({
        .source = event::SystemEventSource::HallSensor,
        .code = event::SystemEventCode::SensorMessageInvalid,
        .severity = event::SystemEventSeverity::Warning,
        .slot_id = {},
        .transport = {},
        .device = {},
        .message = "invalid test message",
    });
    std::this_thread::sleep_for(20ms);
    reporter.stop();
}

}  // namespace

int main() {
    try {
        testDeduplicationAndRecovery();
        testSinkFailureIsRetried();
        testThrowingSinkCannotEscapeWorker();
        std::cout << "System event reporter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "System event reporter tests failed: " << error.what() << '\n';
        return 1;
    }
}

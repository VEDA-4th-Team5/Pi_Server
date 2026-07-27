#include "event/FireAlarmEvent.hpp"
#include "event/FireAlarmManager.hpp"
#include "sensor/SensorProtocolParser.hpp"

#include <chrono>
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct PublishedMessage {
    std::string topic;
    std::string payload;
};

event::FireSignal toSignal(const sensor::FireSensorMessage& message) {
    event::FireSignal signal;
    signal.sensorId = message.sensorId;
    signal.detected = message.state == sensor::FireSensorState::Detected;
    signal.occurredAt = message.occurredAt;
    signal.sourceSequence = message.sequence;
    signal.sourceTransport = message.transport;
    signal.rawPayload = message.raw;
    return signal;
}

void testParser() {
    const sensor::SensorProtocolParser parser;
    const auto now = std::chrono::system_clock::now();

    require(sensor::SensorProtocolParser::isFireLine("FIRE:F1:DETECTED"),
            "FIRE line must be routed to the fire parser");
    require(!sensor::SensorProtocolParser::isFireLine("SENSOR:H1:OCCUPIED"),
            "SENSOR line must not be routed to the fire parser");

    auto detected = parser.parseFire("FIRE:F1:DETECTED", now);
    require(detected.has_value(), "minimal FIRE frame must parse");
    require(detected->sensorId == "F1", "sensor id mismatch");
    require(detected->state == sensor::FireSensorState::Detected,
            "state must be Detected");

    auto cleared = parser.parseFire(" fire : F1 : cleared : 7 \r\n", now);
    require(cleared.has_value(), "FIRE frame must be case and space tolerant");
    require(cleared->state == sensor::FireSensorState::Cleared,
            "state must be Cleared");
    require(cleared->sequence.has_value() && *cleared->sequence == 7,
            "sequence must be parsed");

    std::string error;
    require(!parser.parseFire("FIRE:F1:BURNING", now, &error).has_value(),
            "unknown state must be rejected");
    require(!error.empty(), "rejection must report a reason");
    require(!parser.parseFire("FIRE::DETECTED", now).has_value(),
            "empty sensor id must be rejected");

    // 기존 주차 센서 프레임이 깨지지 않았는지 함께 확인한다.
    auto parking = parser.parse("SENSOR:H1:OCCUPIED:3:1700000000000", now);
    require(parking.has_value(), "SENSOR frame must still parse");
    require(parking->sequence.has_value() && *parking->sequence == 3,
            "SENSOR sequence must still parse");
}

void testBindingSpec() {
    const auto bindings =
        event::parseFireSensorBindings("F1=EV01:ch02, F2=EV02 ,,broken, =EV03");
    require(bindings.size() == 2, "only well-formed entries must be kept");
    require(bindings[0].sensorId == "F1" && bindings[0].slotId == "EV01" &&
                bindings[0].channelId == "ch02",
            "channel suffix must be parsed");
    require(bindings[1].channelId.empty(),
            "missing channel must fall back to the default");
}

void testAlarmFlow() {
    std::vector<PublishedMessage> published;
    event::FireAlarmManager manager(
        "cam01", "ch01", "parking/fire",
        event::parseFireSensorBindings("F1=EV01:ch02"),
        [&published](const std::string& topic, const std::string& payload) {
            published.push_back({topic, payload});
            return true;
        });

    const sensor::SensorProtocolParser parser;
    const auto now = std::chrono::system_clock::now();

    auto first = parser.parseFire("FIRE:F1:DETECTED:1", now);
    require(first.has_value(), "fixture frame must parse");
    first->raw = "FIRE:F1:DETECTED:1";
    require(manager.onFireSignal(toSignal(*first)),
            "first detection must publish");
    require(published.size() == 1, "exactly one publish expected");
    require(published[0].topic == "parking/fire/EV01",
            "topic must be <prefix>/<slot_id>");
    require(contains(published[0].payload, "\"slot_id\":\"EV01\""),
            "slot_id must be mapped from the sensor id");
    require(contains(published[0].payload, "\"channel_id\":\"ch02\""),
            "per-binding channel must win over the default");
    require(contains(published[0].payload,
                     "\"event_type\":\"sensor_fire_suspected\""),
            "fire must be published as a candidate, not a confirmation");
    require(contains(published[0].payload, "\"severity\":\"critical\""),
            "detected fire must be critical");
    require(contains(published[0].payload, "\"active\":true"),
            "detected fire must be active");
    require(contains(published[0].payload, "\"ack_state\":\"unacked\""),
            "control room ack state must start unacked");
    require(contains(published[0].payload,
                     "\"raw_payload\":\"FIRE:F1:DETECTED:1\""),
            "raw frame must travel with the alarm as evidence");

    auto repeat = parser.parseFire("FIRE:F1:DETECTED:2", now);
    require(!manager.onFireSignal(toSignal(*repeat)),
            "repeated same state must not republish");
    require(published.size() == 1, "no extra publish for an unchanged state");

    auto stale = parser.parseFire("FIRE:F1:CLEARED:1", now);
    require(!manager.onFireSignal(toSignal(*stale)),
            "out-of-order sequence must be dropped");
    require(published.size() == 1, "stale frame must not publish");

    auto cleared = parser.parseFire("FIRE:F1:CLEARED:3", now);
    require(manager.onFireSignal(toSignal(*cleared)),
            "state change back to cleared must publish");
    require(published.size() == 2, "clear must publish once");
    require(contains(published[1].payload,
                     "\"event_type\":\"sensor_fire_cleared\""),
            "clear event type mismatch");
    require(contains(published[1].payload, "\"active\":false"),
            "cleared fire must not be active");
}

void testUnmappedSensorStillAlarms() {
    std::vector<PublishedMessage> published;
    event::FireAlarmManager manager(
        "cam01", "ch01", "parking/fire", {},
        [&published](const std::string& topic, const std::string& payload) {
            published.push_back({topic, payload});
            return true;
        });

    event::FireSignal signal;
    signal.sensorId = "F9";
    signal.detected = true;
    signal.rawPayload = "FIRE:F9:DETECTED";

    require(manager.onFireSignal(signal),
            "a config gap must not swallow a fire signal");
    require(published.size() == 1, "unmapped sensor must still publish");
    require(published[0].topic == "parking/fire/unmapped",
            "unmapped sensor must not produce an empty topic segment");
    require(contains(published[0].payload, "\"slot_id\":\"\""),
            "unmapped sensor must report an empty slot_id");
}

}  // namespace

int main() {
    try {
        testParser();
        testBindingSpec();
        testAlarmFlow();
        testUnmappedSensorStillAlarms();
    } catch (const std::exception& error) {
        std::cerr << "FireAlarmTest failed: " << error.what() << std::endl;
        return 1;
    }

    std::cout << "FireAlarmTest passed" << std::endl;
    return 0;
}

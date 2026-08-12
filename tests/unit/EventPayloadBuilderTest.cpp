#include "event/CameraEvent.hpp"
#include "event/EventPayloadBuilder.hpp"
#include "event/FireAlarmEvent.hpp"
#include "util/TimeUtil.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// Qt 클라이언트가 이 JSON을 직접 파싱하므로(필드명/타입이 계약), 필드 하나만
// 틀려도 여기선 안 잡히고 Qt 쪽에서 "의도하지 않은 동작"으로만 드러난다.
// 이 테스트는 그 계약을 EventPayloadBuilder 쪽에서 고정해 둔다.

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void testBuildJsonFieldsAndEscaping() {
    event::CameraEvent camera_event;
    camera_event.event_channel_id = "vs-0";
    camera_event.video_source_token = "token-1";
    camera_event.rule_name = "EV01_RULE";
    camera_event.action = "Start";
    camera_event.source_type = "camera";
    camera_event.source_id = "ch01";
    camera_event.event_type = "VEHICLE_DETECTED";
    camera_event.severity = "info";
    camera_event.is_active = true;
    camera_event.iva_area_id = "EV01";
    camera_event.slot_id = "EV01";
    camera_event.is_iva_area_event = true;
    camera_event.timestamp = "2026-08-12T10:00:00+09:00";
    camera_event.raw_topic = "camera/ch01/event";
    camera_event.raw_payload = R"({"a":"b"})";

    const std::string json = event::EventPayloadBuilder::buildJson(
        "cam01", "ch01", camera_event, "/data/snapshots/x.jpg");

    require(contains(json, R"("camera_id":"cam01")"), "camera_id must be present");
    require(contains(json, R"("channel_id":"ch01")"), "channel_id must be present");
    require(contains(json, R"("event_channel_id":"vs-0")"),
            "event_channel_id must be present");
    require(contains(json, R"("rule_name":"EV01_RULE")"),
            "rule_name must be present");
    require(contains(json, R"("action":"Start")"), "action must be present");
    require(contains(json, R"("event_type":"VEHICLE_DETECTED")"),
            "event_type must be present");
    require(contains(json, R"("active":true)"),
            "is_active=true must serialize as JSON boolean true, not string");
    require(contains(json, R"("snapshot_mode":"iva_area_roi")"),
            "is_iva_area_event=true must select iva_area_roi snapshot mode");
    require(contains(json, R"("snapshot_path":"/data/snapshots/x.jpg")"),
            "snapshot_path must be present");
    require(contains(json, R"("ack_state":"unacked")"),
            "new camera events must default to unacked");
    require(contains(json, R"("raw_payload":"{\"a\":\"b\"}")"),
            "embedded quotes in raw_payload must be escaped, not break the JSON");
}

void testBuildJsonNonIvaAreaUsesFullFrameSnapshotMode() {
    event::CameraEvent camera_event;
    camera_event.is_iva_area_event = false;
    camera_event.is_active = false;

    const std::string json = event::EventPayloadBuilder::buildJson(
        "cam01", "ch01", camera_event, "");

    require(contains(json, R"("snapshot_mode":"all_channels_full_size")"),
            "is_iva_area_event=false must select all_channels_full_size");
    require(contains(json, R"("active":false)"),
            "is_active=false must serialize as JSON boolean false");
}

void testBuildFireJsonOpenLifecycle() {
    event::FireSignal signal;
    signal.sensorId = "F1";
    signal.rawPayload = "FIRE:F1:DETECTED";

    const std::string json = event::EventPayloadBuilder::buildFireJson(
        "cam01", "ch01", signal, event::FireAlarmLifecycle::Open,
        "fire-F1-1", "alarm-1");

    require(contains(json, R"("event_type":"FIRE_SUSPECTED")"),
            "Open lifecycle must report FIRE_SUSPECTED event_type");
    require(contains(json, R"("alarm_state":"OPEN")"),
            "Open lifecycle must report OPEN alarm_state");
    require(contains(json, R"("ack_state":"unacked")"),
            "Open lifecycle must default ack_state to unacked");
    require(contains(json, R"("active":true)"),
            "Open lifecycle must be active");
    require(contains(json, R"("severity":"critical")"),
            "an open fire candidate must be reported as critical, "
            "not silently downgraded");
}

void testBuildFireJsonAcknowledgedLifecycle() {
    event::FireSignal signal;
    signal.sensorId = "F1";

    const std::string json = event::EventPayloadBuilder::buildFireJson(
        "cam01", "ch01", signal, event::FireAlarmLifecycle::Acknowledged,
        "fire-F1-1", "alarm-1");

    require(contains(json, R"("event_type":"FIRE_ACKNOWLEDGED")"),
            "Acknowledged lifecycle must report FIRE_ACKNOWLEDGED event_type");
    require(contains(json, R"("alarm_state":"ACKNOWLEDGED")"),
            "Acknowledged lifecycle must report ACKNOWLEDGED alarm_state");
    require(contains(json, R"("ack_state":"acknowledged")"),
            "Acknowledged lifecycle must report acknowledged ack_state");
    require(contains(json, R"("active":true)"),
            "an acknowledged-but-unresolved fire must still be active");
}

void testBuildFireJsonResolvedLifecycle() {
    event::FireSignal signal;
    signal.sensorId = "F1";

    const std::string json = event::EventPayloadBuilder::buildFireJson(
        "cam01", "ch01", signal, event::FireAlarmLifecycle::Resolved,
        "fire-F1-1", "alarm-1");

    require(contains(json, R"("event_type":"FIRE_CLEARED")"),
            "Resolved lifecycle must report FIRE_CLEARED event_type");
    require(contains(json, R"("alarm_state":"RESOLVED")"),
            "Resolved lifecycle must report RESOLVED alarm_state");
    require(contains(json, R"("ack_state":"resolved")"),
            "Resolved lifecycle must report resolved ack_state");
    require(contains(json, R"("active":false)"),
            "a resolved fire must not remain active — a stuck true here "
            "would leave Qt showing a phantom ongoing fire");
    require(contains(json, R"("severity":"info")"),
            "a resolved fire must downgrade severity so it stops paging "
            "the control room as critical");
}

void testBuildFireEventIdPrefersSourceSequence() {
    event::FireSignal signal;
    signal.sensorId = "F1";
    signal.sourceSequence = 42;

    require(event::EventPayloadBuilder::buildFireEventId(signal) == "fire-F1-42",
            "when the sensor frame carries a sequence number, it must be "
            "used verbatim as the event id for de-duplication");
}

void testBuildFireEventIdFallsBackToTimestamp() {
    event::FireSignal detected_signal;
    detected_signal.sensorId = "F1";
    detected_signal.detected = true;
    detected_signal.occurredAt = std::chrono::system_clock::time_point{
        std::chrono::milliseconds(1755000000000)};

    const std::string detected_id =
        event::EventPayloadBuilder::buildFireEventId(detected_signal);
    require(detected_id == "fire-F1-DETECTED-1755000000000",
            "without a sequence number, a detected signal must fall back "
            "to a DETECTED-<epoch_ms> id");

    event::FireSignal cleared_signal = detected_signal;
    cleared_signal.detected = false;
    const std::string cleared_id =
        event::EventPayloadBuilder::buildFireEventId(cleared_signal);
    require(cleared_id == "fire-F1-CLEARED-1755000000000",
            "the same signal marked cleared must produce a different id "
            "prefix, or a clear event could collide with its own alarm");
}

}  // namespace

int main() {
    try {
        testBuildJsonFieldsAndEscaping();
        testBuildJsonNonIvaAreaUsesFullFrameSnapshotMode();
        testBuildFireJsonOpenLifecycle();
        testBuildFireJsonAcknowledgedLifecycle();
        testBuildFireJsonResolvedLifecycle();
        testBuildFireEventIdPrefersSourceSequence();
        testBuildFireEventIdFallsBackToTimestamp();
    } catch (const std::exception& error) {
        std::cerr << "EventPayloadBuilderTest failed: " << error.what()
                  << std::endl;
        return 1;
    }

    std::cout << "EventPayloadBuilderTest passed" << std::endl;
    return 0;
}

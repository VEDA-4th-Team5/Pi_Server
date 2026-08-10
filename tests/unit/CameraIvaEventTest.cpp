#include "app/AppConfig.hpp"
#include "event/CameraEventParser.hpp"
#include "event/IvaEventResolver.hpp"
#include "event/IvaSlotOccupancyAggregator.hpp"
#include "parking/ParkingSlotConfig.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(const bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

parking::ParkingSlotConfig slot(
    std::string slotId, std::string token, std::string rule,
    std::string cameraId = "cam01") {
    parking::ParkingSlotConfig result;
    result.slotId = std::move(slotId);
    result.enabled = true;
    result.sensorId = "HALL-" + result.slotId;
    result.cameraBindings.push_back(
        {std::move(cameraId), std::move(token), std::move(rule), true, 100});
    return result;
}

app::IvaAreaConfig area(std::string slotId, std::string channelId) {
    return {slotId, slotId, std::move(channelId), 0.1, 0.2, 0.3, 0.4, 0};
}

}  // namespace

int main() {
    bool success = true;
    const std::vector<parking::ParkingSlotConfig> slots{
        slot("EV01", "vs-0", "name1"),
        slot("EV02", "VideoSourceToken-1", "name1")};
    const std::vector<app::IvaAreaConfig> areas{
        area("EV01", "ch01"), area("EV02", "ch02")};

    auto first = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/name1/&VideoSourceToken-0", "{\"active\":true}",
        "ch01");
    success &= expect(first.is_iva_area_event, "IvaArea must be classified");
    success &= expect(first.video_source_token == "vs-0",
                      "long token must normalize to vs-0");
    success &= expect(first.event_channel_id == "ch01",
                      "VideoSourceToken-0 must map to ch01");
    std::string error;
    auto firstTarget = event::IvaEventResolver::resolve(
        "cam01", first, slots, areas, &error);
    success &= expect(firstTarget && firstTarget->slotId == "EV01",
                      "token 0/name1 must resolve EV01: " + error);

    auto second = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/name1/&vs-1", "{\"active\":true}", "ch01");
    success &= expect(second.video_source_token == "vs-1",
                      "short token must remain canonical");
    success &= expect(second.event_channel_id == "ch02",
                      "vs-1 must map to ch02");
    error.clear();
    auto secondTarget = event::IvaEventResolver::resolve(
        "cam01", second, slots, areas, &error);
    success &= expect(secondTarget && secondTarget->slotId == "EV02",
                      "same rule on another token must resolve EV02: " + error);

    auto invalid = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/name1/&VideoSourceToken-x", "true", "ch01");
    success &= expect(invalid.video_source_token.empty(),
                      "malformed token must not be accepted");
    error.clear();
    success &= expect(!event::IvaEventResolver::resolve(
                          "cam01", invalid, slots, areas, &error),
                      "malformed token must not fall back to EV01");

    auto missingRule = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/&VideoSourceToken-0", "{\"active\":true}",
        "ch01");
    error.clear();
    success &= expect(!event::IvaEventResolver::resolve(
                          "cam01", missingRule, slots, areas, &error),
                      "missing rule must not map by channel alone");

    auto inactive = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/name1/&VideoSourceToken-0",
        "{\"active\":false}", "ch01");
    success &= expect(!inactive.is_active,
                      "active=false must be recognized as inactive");

    auto multiDigit = event::CameraEventParser::parse(
        "mac/onvif-ej/IvaArea/name1/&VideoSourceToken-10", "true", "ch01");
    success &= expect(multiDigit.video_source_token == "vs-10" &&
                          multiDigit.event_channel_id == "ch11",
                      "multi-digit token must be parsed completely");

    auto customEnter = event::CameraEventParser::parse(
        "cam01/onvif-ej/iva/vs-0/EV01/enter",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"EV01","slot_id":"EV01","event_type":"IVA_AREA","action":"ENTER","active":true})",
        "ch01");
    success &= expect(customEnter.is_smart_parking_iva &&
                          customEnter.protocol_valid &&
                          customEnter.event_type == "camera_iva_area_enter" &&
                          customEnter.is_active && customEnter.action == "ENTER",
                      "custom ENTER publication must be parsed");

    auto customIntrusion = event::CameraEventParser::parse(
        "cam01/onvif-ej/iva/vs-0/EV01/intrusion",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"name1","slot_id":"EV01","event_type":"IVA_AREA","action":"INTRUSION","active":true})",
        "ch01");
    success &= expect(
        customIntrusion.is_smart_parking_iva &&
            customIntrusion.protocol_valid &&
            customIntrusion.event_type == "camera_iva_area_intrusion" &&
            customIntrusion.is_active &&
            customIntrusion.action == "INTRUSION",
        "custom INTRUSION publication must be parsed");

    auto customExit = event::CameraEventParser::parse(
        "cam01/onvif-ej/iva/vs-0/EV01/exit",
        R"({ "schema": "smart-parking-iva-v1", "camera_id": "cam01", "video_source_token": "vs-0", "rule_name": "EV01", "slot_id": "EV01", "event_type": "IVA_AREA", "action": "EXIT", "active": false })",
        "ch01");
    success &= expect(customExit.is_smart_parking_iva &&
                          customExit.protocol_valid &&
                          customExit.event_type == "camera_iva_area_exit" &&
                          !customExit.is_active && customExit.action == "EXIT",
                      "custom EXIT publication with whitespace must be parsed");

    auto tokenMismatch = event::CameraEventParser::parse(
        "cam01/onvif-ej/iva/vs-0/EV01/enter",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-1","rule_name":"EV01","slot_id":"EV01","event_type":"IVA_AREA","action":"ENTER","active":true})",
        "ch01");
    success &= expect(tokenMismatch.is_smart_parking_iva &&
                          !tokenMismatch.protocol_valid,
                      "topic/payload token mismatch must be rejected");

    auto inconsistentExit = event::CameraEventParser::parse(
        "cam01/onvif-ej/iva/vs-0/EV01/exit",
        R"({"schema":"smart-parking-iva-v1","camera_id":"cam01","video_source_token":"vs-0","rule_name":"EV01","slot_id":"EV01","event_type":"IVA_AREA","action":"EXIT","active":true})",
        "ch01");
    success &= expect(!inconsistentExit.protocol_valid,
                      "EXIT active=true must be rejected");

    auto ambiguousSlots = slots;
    ambiguousSlots.push_back(slot("EV03", "vs-0", "name1"));
    auto ambiguousAreas = areas;
    ambiguousAreas.push_back(area("EV03", "ch03"));
    error.clear();
    success &= expect(!event::IvaEventResolver::resolve(
                          "cam01", first, ambiguousSlots, ambiguousAreas,
                          &error) &&
                          error.find("ambiguous") != std::string::npos,
                      "ambiguous mapping must be rejected");

    auto aggregateSlots = std::vector<parking::ParkingSlotConfig>{
        slot("EV01", "vs-0", "name1"),
        slot("EV02", "vs-0", "name2"),
        slot("EV03", "vs-0", "name3"),
        slot("EV04", "vs-0", "name4")};
    event::IvaSlotOccupancyAggregator aggregator(aggregateSlots);
    auto aggregate = aggregator.update(
        "EV01", "cam01", "vs-0", "name1", true);
    success &= expect(aggregate && aggregate->occupied &&
                          aggregate->activeAreaCount == 1 &&
                          aggregate->configuredAreaCount == 1,
                      "active name1 must occupy EV01");
    aggregate = aggregator.update(
        "EV01", "cam01", "vs-0", "name1", false);
    success &= expect(aggregate && !aggregate->occupied &&
                          aggregate->knownAreaCount == 1,
                      "inactive name1 must clear EV01");
    aggregate = aggregator.update(
        "EV04", "cam01", "VideoSourceToken-0", "name4", false);
    success &= expect(aggregate && !aggregate->occupied,
                      "inactive name4 must clear EV04");
    aggregate = aggregator.update(
        "EV04", "cam01", "vs-0", "name4", true);
    success &= expect(aggregate && aggregate->occupied,
                      "active name4 must occupy EV04");
    success &= expect(!aggregator.update(
                          "EV04", "cam01", "vs-0", "name4", true),
                      "duplicate IVA state must not emit another transition");

    const std::string bundledNotifications = R"(
      <wsnt:Notify>
        <wsnt:NotificationMessage>
          <tt:Source><tt:SimpleItem Name="Rule" Value="name1"/></tt:Source>
          <tt:Data><tt:SimpleItem Name="Action" Value="Intrusion"/>
                   <tt:SimpleItem Name="State" Value="true"/></tt:Data>
        </wsnt:NotificationMessage>
        <wsnt:NotificationMessage>
          <tt:Source><tt:SimpleItem Name="Rule" Value="name1"/></tt:Source>
          <tt:Data><tt:SimpleItem Name="Action" Value="Exit"/>
                   <tt:SimpleItem Name="State" Value="false"/></tt:Data>
        </wsnt:NotificationMessage>
      </wsnt:Notify>)";
    const auto bundled = event::CameraEventParser::parseMany(
        "cam01/onvif-ej/OpenApp/WiseAI/IvaArea/&vs-0",
        bundledNotifications, "ch01");
    success &= expect(bundled.size() == 2,
                      "two NotificationMessage blocks must produce two events");
    if (bundled.size() == 2) {
        success &= expect(
            bundled[0].event_type == "camera_iva_area_intrusion" &&
                bundled[0].is_active &&
                bundled[0].raw_payload.find("name1") != std::string::npos,
            "first bundled notification must preserve name1 intrusion");
        success &= expect(
                bundled[1].event_type == "camera_iva_area_exit" &&
                !bundled[1].is_active &&
                bundled[1].raw_payload.find("name1") != std::string::npos,
            "second bundled notification must preserve name1 exit");
    }

    if (!success) return EXIT_FAILURE;
    std::cout << "camera IVA event tests passed\n";
    return EXIT_SUCCESS;
}

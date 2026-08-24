#include "app/AppConfig.hpp"
#include "camera/OnvifIvaEventSource.hpp"
#include "event/OnvifIvaEventAdapter.hpp"
#include "parking/ParkingSlotConfig.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

parking::ParkingSlotConfig slot(const std::string& slot_id,
                                const std::string& token,
                                const std::string& rule) {
    return {slot_id, true, "ev_charging", "HALL-" + slot_id,
            {{"cam01", token, rule, true, 100}}};
}

app::IvaAreaConfig area(const std::string& slot_id,
                        const std::string& rule,
                        const std::string& channel,
                        const int snapshot_channel) {
    return {slot_id, rule, channel, 0.1, 0.2, 0.3, 0.4,
            snapshot_channel, true};
}

}  // namespace

int main() {
    try {
        const std::string xml = R"XML(
<tev:PullMessagesResponse xmlns:tev="http://www.onvif.org/ver10/events/wsdl"
 xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2"
 xmlns:tt="http://www.onvif.org/ver10/schema">
 <wsnt:NotificationMessage>
  <wsnt:Topic>tns:OpenApp/WiseAI/IvaArea</wsnt:Topic>
  <wsnt:Message>
   <tt:Message UtcTime="2026-08-24T03:10:00.125+00:00" PropertyOperation="Changed">
    <tt:Source><tt:SimpleItem Name="VideoSourceToken" Value="vs-0"/>
     <tt:SimpleItem Name="RuleName" Value="name1"/></tt:Source>
    <tt:Data><tt:SimpleItem Name="State" Value="true"/>
     <tt:SimpleItem Name="ObjectId" Value="100"/>
     <tt:SimpleItem Name="Action" Value="Intrusion"/></tt:Data>
   </tt:Message>
  </wsnt:Message>
 </wsnt:NotificationMessage>
 <wsnt:NotificationMessage>
  <wsnt:Topic>tns:OpenApp/WiseAI/IvaArea</wsnt:Topic>
  <wsnt:Message>
   <tt:Message UtcTime="2026-08-24T03:10:10.250Z" PropertyOperation="Changed">
    <tt:Source><tt:SimpleItem Name="VideoSourceToken" Value="vs-2"/>
     <tt:SimpleItem Name="RuleName" Value="name5"/></tt:Source>
    <tt:Data><tt:SimpleItem Name="State" Value="true"/>
     <tt:SimpleItem Name="ObjectId" Value="200"/>
     <tt:SimpleItem Name="Action" Value="Exit"/></tt:Data>
   </tt:Message>
  </wsnt:Message>
 </wsnt:NotificationMessage>
 <wsnt:NotificationMessage>
  <wsnt:Topic>tns:VideoSource/MotionAlarm</wsnt:Topic>
  <wsnt:Message><tt:Message UtcTime="2026-08-24T03:10:11Z">
   <tt:Source><tt:SimpleItem Name="VideoSourceToken" Value="vs-3"/></tt:Source>
  </tt:Message></wsnt:Message>
 </wsnt:NotificationMessage>
</tev:PullMessagesResponse>)XML";

        const auto events = camera::OnvifIvaEventSource::parseEvents(xml);
        require(events.size() == 2,
                "only the two IvaArea notifications must be parsed");
        require(events[0].videoSourceToken == "vs-0" &&
                    events[0].ruleName == "name1" &&
                    events[0].objectId == "100" &&
                    events[0].action == "Intrusion",
                "CH1 Intrusion fields were not preserved");
        require(events[1].videoSourceToken == "vs-2" &&
                    events[1].ruleName == "name5" &&
                    events[1].objectId == "200" &&
                    events[1].action == "Exit",
                "CH3 Exit fields were not preserved");

        app::AppConfig config{};
        config.camera_id = "cam01";
        config.parking_occupancy_source = "HYBRID_OR";
        config.iva_areas = {
            area("EV01", "name1", "ch01", 0),
            area("EV05", "name5", "ch03", 2)};
        const std::vector<parking::ParkingSlotConfig> slots{
            slot("EV01", "vs-0", "name1"),
            slot("EV05", "vs-2", "name5")};

        std::string error;
        const auto intrusion = event::OnvifIvaEventAdapter::adapt(
            events[0], config, slots, &error);
        require(intrusion && intrusion->slotId == "EV01" &&
                    intrusion->channelId == "ch01" &&
                    intrusion->videoSourceToken == "vs-0" &&
                    intrusion->action == event::IvaOccupancyAction::Intrusion &&
                    intrusion->occupancyAuthority &&
                    intrusion->occurredAtFromSource &&
                    intrusion->sourceTransport == "camera-onvif" &&
                    !intrusion->sourceIdentity.empty(),
                "CH1 Intrusion was not adapted to the existing occupancy path: " +
                    error);

        error.clear();
        const auto exit = event::OnvifIvaEventAdapter::adapt(
            events[1], config, slots, &error);
        require(exit && exit->slotId == "EV05" &&
                    exit->channelId == "ch03" &&
                    exit->videoSourceToken == "vs-2" &&
                    exit->ruleName == "name5" &&
                    exit->objectId == "200" &&
                    exit->action == event::IvaOccupancyAction::Exit &&
                    exit->authoritativeExit,
                "CH3 Exit was not adapted to EV05: " + error);
        require(exit->occurredAt - intrusion->occurredAt == 10125ms,
                "camera UtcTime precision/order was not preserved");

        const auto repeated = event::OnvifIvaEventAdapter::adapt(
            events[1], config, slots, &error);
        require(repeated &&
                    repeated->sourceIdentity == exit->sourceIdentity,
                "the same camera event must have a stable idempotency key");

        require(camera::OnvifIvaEventSource::deriveEventEndpoint(
                    "http://172.20.32.99/opensdk/cv_snapshot_api") ==
                    "http://172.20.32.99/onvif/event_service",
                "ONVIF endpoint derivation failed");

        std::cout << "ONVIF IVA event source tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

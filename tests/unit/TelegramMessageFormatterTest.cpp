#include "notification/TelegramMessageFormatter.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

notification::TelegramMessage message(const std::string& payload,
                                      const bool retain = false) {
    return {"parking/v1/events/EV01", payload, 1, retain};
}

}  // namespace

int main() {
    const notification::TelegramMessageFormatter formatter;

    const auto fire = message(
        R"({"event_id":"fire-ch01-1","event_type":"FIRE_SUSPECTED","channel_id":"ch01","source_id":"FLAME01","timestamp":"2026-08-17T12:30:00Z","raw_payload":"secret-debug-data"})");
    require(formatter.shouldNotify(fire), "fire alert was filtered");
    require(formatter.eventId(fire) == "fire-ch01-1",
            "fire event identity was not extracted");
    const std::string fire_text = formatter.format(fire);
    require(fire_text.find("화재 의심 감지") != std::string::npos,
            "fire title is not user-facing");
    require(fire_text.find("ch01") != std::string::npos,
            "fire channel is missing");
    require(fire_text.find("2026-08-17 21:30:00 KST") != std::string::npos,
            "UTC fire timestamp was not converted to Korea time");
    require(fire_text.find("Topic:") == std::string::npos &&
                fire_text.find("QoS:") == std::string::npos &&
                fire_text.find("Payload:") == std::string::npos &&
                fire_text.find("secret-debug-data") == std::string::npos,
            "internal MQTT fields leaked into the fire alert");
    require(!formatter.shouldNotify(message(fire.payload, true)),
            "retained state produced a Telegram alert");

    const auto non_ev = message(
        R"({"event_type":"NON_EV_ALERT","slot_id":"EV01","plate_number":"315다8504","timestamp":"2026-08-17T12:31:00Z"})");
    require(formatter.shouldNotify(non_ev), "NON_EV alert was filtered");
    const std::string non_ev_text = formatter.format(non_ev);
    require(non_ev_text.find("EV01") != std::string::npos &&
                non_ev_text.find("315다8504") != std::string::npos &&
                non_ev_text.find("2026-08-17 21:31:00 KST") !=
                    std::string::npos,
            "NON_EV alert omitted operator information");

    const auto overstay = message(
        R"({"event_type":"OVERTIME_VIOLATION","slot_id":"EV02","plate_number":"","occupied_seconds":3600,"timestamp":"2026-08-17T12:32:00Z"})");
    require(formatter.shouldNotify(overstay), "overstay alert was filtered");
    const std::string overstay_text = formatter.format(overstay);
    require(overstay_text.find("장기 점유 경고") != std::string::npos &&
                overstay_text.find("60분") != std::string::npos &&
                overstay_text.find("2026-08-17 21:32:00 KST") !=
                    std::string::npos,
            "overstay alert omitted threshold information");

    const auto already_kst = message(
        R"({"event_type":"OVERTIME_VIOLATION","slot_id":"EV02","timestamp":"2026-08-17T21:32:00+09:00"})");
    require(formatter.format(already_kst).find(
                "2026-08-17 21:32:00 KST") != std::string::npos,
            "offset timestamp was converted twice");

    const auto warning = message(
        R"({"event_type":"SENSOR_ERROR","severity":"WARNING","error_code":"SENSOR_MESSAGE_INVALID"})");
    require(!formatter.shouldNotify(warning),
            "warning-level sensor noise produced a Telegram alert");
    const auto critical = message(
        R"({"event_type":"SENSOR_ERROR","severity":"CRITICAL","slot_id":"EV03","error_code":"UART_OPEN_FAILED","timestamp":"2026-08-17T12:33:00Z"})");
    require(formatter.shouldNotify(critical),
            "critical sensor error was filtered");
    require(formatter.format(critical).find("UART_OPEN_FAILED") !=
                std::string::npos,
            "critical sensor error omitted its stable error code");

    require(!formatter.shouldNotify(message(
                R"({"event_type":"SLOT_OCCUPIED","slot_id":"EV01"})")),
            "ordinary occupancy produced a Telegram alert");
    require(!formatter.shouldNotify(message("not-json")),
            "invalid JSON produced a Telegram alert");

    std::cout << "Telegram message formatter tests passed\n";
    return 0;
}

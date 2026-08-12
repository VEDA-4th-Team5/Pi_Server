#include "database/EventDatabase.hpp"
#include "http/ParkingHttpServer.hpp"
#include "settings/OverstayThresholdService.hpp"
#include "settings/ParkingRoiSettingsService.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
bool initializeDatabase(const fs::path& path, const fs::path& image_path) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) return false;
    const std::string sql =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE VEHICLE(vehicle_id INTEGER PRIMARY KEY,plate_number TEXT UNIQUE NOT NULL,is_ev INTEGER NOT NULL);"
        "CREATE TABLE PARKING_SLOT(slot_id TEXT PRIMARY KEY,slot_type TEXT NOT NULL,status TEXT NOT NULL,sensor_type TEXT,updated_at TEXT);"
        "CREATE TABLE PARKING_SESSION(session_id INTEGER PRIMARY KEY,vehicle_id INTEGER,slot_id TEXT NOT NULL,plate_number TEXT,entry_time TEXT,exit_time TEXT,duration_sec INTEGER,status TEXT NOT NULL);"
        "CREATE TABLE IMAGE_LOG(image_id INTEGER PRIMARY KEY,session_id INTEGER,original_image_path TEXT,enhanced_image_path TEXT,enhancement_type TEXT,ocr_result TEXT,captured_at TEXT);"
        "CREATE TABLE EVENT_LOG(event_id INTEGER PRIMARY KEY,session_id INTEGER,slot_id TEXT,event_type TEXT NOT NULL,message TEXT,created_at TEXT,handled INTEGER);"
        "INSERT INTO VEHICLE VALUES(1,'223로2825',1);"
        "INSERT INTO PARKING_SLOT VALUES('EV01','EV_CHARGING','OCCUPIED','CAMERA','2026-07-15 12:00:00');"
        "INSERT INTO PARKING_SLOT VALUES('EV02','EV_CHARGING','VACANT','CAMERA','2026-07-15 12:00:00');"
        "INSERT INTO PARKING_SESSION VALUES(7,1,'EV01','223로2825','2026-07-15 12:00:00',NULL,0,'ACTIVE');"
        "INSERT INTO IMAGE_LOG VALUES(9,7,'" + image_path.string() +
        "',NULL,'BESTSHOT_PLATE','223로2825','2026-07-15 12:00:01');";
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::cerr << (error == nullptr ? "SQLite init failed" : error) << '\n';
        sqlite3_free(error);
    }
    sqlite3_close(database);
    return result == SQLITE_OK;
}
bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pi-server-http-api-test-" + std::to_string(getpid()));
    const fs::path data = root / "data";
    const fs::path image = data / "snapshots" / "plate.jpg";
    const fs::path db_path = root / "parking.db";
    fs::create_directories(image.parent_path());
    { std::ofstream output(image, std::ios::binary); output << "fake-jpeg-for-http-test"; }
    if (!initializeDatabase(db_path, image)) return 1;

    database::EventDatabase database;
    if (!database.open(db_path.string())) return 1;
    // 구형 IMAGE_LOG에서 시작해도 migration이 반복 실행 가능해야 한다.
    database.migrateRuntimeSchema();
    database.migrateRuntimeSchema();
    settings::OverstayThresholdService overstay_settings(database);
    if (!overstay_settings.initialize()) return 1;
    const std::vector<app::IvaAreaConfig> roi_bootstrap{
        {"EV01", "name1", "ch01", 0.0, 0.0, 1.0, 1.0, 0},
        {"EV02", "name2", "ch01", 0.1, 0.1, 0.4, 0.4, 0},
        // 알려진 슬롯이지만 SQLite/환경에 ROI가 없으면 전체 프레임으로
        // 대체하지 않고 GET 404를 반환해야 한다.
        {"EV03", "name3", "ch01", 0.0, 0.0, 1.0, 1.0, 0, false}};
    settings::ParkingRoiSettingsService roi_settings(database, roi_bootstrap);
    if (!roi_settings.initialize()) return 1;
    const fs::path start_evidence = data / "snapshots" / "start.jpg";
    const fs::path overstay_evidence = data / "snapshots" / "overstay.jpg";
    { std::ofstream output(start_evidence, std::ios::binary); output << "start"; }
    { std::ofstream output(overstay_evidence, std::ios::binary); output << "overstay"; }
    if (database.insertEvidenceImage(
            7, start_evidence.string(), "OCCUPANCY_START_EVIDENCE",
            "2026-07-15T12:00:02") !=
        database::EvidenceInsertResult::Inserted) return 1;
    if (database.insertEvidenceImage(
            7, overstay_evidence.string(), "OVERSTAY_EVIDENCE",
            "2026-07-15T13:00:00") !=
        database::EvidenceInsertResult::Inserted) return 1;
    http::ServerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 18081;
    config.data_root = data.string();
    const char* tls_cert = std::getenv("HTTP_TEST_TLS_CERT");
    const char* tls_key = std::getenv("HTTP_TEST_TLS_KEY");
    const bool test_tls = tls_cert != nullptr && tls_key != nullptr;
    if (test_tls) {
        config.tls_certificate_path = tls_cert;
        config.tls_private_key_path = tls_key;
    }
    http::ParkingHttpServer server(database, config, &overstay_settings,
                                   &roi_settings);
    if (!server.start()) return 1;
    httplib::Client client(std::string(test_tls ? "https://" : "http://") +
                           "127.0.0.1:" + std::to_string(config.port));
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (test_tls) client.enable_server_certificate_verification(false);
#endif
    bool success = true;
    auto health = client.Get("/api/v1/health");
    success &= expect(health && health->status == 200, "health endpoint");
    auto threshold = client.Get("/api/v1/settings/overstay-threshold");
    success &= expect(threshold && threshold->status == 200 &&
        nlohmann::json::parse(threshold->body).at("thresholdSeconds") == 3600,
        "default overstay threshold");
    auto updated = client.Put("/api/v1/settings/overstay-threshold",
                              "{\"thresholdSeconds\":1800}",
                              "application/json");
    success &= expect(updated && updated->status == 200 &&
        nlohmann::json::parse(updated->body).at("applyPolicy") ==
            "ACTIVE_AND_NEW_SESSIONS",
        "PUT overstay threshold");
    threshold = client.Get("/api/settings/overstay-threshold");
    success &= expect(threshold && threshold->status == 200 &&
        nlohmann::json::parse(threshold->body).at("thresholdSeconds") == 1800,
        "updated threshold and compatibility route");
    for (const std::string body : {
             "{\"thresholdSeconds\":59}",
             "{\"thresholdSeconds\":86401}",
             "{\"thresholdSeconds\":\"1800\"}", "not-json"}) {
        auto invalid = client.Put("/api/v1/settings/overstay-threshold", body,
                                  "application/json");
        success &= expect(invalid && invalid->status == 400,
                          "invalid threshold rejected");
    }
    success &= expect(overstay_settings.thresholdSeconds() == 1800,
                      "invalid PUT did not change setting");
    auto roi_list = client.Get("/api/v1/settings/parking-slots/roi");
    success &= expect(roi_list && roi_list->status == 200 &&
        nlohmann::json::parse(roi_list->body).at("count") == 2 &&
        nlohmann::json::parse(roi_list->body).at("items").at(0).at("revision") == 1,
        "ROI list endpoint");
    auto roi_get = client.Get("/api/v1/settings/parking-slots/ev01/roi");
    success &= expect(roi_get && roi_get->status == 200 &&
        nlohmann::json::parse(roi_get->body).at("roi").at("width") == 1.0 &&
        nlohmann::json::parse(roi_get->body).at("revision") == 1,
        "ROI GET endpoint normalizes slot id");
    const auto missing_roi = client.Get(
        "/api/v1/settings/parking-slots/EV03/roi");
    success &= expect(missing_roi && missing_roi->status == 404 &&
                      !roi_settings.roiForSlot("EV03"),
                      "missing ROI is not replaced by full frame");
    auto roi_put = client.Put(
        "/api/v1/settings/parking-slots/EV01/roi",
        R"({"x":0.25,"y":0.2,"width":0.5,"height":0.6})",
        "application/json");
    success &= expect(roi_put && roi_put->status == 200 &&
        nlohmann::json::parse(roi_put->body).at("appliedImmediately") == true &&
        nlohmann::json::parse(roi_put->body).at("revision") == 2,
        "ROI PUT applies immediately");
    const auto applied_roi = roi_settings.roiForSlot("EV01");
    success &= expect(applied_roi && applied_roi->x == 0.25 &&
                      applied_roi->height == 0.6,
                      "ROI PUT updated in-memory value");
    const auto applied_roi_trace = roi_settings.resolveForUse("EV01");
    success &= expect(applied_roi_trace && applied_roi_trace->revision == 2 &&
                      applied_roi_trace->value.x == 0.25,
                      "ROI PUT updated the revisioned runtime value");
    for (const std::string body : {
             R"({"x":-0.1,"y":0,"width":0.5,"height":0.5})",
             R"({"x":0.8,"y":0,"width":0.5,"height":0.5})",
             R"({"x":0,"y":0,"width":"bad","height":0.5})",
             "not-json"}) {
        const auto invalid = client.Put(
            "/api/v1/settings/parking-slots/EV01/roi", body,
            "application/json");
        success &= expect(invalid && invalid->status == 400,
                          "invalid ROI rejected");
    }
    const auto unknown_roi = client.Put(
        "/api/v1/settings/parking-slots/EV99/roi",
        R"({"x":0,"y":0,"width":1,"height":1})",
        "application/json");
    success &= expect(unknown_roi && unknown_roi->status == 404,
                      "unknown ROI slot rejected");
    const auto configure_missing = client.Put(
        "/api/v1/settings/parking-slots/EV03/roi",
        R"({"x":0.2,"y":0.2,"width":0.3,"height":0.3})",
        "application/json");
    success &= expect(configure_missing && configure_missing->status == 200 &&
                      roi_settings.roiForSlot("EV03").has_value(),
                      "known slot accepts its first explicit ROI");
    success &= expect(roi_settings.roiForSlot("EV01")->x == 0.25,
                      "invalid ROI did not change current value");
    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> clients;
    for (int index = 0; index < 4; ++index) {
        clients.emplace_back([&, index] {
            httplib::Client concurrent_client(
                std::string(test_tls ? "https://" : "http://") +
                "127.0.0.1:" + std::to_string(config.port));
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            if (test_tls)
                concurrent_client.enable_server_certificate_verification(false);
#endif
            for (int request = 0; request < 5; ++request) {
                const int seconds = 1800 + ((index + request) % 4) * 60;
                const auto put = concurrent_client.Put(
                    "/api/v1/settings/overstay-threshold",
                    "{\"thresholdSeconds\":" + std::to_string(seconds) + "}",
                    "application/json");
                const auto get = concurrent_client.Get(
                    "/api/v1/settings/overstay-threshold");
                if (!put || put->status != 200 || !get || get->status != 200)
                    concurrent_ok.store(false);
            }
        });
    }
    for (auto& thread : clients) thread.join();
    success &= expect(concurrent_ok.load(), "concurrent GET/PUT requests");
    updated = client.Put("/api/v1/settings/overstay-threshold",
                         "{\"thresholdSeconds\":1800}", "application/json");
    success &= expect(updated && updated->status == 200,
                      "deterministic final threshold restore");
    auto slots = client.Get("/api/v1/parking-slots");
    success &= expect(slots && slots->status == 200, "parking slots endpoint");
    if (slots) success &= expect(nlohmann::json::parse(slots->body).at("count") == 2,
                                 "two slots returned");
    auto detail = client.Get("/api/v1/parking-slots/EV01");
    success &= expect(detail && detail->status == 200, "slot detail endpoint");
    if (detail) {
        const auto body = nlohmann::json::parse(detail->body);
        success &= expect(body.at("active_session").at("session_id") == 7,
                          "active session joined");
        success &= expect(body.at("active_session").at("ev_status") == "EV",
                          "EV classification returned");
    }
    auto images = client.Get("/api/v1/parking-sessions/7/images");
    success &= expect(images && images->status == 200, "session images endpoint");
    if (images) {
        const auto items = nlohmann::json::parse(images->body).at("items");
        success &= expect(items.size() == 3, "legacy and two evidence images returned");
        success &= expect(
            items.at(1).at("evidence_reason") == "OCCUPANCY_START_EVIDENCE" &&
            items.at(2).at("evidence_reason") == "OVERSTAY_EVIDENCE",
            "evidence reasons returned in captured_at order");
        success &= expect(!items.at(1).contains("original_image_path"),
                          "absolute path is not exposed");
    }
    auto original = client.Get("/api/v1/images/9/original");
    success &= expect(original && original->status == 200 &&
                      original->body == "fake-jpeg-for-http-test", "original image response");
    auto enhanced = client.Get("/api/v1/images/9/enhanced");
    success &= expect(enhanced && enhanced->status == 404,
                      "missing enhanced image is explicit");
    server.stop();
    settings::OverstayThresholdService reloaded_settings(database);
    success &= expect(reloaded_settings.initialize() &&
                      reloaded_settings.thresholdSeconds() == 1800,
                      "threshold persisted across settings reload");
    settings::ParkingRoiSettingsService reloaded_roi(database, roi_bootstrap);
    success &= expect(reloaded_roi.initialize() &&
                      reloaded_roi.roiForSlot("EV01")->x == 0.25 &&
                      reloaded_roi.roiForSlot("EV01")->height == 0.6 &&
                      reloaded_roi.resolveForUse("EV01")->revision == 2,
                      "ROI persisted across settings reload");
    database.close();
    const auto failed_update = overstay_settings.update(2400);
    success &= expect(!failed_update.success &&
                      overstay_settings.thresholdSeconds() == 1800,
                      "DB failure preserved in-memory threshold");
    const auto failed_roi = roi_settings.update(
        "EV01", {0.1, 0.1, 0.2, 0.2});
    success &= expect(!failed_roi.success &&
                      roi_settings.roiForSlot("EV01")->x == 0.25 &&
                      roi_settings.resolveForUse("EV01")->revision == 2,
                      "DB failure preserved in-memory ROI");
    fs::remove_all(root);
    if (success) std::cout << "HTTP API integration test passed\n";
    return success ? 0 : 1;
}

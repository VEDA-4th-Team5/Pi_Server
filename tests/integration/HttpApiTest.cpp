#include "database/EventDatabase.hpp"
#include "http/ParkingHttpServer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
    http::ParkingHttpServer server(database, config);
    if (!server.start()) return 1;
    httplib::Client client(std::string(test_tls ? "https://" : "http://") +
                           "127.0.0.1:" + std::to_string(config.port));
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (test_tls) client.enable_server_certificate_verification(false);
#endif
    bool success = true;
    auto health = client.Get("/api/v1/health");
    success &= expect(health && health->status == 200, "health endpoint");
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
    auto original = client.Get("/api/v1/images/9/original");
    success &= expect(original && original->status == 200 &&
                      original->body == "fake-jpeg-for-http-test", "original image response");
    auto enhanced = client.Get("/api/v1/images/9/enhanced");
    success &= expect(enhanced && enhanced->status == 404,
                      "missing enhanced image is explicit");
    server.stop();
    database.close();
    fs::remove_all(root);
    if (success) std::cout << "HTTP API integration test passed\n";
    return success ? 0 : 1;
}

#include "app/RuntimeShutdown.hpp"
#include "auth/AuthService.hpp"
#include "database/EventDatabase.hpp"
#include "http/ParkingHttpServer.hpp"
#include "mqtt/MqttEndpoint.hpp"
#include "settings/OverstayThresholdService.hpp"
#include "settings/ParkingRoiSettingsService.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
using namespace std::chrono_literals;

class ManualEvent {
public:
    void signal() {
        {
            std::lock_guard lock(mutex_);
            signaled_ = true;
        }
        condition_.notify_all();
    }

    bool waitFor(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this] { return signaled_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{false};
};

class FakeMqttTransport final : public mqtt::IMqttTransport {
public:
    bool start(const mqtt::MqttConnectionOptions&,
               const std::vector<mqtt::MqttSubscription>&,
               MessageCallback message_callback,
               ObserverCallbacks observer_callbacks) override {
        message_callback_ = std::move(message_callback);
        observer_callbacks_ = std::move(observer_callbacks);
        return true;
    }

    mqtt::MqttPublishResult publish(const std::string&, const std::string&,
                                    int, bool) override {
        return {true, 1};
    }

    bool stopAndJoin() noexcept override {
        stopped_ = true;
        return true;
    }

    void emit(const std::string& payload) {
        if (message_callback_) message_callback_("test/topic", payload);
    }

    bool stopped() const noexcept { return stopped_; }

private:
    MessageCallback message_callback_;
    ObserverCallbacks observer_callbacks_;
    bool stopped_{};
};

bool waitForState(http::ParkingHttpServer& server,
                  const http::HttpServerState expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.state() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

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
void configureTlsClient(httplib::Client& client, bool test_tls,
                        const char* tls_ca) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (!test_tls) return;
    if (tls_ca != nullptr) {
        client.set_ca_cert_path(tls_ca);
        client.enable_server_certificate_verification(true);
    } else {
        client.enable_server_certificate_verification(false);
    }
#else
    (void)client;
    (void)test_tls;
    (void)tls_ca;
#endif
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
    auth::AuthService auth_service(database);
    std::int64_t app_user_id{};
    std::string auth_error;
    if (!auth_service.addUser("operator", "pass",
                              "Parking Operator", &app_user_id,
                              &auth_error)) return 1;
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
    const char* test_listen = std::getenv("HTTP_TEST_LISTEN_ADDRESS");
    config.listen_address = test_listen == nullptr
        ? "127.0.0.1" : std::string(test_listen);
    config.port = 18081;
    config.data_root = data.string();
    const char* tls_cert = std::getenv("HTTP_TEST_TLS_CERT");
    const char* tls_key = std::getenv("HTTP_TEST_TLS_KEY");
    const char* tls_ca = std::getenv("HTTP_TEST_TLS_CA");
    const bool test_tls = tls_cert != nullptr && tls_key != nullptr;
    if (test_tls) {
        config.tls_certificate_path = tls_cert;
        config.tls_private_key_path = tls_key;
    }
    http::ParkingHttpServer server(database, auth_service, config,
                                   &overstay_settings,
                                   &roi_settings);
    if (!server.start()) return 1;
    httplib::Client client(std::string(test_tls ? "https://" : "http://") +
                           "127.0.0.1:" + std::to_string(config.port));
    configureTlsClient(client, test_tls, tls_ca);
    bool success = true;
    {
        http::ServerConfig insecure_config = config;
        insecure_config.listen_address = "0.0.0.0";
        insecure_config.port = 18082;
        insecure_config.tls_certificate_path.clear();
        insecure_config.tls_private_key_path.clear();
        insecure_config.require_tls = true;
        http::ParkingHttpServer insecure_server(
            database, auth_service, insecure_config);
        success &= expect(!insecure_server.start(),
                          "remote HTTP does not bypass required TLS");
    }
    {
        http::ServerConfig invalid_tls_config = config;
        invalid_tls_config.port = 18083;
        invalid_tls_config.tls_certificate_path = "/missing/server.crt";
        invalid_tls_config.tls_private_key_path = "/missing/server.key";
        invalid_tls_config.require_tls = true;
        http::ParkingHttpServer invalid_tls_server(
            database, auth_service, invalid_tls_config);
        success &= expect(!invalid_tls_server.start(),
                          "invalid TLS configuration has no HTTP fallback");
    }
    auto health = client.Get("/api/v1/health");
    success &= expect(health && health->status == 200 &&
        nlohmann::json::parse(health->body).at("success") == true,
        "public health endpoint");
    auto unauthenticated = client.Get("/api/v1/parking-slots");
    success &= expect(unauthenticated && unauthenticated->status == 401 &&
        unauthenticated->get_header_value("Cache-Control") == "no-store",
        "protected endpoint rejects missing token");
    auto malformed_login = client.Post("/api/v1/auth/login", "not-json",
                                        "application/json");
    success &= expect(malformed_login && malformed_login->status == 400,
                      "malformed login request");
    auto wrong_content_type = client.Post(
        "/api/v1/auth/login",
        R"({"accountId":"operator","password":"pass"})",
        "text/plain");
    success &= expect(wrong_content_type && wrong_content_type->status == 400,
                      "non-JSON login content type rejected");
    auto wrong_login = client.Post(
        "/api/v1/auth/login",
        R"({"accountId":"operator","password":"nope"})",
        "application/json");
    success &= expect(wrong_login && wrong_login->status == 401,
                      "wrong password rejected");
    auto login = client.Post(
        "/api/v1/auth/login",
        R"({"accountId":" Operator ","password":"pass"})",
        "application/json");
    success &= expect(login && login->status == 200 &&
        login->get_header_value("Cache-Control") == "no-store",
        "valid login");
    const auto login_body = login
        ? nlohmann::json::parse(login->body) : nlohmann::json{};
    const std::string access_token = login_body.value("accessToken", "");
    success &= expect(login_body.value("success", false) &&
        login_body.value("tokenType", "") == "Bearer" &&
        !access_token.empty() &&
        login_body.at("user").at("id").get<std::int64_t>() == app_user_id &&
        login_body.at("user").at("accountId") == "operator" &&
        login_body.at("user").at("displayName") == "Parking Operator" &&
        !login_body.value("expiresAt", "").empty(),
        "Qt login response contract");
    client.set_default_headers({{"Authorization", "Bearer " + access_token}});
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto failed = client.Post(
            "/api/v1/auth/login",
            R"({"accountId":"rate-limit-user","password":"nope"})",
            "application/json");
        success &= expect(failed && failed->status == 401,
                          "login rate-limit admitted failure");
    }
    auto limited = client.Post(
        "/api/v1/auth/login",
        R"({"accountId":"rate-limit-user","password":"nope"})",
        "application/json");
    int retry_after{};
    if (limited) {
        try {
            retry_after = std::stoi(
                limited->get_header_value("Retry-After"));
        } catch (...) {
            retry_after = 0;
        }
    }
    success &= expect(limited && limited->status == 429 &&
        retry_after >= 1 && retry_after <= 60,
        "login rate-limit response contract");

    auto logout_login = client.Post(
        "/api/v1/auth/login",
        R"({"accountId":"operator","password":"pass"})",
        "application/json");
    const auto logout_body = logout_login
        ? nlohmann::json::parse(logout_login->body) : nlohmann::json{};
    const std::string logout_token = logout_body.value("accessToken", "");
    httplib::Client logout_client(
        std::string(test_tls ? "https://" : "http://") +
        "127.0.0.1:" + std::to_string(config.port));
    configureTlsClient(logout_client, test_tls, tls_ca);
    logout_client.set_default_headers(
        {{"Authorization", "Bearer " + logout_token}});
    auto logout = logout_client.Post("/api/v1/auth/logout", "",
                                     "application/json");
    auto after_logout = logout_client.Get("/api/v1/parking-slots");
    success &= expect(logout_login && logout_login->status == 200 &&
        !logout_token.empty() && logout && logout->status == 200 &&
        after_logout && after_logout->status == 401,
        "logout revokes current Bearer session");
    auto threshold = client.Get("/api/v1/settings/overstay-threshold");
    const auto initial_threshold = threshold
        ? nlohmann::json::parse(threshold->body) : nlohmann::json{};
    success &= expect(threshold && threshold->status == 200 &&
        initial_threshold.at("thresholdSeconds") == 3600 &&
        initial_threshold.at("effectiveSeconds") == 3600 &&
        initial_threshold.at("appliedRevision").get<std::uint64_t>() > 0 &&
        initial_threshold.at("runtimeApplied") == true &&
        initial_threshold.at("runtimeHealthy") == true,
        "default healthy overstay threshold");
    auto updated = client.Put("/api/v1/settings/overstay-threshold",
                              "{\"thresholdSeconds\":1800}",
                              "application/json");
    const auto updated_threshold = updated
        ? nlohmann::json::parse(updated->body) : nlohmann::json{};
    success &= expect(updated && updated->status == 200 &&
        updated_threshold.at("requestedSeconds") == 1800 &&
        updated_threshold.at("effectiveSeconds") == 1800 &&
        updated_threshold.at("runtimeApplied") == true &&
        updated_threshold.at("runtimeHealthy") == true &&
        updated_threshold.at("applyPolicy") == "ACTIVE_AND_NEW_SESSIONS",
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
        nlohmann::json::parse(roi_list->body).at("count") == 2,
        "ROI list endpoint");
    auto roi_get = client.Get("/api/v1/settings/parking-slots/ev01/roi");
    success &= expect(roi_get && roi_get->status == 200 &&
        nlohmann::json::parse(roi_get->body).at("roi").at("width") == 1.0,
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
        nlohmann::json::parse(roi_put->body).at("appliedImmediately") == true,
        "ROI PUT applies immediately");
    const auto applied_roi = roi_settings.roiForSlot("EV01");
    success &= expect(applied_roi && applied_roi->x == 0.25 &&
                      applied_roi->height == 0.6,
                      "ROI PUT updated in-memory value");
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
            configureTlsClient(concurrent_client, test_tls, tls_ca);
            concurrent_client.set_default_headers(
                {{"Authorization", "Bearer " + access_token}});
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

    std::atomic<int> failed_runtime_calls{0};
    overstay_settings.setApplyCallback(
        [&](const std::chrono::milliseconds) {
            ++failed_runtime_calls;
            throw std::runtime_error("injected runtime apply failure");
        });
    auto runtime_failed = client.Put(
        "/api/v1/settings/overstay-threshold",
        "{\"thresholdSeconds\":1860}", "application/json");
    const auto failed_body = runtime_failed
        ? nlohmann::json::parse(runtime_failed->body) : nlohmann::json{};
    success &= expect(runtime_failed && runtime_failed->status == 503 &&
                      failed_runtime_calls.load() == 1 &&
                      failed_body.at("success") == false &&
                      failed_body.at("runtimeApplied") == false &&
                      failed_body.at("runtimeHealthy") == false,
                      "runtime apply failure is not reported as success");
    threshold = client.Get("/api/v1/settings/overstay-threshold");
    const auto unhealthy_body = threshold
        ? nlohmann::json::parse(threshold->body) : nlohmann::json{};
    success &= expect(threshold && threshold->status == 200 &&
                      unhealthy_body.at("thresholdSeconds") == 1860 &&
                      unhealthy_body.at("runtimeApplied") == false &&
                      unhealthy_body.at("runtimeHealthy") == false,
                      "GET exposes unhealthy runtime policy");

    overstay_settings.setApplyCallback(
        [](const std::chrono::milliseconds) {});
    auto runtime_recovered = client.Put(
        "/api/v1/settings/overstay-threshold",
        "{\"thresholdSeconds\":1860}", "application/json");
    const auto recovered_body = runtime_recovered
        ? nlohmann::json::parse(runtime_recovered->body) : nlohmann::json{};
    success &= expect(runtime_recovered && runtime_recovered->status == 200 &&
                      recovered_body.at("runtimeApplied") == true &&
                      recovered_body.at("runtimeHealthy") == true,
                      "same-value retry recovers unhealthy runtime policy");
    updated = client.Put("/api/v1/settings/overstay-threshold",
                         "{\"thresholdSeconds\":1800}", "application/json");
    success &= expect(updated && updated->status == 200,
                      "threshold restored after runtime failure test");

    ManualEvent apply_entered;
    ManualEvent release_apply;
    std::atomic<bool> apply_timed_out{false};
    std::atomic<int> apply_calls{0};
    std::atomic<bool> put_completed{false};
    std::atomic<bool> put_succeeded{false};
    std::atomic<bool> stop_returned{false};
    std::atomic<bool> shutdown_succeeded{false};
    std::atomic<int> evidence_teardown{0};
    std::atomic<int> timer_teardown{0};
    std::atomic<int> target_teardown{0};
    auto mqtt_transport = std::make_unique<FakeMqttTransport>();
    auto* const mqtt_transport_probe = mqtt_transport.get();
    mqtt::MqttEndpoint mqtt_endpoint(std::move(mqtt_transport));
    std::atomic<int> mqtt_application_calls{0};
    success &= expect(
        mqtt_endpoint.bindApplicationHandler(
            [&](const std::string&, const std::string&) {
                ++mqtt_application_calls;
            }) &&
            mqtt_endpoint.start(
                {"http-mqtt-cut-test", "127.0.0.1", 1883, 60}, {}),
        "MQTT endpoint fixture start");
    mqtt_transport_probe->emit("before-close");
    success &= expect(mqtt_application_calls.load() == 1,
                      "MQTT endpoint fixture did not accept initial ingress");
    overstay_settings.setApplyCallback(
        [&](const std::chrono::milliseconds) {
            ++apply_calls;
            apply_entered.signal();
            if (!release_apply.waitFor(5s)) apply_timed_out.store(true);
        });
    std::thread held_put([&] {
        httplib::Client held_client(
            std::string(test_tls ? "https://" : "http://") +
            "127.0.0.1:" + std::to_string(config.port));
        configureTlsClient(held_client, test_tls, tls_ca);
        held_client.set_default_headers(
            {{"Authorization", "Bearer " + access_token}});
        const auto response = held_client.Put(
            "/api/v1/settings/overstay-threshold",
            "{\"thresholdSeconds\":1860}", "application/json");
        put_succeeded.store(response && response->status == 200);
        put_completed.store(true);
    });
    success &= expect(apply_entered.waitFor(2s),
                      "held HTTP PUT entered its runtime callback");

    app::RuntimeShutdownHooks shutdown_hooks;
    shutdown_hooks.stopHttp = [&] {
        server.closeIngress();
        mqtt_endpoint.closeIngress();
        if (!server.stop()) throw std::runtime_error("HTTP stop failed");
    };
    shutdown_hooks.quiesceMqttApplication = [&] {
        if (!mqtt_endpoint.quiesceIngress())
            throw std::runtime_error("MQTT application drain failed");
    };
    shutdown_hooks.drainEvidence = [&] { ++evidence_teardown; };
    shutdown_hooks.drainTimer = [&] { ++timer_teardown; };
    shutdown_hooks.destroyCallbackTargets = [&] { ++target_teardown; };
    shutdown_hooks.stopMqtt = [&] {
        if (!mqtt_endpoint.stop())
            throw std::runtime_error("MQTT stop failed");
    };
    app::RuntimeShutdown runtime_shutdown(std::move(shutdown_hooks));
    std::thread stopper([&] {
        shutdown_succeeded.store(runtime_shutdown.shutdown());
        stop_returned.store(true, std::memory_order_release);
    });
    success &= expect(waitForState(server, http::HttpServerState::Quiescing),
                      "HTTP server entered Quiescing state");
    success &= expect(
        mqtt_endpoint.state() == mqtt::MqttEndpointState::Quiescing,
        "MQTT admission stayed open while HTTP drain was blocked");
    success &= expect(!stop_returned.load(std::memory_order_acquire),
                      "HTTP stop returned while an accepted handler was active");
    success &= expect(!put_completed.load(),
                      "held HTTP request completed before callback release");
    success &= expect(evidence_teardown.load() == 0 &&
                      timer_teardown.load() == 0 &&
                      target_teardown.load() == 0,
                      "runtime dependencies were torn down before HTTP join");
    mqtt_transport_probe->emit("late-while-http-held");
    success &= expect(mqtt_application_calls.load() == 1,
                      "MQTT mutation entered after the combined admission cut");

    std::atomic<bool> late_rejected{false};
    std::thread late_put([&] {
        httplib::Client late_client(
            std::string(test_tls ? "https://" : "http://") +
            "127.0.0.1:" + std::to_string(config.port));
        configureTlsClient(late_client, test_tls, tls_ca);
        late_client.set_default_headers(
            {{"Authorization", "Bearer " + access_token}});
        const auto response = late_client.Put(
            "/api/v1/settings/overstay-threshold",
            "{\"thresholdSeconds\":1920}", "application/json");
        late_rejected.store(!response || response->status == 503);
    });
    release_apply.signal();
    held_put.join();
    late_put.join();
    stopper.join();
    success &= expect(!apply_timed_out.load(),
                      "HTTP shutdown test callback reached its watchdog");
    success &= expect(put_succeeded.load(),
                      "accepted HTTP PUT did not finish during shutdown");
    success &= expect(late_rejected.load() && apply_calls.load() == 1 &&
                      overstay_settings.thresholdSeconds() == 1860,
                      "request admitted after HTTP Quiescing mutated runtime");
    success &= expect(stop_returned.load() &&
                      server.state() == http::HttpServerState::Stopped &&
                      mqtt_endpoint.state() ==
                          mqtt::MqttEndpointState::Stopped &&
                      mqtt_transport_probe->stopped(),
                      "HTTP/MQTT endpoints did not finish in Stopped state");
    success &= expect(shutdown_succeeded.load() &&
                      evidence_teardown.load() == 1 &&
                      timer_teardown.load() == 1 &&
                      target_teardown.load() == 1,
                      "runtime teardown did not resume exactly once after HTTP join");
    success &= expect(runtime_shutdown.shutdown(),
                      "runtime shutdown was not idempotent");
    overstay_settings.setApplyCallback({});
    success &= expect(overstay_settings.update(1800).success,
                      "threshold restore after HTTP shutdown");
    settings::OverstayThresholdService reloaded_settings(database);
    success &= expect(reloaded_settings.initialize() &&
                      reloaded_settings.thresholdSeconds() == 1800,
                      "threshold persisted across settings reload");
    settings::ParkingRoiSettingsService reloaded_roi(database, roi_bootstrap);
    success &= expect(reloaded_roi.initialize() &&
                      reloaded_roi.roiForSlot("EV01")->x == 0.25 &&
                      reloaded_roi.roiForSlot("EV01")->height == 0.6,
                      "ROI persisted across settings reload");
    std::atomic<int> db_failure_apply_calls{0};
    overstay_settings.setApplyCallback(
        [&](const std::chrono::milliseconds) {
            ++db_failure_apply_calls;
        });
    database.close();
    const auto failed_update = overstay_settings.update(2400);
    success &= expect(!failed_update.success &&
                      overstay_settings.thresholdSeconds() == 1800 &&
                      db_failure_apply_calls.load() == 0,
                      "DB failure preserved threshold and released no runtime apply");
    const auto failed_roi = roi_settings.update(
        "EV01", {0.1, 0.1, 0.2, 0.2});
    success &= expect(!failed_roi.success &&
                      roi_settings.roiForSlot("EV01")->x == 0.25,
                      "DB failure preserved in-memory ROI");
    fs::remove_all(root);
    if (success) std::cout << "HTTP API integration test passed\n";
    return success ? 0 : 1;
}

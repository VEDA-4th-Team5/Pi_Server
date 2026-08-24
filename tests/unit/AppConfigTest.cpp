#include "app/AppConfig.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void setValue(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

void clearValue(const char* key) {
#ifdef _WIN32
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

bool require(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

}

int main() {
    setValue("PI_SERVER_ROOT", "/tmp/pi-server-test-root");
    setValue("PARKING_SLOT_CONFIG", "config/custom_slots.json");
    setValue("PARKING_SLOTS_CONFIG", "config/legacy_slots.json");
    setValue("EVENT_DB_PATH", "data/db/custom.sqlite3");
    setValue("SNAPSHOT_DIR", "data/custom_snapshots");
    setValue("HTTP_DATA_ROOT", "data/http");
    setValue("HTTP_TLS_CERT_PATH", "certs/server.crt");
    setValue("HTTP_TLS_KEY_PATH", "certs/server.key");
    setValue("HTTP_REQUIRE_TLS", "false");
    setValue("AUTH_SESSION_TTL_SECONDS", "30000");
    setValue("AUTH_LOGIN_WINDOW_SECONDS", "240");
    setValue("AUTH_LOGIN_MAX_FAILURES", "4");
    setValue("AUTH_LOGIN_COOLDOWN_SECONDS", "90");
    setValue("PARKING_OCCUPANCY_SOURCE", "hybrid_or");
    setValue("PARKING_ALERT_DRIVER_ENABLED", "true");
    setValue("PARKING_ALERT_DEVICE_PATH", "/dev/test-parking-alert");
    setValue("PARKING_ALERT_SLOT_MAP", "EV01:7,EV02:8");
    clearValue("PARKING_OCCUPANCY_CONFIRM_MS");

    const app::AppConfig config = app::AppConfig::loadFromEnv();
    const std::string root = std::filesystem::path("/tmp/pi-server-test-root").string();
    bool ok = true;
    ok &= require(config.parking_slot_config_path ==
                      (std::filesystem::path(root) / "config/custom_slots.json")
                          .lexically_normal().string(),
                  "PARKING_SLOT_CONFIG must be rooted at PI_SERVER_ROOT");
    ok &= require(config.parking_slots_config_path ==
                      config.parking_slot_config_path,
                  "legacy plural slot config field must follow the canonical path");
    ok &= require(config.db_path ==
                      (std::filesystem::path(root) / "data/db/custom.sqlite3")
                          .lexically_normal().string(),
                  "EVENT_DB_PATH must be rooted at PI_SERVER_ROOT");
    ok &= require(config.snapshot_dir ==
                      (std::filesystem::path(root) / "data/custom_snapshots")
                          .lexically_normal().string(),
                  "SNAPSHOT_DIR must be rooted at PI_SERVER_ROOT");
    ok &= require(config.http_data_root ==
                      (std::filesystem::path(root) / "data/http")
                          .lexically_normal().string(),
                  "HTTP_DATA_ROOT must be rooted at PI_SERVER_ROOT");
    ok &= require(config.http_tls_certificate_path ==
                      (std::filesystem::path(root) / "certs/server.crt")
                          .lexically_normal().string(),
                  "HTTP_TLS_CERT_PATH must be rooted at PI_SERVER_ROOT");
    ok &= require(config.http_tls_private_key_path ==
                      (std::filesystem::path(root) / "certs/server.key")
                          .lexically_normal().string(),
                  "HTTP_TLS_KEY_PATH must be rooted at PI_SERVER_ROOT");
    ok &= require(!config.http_require_tls &&
                      config.auth_session_ttl_seconds == 30000 &&
                      config.auth_login_window_seconds == 240 &&
                      config.auth_login_max_failures == 4 &&
                      config.auth_login_cooldown_seconds == 90,
                  "authentication settings must preserve explicit values");
    ok &= require(config.parking_occupancy_source == "HYBRID_OR",
                  "HYBRID_OR occupancy policy must be accepted");
    ok &= require(config.parking_occupancy_confirm_ms == 5000,
                  "Hall occupancy confirmation must default to five seconds");
    ok &= require(config.parking_alert_driver_enabled &&
                      config.parking_alert_device_path ==
                          "/dev/test-parking-alert" &&
                      config.parking_alert_slot_map == "EV01:7,EV02:8",
                  "parking alert driver settings must preserve explicit values");

    clearValue("PARKING_SLOT_CONFIG");
    const app::AppConfig legacy_config = app::AppConfig::loadFromEnv();
    ok &= require(legacy_config.parking_slot_config_path ==
                      (std::filesystem::path(root) / "config/legacy_slots.json")
                          .lexically_normal().string(),
                  "legacy PARKING_SLOTS_CONFIG must remain supported");
    clearValue("PARKING_SLOTS_CONFIG");
    clearValue("EVENT_DB_PATH");
    clearValue("SNAPSHOT_DIR");
    clearValue("HTTP_DATA_ROOT");
    clearValue("HTTP_TLS_CERT_PATH");
    clearValue("HTTP_TLS_KEY_PATH");
    clearValue("HTTP_REQUIRE_TLS");
    clearValue("AUTH_SESSION_TTL_SECONDS");
    clearValue("AUTH_LOGIN_WINDOW_SECONDS");
    clearValue("AUTH_LOGIN_MAX_FAILURES");
    clearValue("AUTH_LOGIN_COOLDOWN_SECONDS");
    clearValue("PARKING_OCCUPANCY_SOURCE");
    clearValue("PARKING_ALERT_DRIVER_ENABLED");
    clearValue("PARKING_ALERT_DEVICE_PATH");
    clearValue("PARKING_ALERT_SLOT_MAP");
    clearValue("PI_SERVER_ROOT");
    return ok ? 0 : 1;
}

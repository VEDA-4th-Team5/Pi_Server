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
    clearValue("CAMERA_RTSP");
    clearValue("CAMERA_RTSP_CH1");
    setValue("CAMERA_RTSP_CH2", "rtsp://camera/1/profile2/media.smp");
    clearValue("CAMERA_RTSP_CH3");
    clearValue("CAMERA_RTSP_CH4");
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
    setValue("ENTRANCE_ENABLED", "true");
    setValue("ENTRANCE_OUTPUT_ROOT", "data/test_entrance");
    setValue("ENTRANCE_SOURCE_CHANNEL_ID", "ch02");
    setValue("ENTRANCE_CHANNEL_ID", "ch02");
    setValue("ENTRANCE_OBJECT_TTL_SECONDS", "90");
    setValue("ENTRANCE_IMAGE_DEDUP_WINDOW_SECONDS", "25");
    setValue("ENTRANCE_IMAGE_DEDUP_PHASH_THRESHOLD", "7");
    setValue("ENTRANCE_DELETE_ARTIFACTS_ON_SUCCESS", "true");
    setValue("ENTRANCE_FAILURE_RETENTION_HOURS", "12");
    setValue("ENTRANCE_PLATE_MATCH_WINDOW_MINUTES", "45");
    setValue("ENTRANCE_PLATE_MATCH_MIN_CONFIDENCE", "0.9");
    setValue("ENTRANCE_EV_ANALYSIS_ENABLED", "true");
    setValue("ENTRANCE_EV_WORKER_SCRIPT", "tools/test_ev_worker.py");
    setValue("ENTRANCE_EV_TIMEOUT_MS", "7000");
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
    ok &= require(config.entrance_enabled &&
                      config.entrance_source_channel_id == "ch02" &&
                      config.entrance_channel_id == "ch02" &&
                      config.entrance_object_ttl_seconds == 90 &&
                      config.entrance_image_dedup_window_seconds == 25 &&
                      config.entrance_image_dedup_phash_threshold == 7 &&
                      config.entrance_delete_artifacts_on_success &&
                      config.entrance_failure_retention_hours == 12 &&
                      config.entrance_plate_match_window_minutes == 45 &&
                      config.entrance_plate_match_min_confidence == 0.9 &&
                      config.entrance_ev_analysis_enabled &&
                      config.entrance_ev_timeout_ms == 7000 &&
                      config.entrance_ev_worker_script ==
                          (std::filesystem::path(root) /
                           "tools/test_ev_worker.py").lexically_normal().string() &&
                      config.entrance_output_root ==
                          (std::filesystem::path(root) / "data/test_entrance")
                              .lexically_normal().string(),
                  "entrance settings must preserve the physical CH2 mapping");
    ok &= require(config.rtsp_channels.size() == 1 &&
                      config.rtsp_channels.front().channel_id == "ch02" &&
                      config.rtsp_channels.front().rtsp_url ==
                          "rtsp://camera/1/profile2/media.smp",
                  "CAMERA_RTSP_CH2 must create the physical CH2 RTSP input");

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
    clearValue("ENTRANCE_ENABLED");
    clearValue("ENTRANCE_OUTPUT_ROOT");
    clearValue("ENTRANCE_SOURCE_CHANNEL_ID");
    clearValue("ENTRANCE_CHANNEL_ID");
    clearValue("ENTRANCE_OBJECT_TTL_SECONDS");
    clearValue("ENTRANCE_EV_ANALYSIS_ENABLED");
    clearValue("ENTRANCE_EV_WORKER_SCRIPT");
    clearValue("ENTRANCE_EV_TIMEOUT_MS");
    clearValue("CAMERA_RTSP_CH2");
    clearValue("PI_SERVER_ROOT");
    return ok ? 0 : 1;
}

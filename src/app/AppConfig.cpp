#include "app/AppConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

std::string getEnvOrDefault(const char* key, const std::string& default_value) {
    const char* value = std::getenv(key);

    if (value == nullptr || std::string(value).empty()) {
        return default_value;
    }

    return std::string(value);
}

std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// run_server.sh 로 실행하지 않을 때(systemd 등)도 서버가 설정 파일을 직접
// 읽을 수 있게 한다. 공개 설정은 .env.public, 계정/키는 .env.private 이다.
// 환경변수가 있으면 파일보다 우선하며, 단순 KEY='VALUE' 형식만 허용한다.
std::string getEnvOrLocalSetting(const char* key,
                                 const std::string& default_value,
                                 const std::string& path) {
    const std::string environment = getEnvOrDefault(key, "");
    if (!environment.empty()) return environment;

    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != key)
            continue;
        std::string value = trim(line.substr(separator + 1));
        if (value.size() >= 2 &&
            ((value.front() == '\'' && value.back() == '\'') ||
             (value.front() == '"' && value.back() == '"')))
            value = value.substr(1, value.size() - 2);
        return value.empty() ? default_value : value;
    }
    return default_value;
}

int getEnvIntOrDefault(const char* key, int default_value) {
    const char* value = std::getenv(key);

    if (value == nullptr || std::string(value).empty()) {
        return default_value;
    }

    try {
        return std::stoi(value);
    } catch (...) {
        return default_value;
    }
}

bool getEnvBoolOrDefault(const char* key, bool default_value) {
    const std::string value = trim(getEnvOrDefault(key, default_value ? "true" : "false"));
    if (value == "1" || value == "true" || value == "TRUE" || value == "on") return true;
    if (value == "0" || value == "false" || value == "FALSE" || value == "off") return false;
    return default_value;
}

bool getEnvOrLocalBoolOrDefault(const char* key,
                                bool default_value,
                                const char* path) {
    const std::string value = trim(getEnvOrLocalSetting(
        key, default_value ? "true" : "false", path));
    if (value == "1" || value == "true" || value == "TRUE" || value == "on") return true;
    if (value == "0" || value == "false" || value == "FALSE" || value == "off") return false;
    return default_value;
}

int getEnvOrLocalIntOrDefault(const char* key,
                              int default_value,
                              const char* path) {
    const std::string value = getEnvOrLocalSetting(key, "", path);
    if (value.empty()) return default_value;
    try {
        return std::stoi(value);
    } catch (...) {
        return default_value;
    }
}

double parseDoubleOrDefault(const std::string& value, double default_value) {
    if (value.empty()) return default_value;
    try {
        return std::stod(value);
    } catch (...) {
        return default_value;
    }
}

}

namespace app {

AppConfig AppConfig::loadFromEnv() {
    AppConfig config;

    config.camera_id = getEnvOrDefault("CAMERA_ID", "cam01");
    config.mqtt_host = getEnvOrDefault("MQTT_HOST", "localhost");
    config.mqtt_port = getEnvIntOrDefault("MQTT_PORT", 1883);

    config.mqtt_event_sub_topic = getEnvOrDefault("CAMERA_EVENT_SUB_TOPIC", "+/onvif-ej/#");
    config.qt_event_topic_prefix = getEnvOrDefault("QT_EVENT_TOPIC_PREFIX", "parking/camera");
    config.default_channel_id = getEnvOrDefault("DEFAULT_CHANNEL_ID", "ch01");
    config.hall_mqtt_input_enabled =
        getEnvBoolOrDefault("HALL_MQTT_INPUT_ENABLED", true);
    config.hall_mqtt_topic =
        getEnvOrDefault("HALL_MQTT_TOPIC", "parking/sensor/hall");
    config.parking_slot_config_path =
        getEnvOrDefault("PARKING_SLOT_CONFIG", "config/parking_slots.json");
    config.sensor_link_mode = getEnvOrDefault("SENSOR_LINK_MODE", "off");
    config.sensor_uart_device =
        getEnvOrDefault("SENSOR_UART_DEVICE", "/dev/ttyAMA0");
    config.sensor_uart_baud_rate =
        getEnvIntOrDefault("SENSOR_UART_BAUD", 115200);
    config.sensor_uart_read_timeout_ms =
        std::max(1, getEnvIntOrDefault("SENSOR_UART_READ_TIMEOUT_MS", 250));
    config.sensor_uart_reconnect_ms =
        std::max(1, getEnvIntOrDefault("SENSOR_UART_RECONNECT_MS", 1000));
    config.parking_occupancy_confirm_ms =
        std::max(0, getEnvIntOrDefault("PARKING_OCCUPANCY_CONFIRM_MS", 0));
    config.parking_occupancy_source =
        getEnvOrDefault("PARKING_OCCUPANCY_SOURCE", "HALL");
    std::transform(config.parking_occupancy_source.begin(),
                   config.parking_occupancy_source.end(),
                   config.parking_occupancy_source.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(std::toupper(value));
                   });
    if (config.parking_occupancy_source != "HALL" &&
        config.parking_occupancy_source != "CAMERA_IVA") {
        config.parking_occupancy_source = "HALL";
    }
    config.camera_iva_exit_confirm_ms = std::clamp(
        getEnvIntOrDefault("CAMERA_IVA_EXIT_CONFIRM_MS", 20000),
        1000, 60000);
    config.capture_sched_enabled =
        getEnvBoolOrDefault("CAPTURE_SCHED_ENABLED", false);
    config.capture_topic_prefix =
        getEnvOrDefault("CAPTURE_TOPIC_PREFIX", "parking/capture");
    config.capture_response_timeout_ms =
        std::max(1, getEnvIntOrDefault("CAPTURE_RESPONSE_TIMEOUT_MS", 3000));
    config.capture_retry_interval_ms =
        std::max(1, getEnvIntOrDefault("CAPTURE_RETRY_INTERVAL_MS", 2000));
    config.capture_max_retries =
        std::max(0, getEnvIntOrDefault("CAPTURE_MAX_RETRIES", 2));
    config.hall_capture_ocr_enabled =
        getEnvBoolOrDefault("HALL_CAPTURE_OCR_ENABLED", true);
    config.capture_offsets_sec =
        getEnvOrDefault("CAPTURE_OFFSETS_SEC", "30,60");
    config.capture_ocr_max_attempts =
        std::clamp(getEnvIntOrDefault("CAPTURE_OCR_MAX_ATTEMPTS", 2), 1, 2);
    config.plate_led_enabled =
        getEnvBoolOrDefault("PLATE_LED_ENABLED", false);
    config.plate_led_night_start_hour =
        std::clamp(getEnvIntOrDefault("PLATE_LED_NIGHT_START_HOUR", 19), 0, 23);
    config.plate_led_night_end_hour =
        std::clamp(getEnvIntOrDefault("PLATE_LED_NIGHT_END_HOUR", 6), 0, 23);
    config.plate_led_settle_ms =
        std::clamp(getEnvIntOrDefault("PLATE_LED_SETTLE_MS", 200), 0, 2000);
    config.camera_snapshot_api_enabled =
        getEnvBoolOrDefault("CAMERA_SNAPSHOT_API_ENABLED", false);
    config.camera_snapshot_api_rtsp_fallback =
        getEnvBoolOrDefault("CAMERA_SNAPSHOT_API_RTSP_FALLBACK", false);
    config.camera_open_api_base = getEnvOrLocalSetting(
        "CAMERA_OPEN_API_BASE", "", ".env.private");
    config.camera_image_base = getEnvOrLocalSetting(
        "CAMERA_IMAGE_BASE", "", ".env.private");
    config.camera_api_username = getEnvOrLocalSetting(
        "CAMERA_API_USERNAME", "", ".env.private");
    config.camera_api_password = getEnvOrLocalSetting(
        "CAMERA_API_PASSWORD", "", ".env.private");
    config.camera_image_server_port = std::clamp(
        getEnvIntOrDefault("CAMERA_IMAGE_SERVER_PORT", 8080), 1024, 65535);
    config.camera_snapshot_connect_timeout_ms = std::max(
        1, getEnvIntOrDefault("CAMERA_SNAPSHOT_CONNECT_TIMEOUT_MS", 3000));
    config.camera_snapshot_request_timeout_ms = std::max(
        1, getEnvIntOrDefault("CAMERA_SNAPSHOT_REQUEST_TIMEOUT_MS", 30000));
    config.camera_snapshot_jpeg_timeout_ms = std::max(
        1, getEnvIntOrDefault("CAMERA_SNAPSHOT_JPEG_TIMEOUT_MS", 10000));
    config.camera_snapshot_max_retries = std::clamp(
        getEnvIntOrDefault("CAMERA_SNAPSHOT_MAX_RETRIES", 2), 0, 5);
    config.camera_snapshot_retry_delay_ms = std::max(
        1, getEnvIntOrDefault("CAMERA_SNAPSHOT_RETRY_DELAY_MS", 250));

    config.fire_alarm_enabled = getEnvBoolOrDefault("FIRE_ALARM_ENABLED", false);
    config.fire_uart_device = getEnvOrDefault("FIRE_UART_DEVICE", "/dev/ttyAMA0");
    config.fire_uart_baud = getEnvIntOrDefault("FIRE_UART_BAUD", 115200);
    config.fire_uart_reopen_delay_ms =
        getEnvIntOrDefault("FIRE_UART_REOPEN_DELAY_MS", 2000);
    config.fire_topic_prefix =
        getEnvOrDefault("FIRE_TOPIC_PREFIX", "parking/fire");
    config.fire_command_topic_prefix = getEnvOrDefault(
        "FIRE_COMMAND_TOPIC_PREFIX", "parking/v1/commands/fire");
    // "FLAME01=ch01,FLAME02=ch02" 형식의 시연용 채널별 입력 매핑.
    config.fire_sensor_channel_map =
        getEnvOrDefault("FIRE_SENSOR_CHANNEL_MAP", "");

    // 홀센서 주차 점유 경로. 화재와 같은 STM32 UART 링크를 공유한다(fire_uart_* 재사용).
    config.parking_hall_enabled =
        getEnvBoolOrDefault("PARKING_HALL_ENABLED", false);
    config.parking_slots_config_path =
        getEnvOrDefault("PARKING_SLOTS_CONFIG", "config/parking_slots.json");
    config.parking_hall_work_queue_capacity = std::max(
        1, getEnvIntOrDefault("PARKING_HALL_WORK_QUEUE_CAPACITY", 100));

    config.snapshot_dir = getEnvOrDefault("SNAPSHOT_DIR", "data/snapshots");
    config.db_path = getEnvOrDefault("EVENT_DB_PATH", "data/db/parking.db");
    config.gemini_api_key =
        getEnvOrLocalSetting("GEMINI_API_KEY", "", ".env.private");
    config.gemini_model = getEnvOrLocalSetting(
        "GEMINI_MODEL", "gemini-3-flash-preview", ".env.public");
    config.gemini_fallback_model = getEnvOrLocalSetting(
        "GEMINI_FALLBACK_MODEL", "gemini-3.1-flash-lite-preview",
        ".env.public");
    config.plate_preprocess_mode = getEnvOrLocalSetting(
        "PLATE_PREPROCESS_MODE", "pipeline", ".env.public");

    config.preview_width = getEnvIntOrDefault("PREVIEW_WIDTH", 640);
    config.preview_height = getEnvIntOrDefault("PREVIEW_HEIGHT", 360);

    config.rtsp_retry_delay_ms = getEnvIntOrDefault("RTSP_RETRY_DELAY_MS", 1000);
    config.empty_frame_delay_ms = getEnvIntOrDefault("EMPTY_FRAME_DELAY_MS", 200);
    config.initial_frame_timeout_sec = getEnvIntOrDefault("INITIAL_FRAME_TIMEOUT_SEC", 30);
    config.snapshot_frame_wait_ms = getEnvIntOrDefault("SNAPSHOT_FRAME_WAIT_MS", 2500);
    config.max_consecutive_read_failures = getEnvIntOrDefault("MAX_CONSECUTIVE_READ_FAILURES", 30);
    config.bestshot_correlation_window_ms =
        getEnvIntOrDefault("BESTSHOT_CORRELATION_WINDOW_MS", 8000);
    config.iva_duplicate_suppression_ms =
        getEnvIntOrDefault("IVA_DUPLICATE_SUPPRESSION_MS", 1500);
    config.gemini_connect_timeout_sec =
        getEnvIntOrDefault("GEMINI_CONNECT_TIMEOUT_SEC", 5);
    config.gemini_request_timeout_sec =
        getEnvIntOrDefault("GEMINI_REQUEST_TIMEOUT_SEC", 30);
    config.telegram_enabled = getEnvOrLocalBoolOrDefault(
        "TELEGRAM_ENABLED", false, ".env.public");
    config.telegram_bot_token = getEnvOrLocalSetting(
        "TELEGRAM_BOT_TOKEN", "", ".env.private");
    config.telegram_channel = getEnvOrLocalSetting(
        "TELEGRAM_CHANNEL", "", ".env.private");
    config.telegram_connect_timeout_ms = std::max(
        1, getEnvOrLocalIntOrDefault("TELEGRAM_CONNECT_TIMEOUT_MS", 3000,
                                     ".env.public"));
    config.telegram_request_timeout_ms = std::max(
        1, getEnvOrLocalIntOrDefault("TELEGRAM_REQUEST_TIMEOUT_MS", 10000,
                                     ".env.public"));
    config.telegram_retry_count = std::max(
        0, getEnvOrLocalIntOrDefault("TELEGRAM_RETRY_COUNT", 2,
                                     ".env.public"));
    config.telegram_retry_delay_ms = std::max(
        1, getEnvOrLocalIntOrDefault("TELEGRAM_RETRY_DELAY_MS", 500,
                                     ".env.public"));
    config.telegram_queue_capacity = std::max(
        1, getEnvOrLocalIntOrDefault("TELEGRAM_QUEUE_CAPACITY", 256,
                                     ".env.public"));

    config.parking_timer_enabled =
        getEnvBoolOrDefault("PARKING_TIMER_ENABLED", true);
    // 새 단일 설정이 없을 때만 기존 PARKING_TIMEOUT_SECONDS를 시작 기본값으로
    // 받아 이전 배포 설정과 호환한다. 이후 REST 변경값은 SQLite가 우선한다.
    config.parking_overstay_threshold_seconds = std::clamp(
        getEnvIntOrDefault("PARKING_OVERSTAY_THRESHOLD_SECONDS",
            getEnvIntOrDefault("PARKING_TIMEOUT_SECONDS", 3600)),
        60, 86400);

    config.http_api_enabled = getEnvBoolOrDefault("HTTP_API_ENABLED", true);
    config.http_listen_address = getEnvOrDefault("HTTP_LISTEN_ADDRESS", "0.0.0.0");
    config.http_port = getEnvIntOrDefault("HTTP_PORT", 8080);
    config.http_tls_certificate_path = getEnvOrDefault("HTTP_TLS_CERT_PATH", "");
    config.http_tls_private_key_path = getEnvOrDefault("HTTP_TLS_KEY_PATH", "");
    config.http_data_root = getEnvOrDefault("HTTP_DATA_ROOT", "data");
    config.http_max_image_mb = getEnvIntOrDefault("HTTP_MAX_IMAGE_MB", 10);

    for (int i = 1; i <= 4; ++i) {
        std::ostringstream env_key;
        env_key << "CAMERA_RTSP_CH" << i;

        std::string rtsp_url = getEnvOrDefault(env_key.str().c_str(), "");

        if (i == 1 && rtsp_url.empty()) {
            rtsp_url = getEnvOrDefault("CAMERA_RTSP", "");
        }

        if (rtsp_url.empty()) {
            continue;
        }

        std::ostringstream channel_id;
        channel_id << "ch" << std::setw(2) << std::setfill('0') << i;

        config.rtsp_channels.push_back({
            channel_id.str(),
            rtsp_url
        });
    }

    // EV01~EV04는 카메라 웹 설정의 IVA Area 이름과 동일하게 두는 것이 기본이다.
    // ROI 네 값이 전혀 없으면 좌표는 "미설정"으로 유지한다. SQLite에 Qt가
    // 저장한 값이 있으면 ParkingRoiSettingsService가 그것을 우선 복원한다.
    for (int i = 1; i <= 4; ++i) {
        std::ostringstream slot;
        slot << "EV" << std::setw(2) << std::setfill('0') << i;
        // 현재 설치에서는 ch01 한 영상의 네 ROI가 EV01~EV04를 담당한다.
        // 향후 채널 확장 시 IVA_EVxx_CHANNEL_ID로 슬롯별 override한다.
        const std::string channel = "ch01";
        const std::string prefix = "IVA_" + slot.str() + "_";
        const std::string roi_x = getEnvOrLocalSetting(
            (prefix + "ROI_X").c_str(), "", ".env.public");
        const std::string roi_y = getEnvOrLocalSetting(
            (prefix + "ROI_Y").c_str(), "", ".env.public");
        const std::string roi_width = getEnvOrLocalSetting(
            (prefix + "ROI_WIDTH").c_str(), "", ".env.public");
        const std::string roi_height = getEnvOrLocalSetting(
            (prefix + "ROI_HEIGHT").c_str(), "", ".env.public");
        const bool roi_configured =
            !roi_x.empty() || !roi_y.empty() ||
            !roi_width.empty() || !roi_height.empty();

        config.iva_areas.push_back({
            slot.str(),
            getEnvOrLocalSetting((prefix + "AREA_NAME").c_str(), slot.str(),
                                 ".env.public"),
            getEnvOrLocalSetting((prefix + "CHANNEL_ID").c_str(), channel,
                                 ".env.public"),
            parseDoubleOrDefault(roi_x, 0.0),
            parseDoubleOrDefault(roi_y, 0.0),
            parseDoubleOrDefault(roi_width, 1.0),
            parseDoubleOrDefault(roi_height, 1.0),
            std::max(0, getEnvIntOrDefault(
                (prefix + "SNAPSHOT_API_CHANNEL").c_str(), 0)),
            roi_configured
        });
    }

    return config;
}

}

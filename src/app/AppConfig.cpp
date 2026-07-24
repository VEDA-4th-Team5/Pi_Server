#include "app/AppConfig.hpp"

#include <algorithm>
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

// Gemini 비밀파일은 shell을 거치지 않고도 서버가 직접 읽을 수 있게 한다.
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

double getEnvDoubleOrDefault(const char* key, double default_value) {
    const char* value = std::getenv(key);
    if (value == nullptr || std::string(value).empty()) return default_value;
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

    config.snapshot_dir = getEnvOrDefault("SNAPSHOT_DIR", "data/snapshots");
    config.db_path = getEnvOrDefault("EVENT_DB_PATH", "data/db/parking.db");
    config.gemini_api_key =
        getEnvOrLocalSetting("GEMINI_API_KEY", "", ".env.gemini.local");
    config.gemini_model = getEnvOrLocalSetting(
        "GEMINI_MODEL", "gemini-3-flash-preview", ".env.gemini.local");
    config.gemini_fallback_model = getEnvOrLocalSetting(
        "GEMINI_FALLBACK_MODEL", "gemini-3.1-flash-lite-preview",
        ".env.gemini.local");
    config.plate_preprocess_mode = getEnvOrLocalSetting(
        "PLATE_PREPROCESS_MODE", "pipeline", ".env.gemini.local");

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

    config.parking_timer_enabled =
        getEnvBoolOrDefault("PARKING_TIMER_ENABLED", true);
    config.parking_timeout_seconds =
        std::max(1, getEnvIntOrDefault("PARKING_TIMEOUT_SECONDS", 3600));

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
    // ROI 미설정 시 전체 프레임(0,0,1,1)을 사용해 이벤트 사진을 놓치지 않는다.
    for (int i = 1; i <= 4; ++i) {
        std::ostringstream slot;
        slot << "EV" << std::setw(2) << std::setfill('0') << i;
        // 현재 설치에서는 ch01 한 영상의 네 ROI가 EV01~EV04를 담당한다.
        // 향후 채널 확장 시 IVA_EVxx_CHANNEL_ID로 슬롯별 override한다.
        const std::string channel = "ch01";
        const std::string prefix = "IVA_" + slot.str() + "_";

        config.iva_areas.push_back({
            slot.str(),
            getEnvOrLocalSetting((prefix + "AREA_NAME").c_str(), slot.str(),
                                 ".env.iva.local"),
            getEnvOrLocalSetting((prefix + "CHANNEL_ID").c_str(), channel,
                                 ".env.iva.local"),
            getEnvDoubleOrDefault((prefix + "ROI_X").c_str(), 0.0),
            getEnvDoubleOrDefault((prefix + "ROI_Y").c_str(), 0.0),
            getEnvDoubleOrDefault((prefix + "ROI_WIDTH").c_str(), 1.0),
            getEnvDoubleOrDefault((prefix + "ROI_HEIGHT").c_str(), 1.0)
        });
    }

    return config;
}

}

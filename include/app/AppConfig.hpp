#pragma once

#include <string>
#include <vector>

namespace app {

// 채널 번호와 인증정보가 포함될 수 있는 RTSP URL 한 쌍이다.
struct RtspChannelConfig {
    std::string channel_id;
    std::string rtsp_url;
};

// IVA 영역 하나와 실제 주차면/RTSP 채널/프레임 ROI의 대응 관계다.
// ROI 값은 입력 해상도에 독립적인 0.0~1.0 정규화 좌표를 사용한다.
struct IvaAreaConfig {
    std::string slot_id;
    std::string area_name;
    std::string channel_id;
    double roi_x;
    double roi_y;
    double roi_width;
    double roi_height;
};

struct AppConfig {
    // 소스에 계정정보를 넣지 않도록 모든 실행 설정은 환경변수에서 읽는다.
    std::string camera_id;
    std::string mqtt_host;
    int mqtt_port;

    std::string mqtt_event_sub_topic;
    std::string qt_event_topic_prefix;
    std::string default_channel_id;

    // 화재 알림 (STM32 UART -> Pi -> Qt). 토픽/프레임 규격은 아직 미확정이므로
    // 임시로 정한 값이며 여기 한 곳에서만 바꾼다.
    bool fire_alarm_enabled;
    std::string fire_uart_device;
    int fire_uart_baud;
    int fire_uart_reopen_delay_ms;
    std::string fire_topic_prefix;
    std::string fire_sensor_slot_map;

    // 홀센서 주차 점유 경로 (STM32 UART -> Pi). 화재 경로와 같은 STM32 UART 링크를
    // 공유하므로 별도 device 설정을 두지 않고 fire_uart_* 를 재사용한다.
    // 슬롯/센서 매핑은 코드가 아니라 아래 JSON 설정 파일에서 읽는다.
    bool parking_hall_enabled;
    std::string parking_slots_config_path;

    // OCCUPIED 확정(T0) 유예시간. 0(기본)이면 첫 OCCUPIED가 즉시 T0가 되는
    // 기존 동작. 재정렬 중 짧은 OCCUPIED<->VACANT flapping을 거르려면 켠다
    // (예: 10000 = 10초). STM32 하드웨어 디바운스와는 별개의 Pi측 정책이다.
    int parking_occupancy_confirm_ms;

    // 입차 후 촬영 스케줄러 (T0+30s / T0+60s). 카메라 촬영 규약(EVDA-138)이
    // 아직 미확정이라 토픽/페이로드는 draft 이며 기본 비활성이다. 켜면 홀 세션
    // 시작 시 두 번의 촬영 요청을 예약해 아래 prefix 로 발행한다.
    bool capture_sched_enabled;
    std::string capture_topic_prefix;
    int capture_response_timeout_ms;
    int capture_retry_interval_ms;
    int capture_max_retries;

    std::string snapshot_dir;
    std::string db_path;
    std::string gemini_api_key;
    std::string gemini_model;
    std::string gemini_fallback_model;
    std::string plate_preprocess_mode;

    int preview_width;
    int preview_height;

    int rtsp_retry_delay_ms;
    int empty_frame_delay_ms;
    int initial_frame_timeout_sec;
    int snapshot_frame_wait_ms;
    int max_consecutive_read_failures;
    int bestshot_correlation_window_ms;
    int iva_duplicate_suppression_ms;
    int gemini_connect_timeout_sec;
    int gemini_request_timeout_sec;

    bool http_api_enabled;
    std::string http_listen_address;
    int http_port;
    std::string http_tls_certificate_path;
    std::string http_tls_private_key_path;
    std::string http_data_root;
    int http_max_image_mb;

    std::vector<RtspChannelConfig> rtsp_channels;
    std::vector<IvaAreaConfig> iva_areas;

    static AppConfig loadFromEnv();  // 미설정 값에는 안전한 기본값을 적용한다.
};

}

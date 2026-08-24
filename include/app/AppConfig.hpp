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
    // CV Snapshot OpenAPI의 0-based channel. RTSP channel_id와 별도다.
    int snapshot_api_channel{0};
    // true일 때만 이 좌표를 SQLite가 비어 있는 최초 부트스트랩 값으로 쓴다.
    // 테스트/명시적 aggregate 초기화는 기존 호환을 위해 true가 기본이며,
    // AppConfig 환경 로더는 ROI 환경변수가 전혀 없으면 false로 설정한다.
    bool roi_configured{true};
};

struct AppConfig {
    // 소스에 계정정보를 넣지 않도록 모든 실행 설정은 환경변수에서 읽는다.
    std::string camera_id;
    std::string mqtt_host;
    int mqtt_port;

    std::string mqtt_event_sub_topic;
    std::string qt_event_topic_prefix;
    std::string default_channel_id;
    bool hall_mqtt_input_enabled;
    std::string hall_mqtt_topic;
    std::string parking_slot_config_path;
    std::string sensor_link_mode;
    std::string sensor_uart_device;
    int sensor_uart_baud_rate;
    int sensor_uart_read_timeout_ms;
    int sensor_uart_reconnect_ms;

    // 홀센서 OCCUPIED가 이 시간 이상 유지되어야 DB 세션을 생성한다.
    // 0이면 기존처럼 첫 OCCUPIED를 즉시 확정한다.
    int parking_occupancy_confirm_ms;

    // 주차 점유 상태를 확정하는 입력 정책.
    // HALL, CAMERA_IVA 또는 두 입력 중 하나로 입차를 확정하는 HYBRID_OR를
    // 사용한다. HYBRID_OR에서도 슬롯당 활성 세션은 하나만 유지한다.
    std::string parking_occupancy_source;
    // CAMERA_IVA 모드에서 EXIT를 즉시 확정하지 않고 후속 INTRUSION을 기다리는 시간.
    int camera_iva_exit_confirm_ms;

    // 확정된 입차 T0를 기준으로 30초/60초 MQTT 촬영 요청을 예약한다.
    bool capture_sched_enabled;
    std::string capture_topic_prefix;
    int capture_response_timeout_ms;
    int capture_retry_interval_ms;
    int capture_max_retries;
    // 실제 RTSP ROI 30/60초 촬영을 Gemini 재시도 정책에 연결한다.
    bool hall_capture_ocr_enabled;
    std::string capture_offsets_sec;
    int capture_ocr_max_attempts;

    // 야간 번호판 조명 LED (Pi -> STM32). 예약된 촬영 직전에만 켜고 직후에 끈다.
    // 야간 구간은 일출/일몰 계산 없이 관리자가 계절마다 조정하는 시각 구간이다.
    bool plate_led_enabled;
    int plate_led_night_start_hour;
    int plate_led_night_end_hour;
    // LED 점등부터 실제 촬영까지 두는 정착 지연이다.
    int plate_led_settle_ms;

    // CV5 cv_snapshot_api에서 세션 증거와 30/60초 original/enhanced JPEG를
    // 생성·다운로드한다. fallback=false이면 Pi RTSP 수신을 시작하지 않는다.
    bool camera_snapshot_api_enabled;
    bool camera_snapshot_api_rtsp_fallback;
    std::string camera_open_api_base;
    std::string camera_image_base;
    std::string camera_api_username;
    std::string camera_api_password;
    int camera_image_server_port;
    int camera_snapshot_connect_timeout_ms;
    int camera_snapshot_request_timeout_ms;
    int camera_snapshot_jpeg_timeout_ms;
    int camera_snapshot_max_retries;
    int camera_snapshot_retry_delay_ms;

    // Hanwha RTSP metadata의 Vehicle/Plate BestShot 다운로드 경로.
    // IVA + Snapshot API 운영에서는 중복 세션/촬영을 막기 위해 기본 비활성화한다.
    bool bestshot_enabled{false};

    // 입구 BestShot은 PNM-C16083RVQ의 실제 CH2 RTSP Metadata를 사용한다.
    // 카메라에서도 CH2의 입구 BestShot 규칙만 활성화한다.
    bool entrance_enabled{false};
    std::string entrance_camera_id{"cam01"};
    std::string entrance_source_channel_id{"ch02"};
    std::string entrance_channel_id{"ch02"};
    std::string entrance_output_root{"data/entrance"};
    int entrance_object_ttl_seconds{60};
    int entrance_pending_capacity{64};
    int entrance_image_dedup_window_seconds{20};
    int entrance_image_dedup_phash_threshold{10};
    bool entrance_delete_artifacts_on_success{true};
    int entrance_failure_retention_hours{24};
    int entrance_plate_match_window_minutes{30};
    double entrance_plate_match_min_confidence{0.85};
    bool entrance_ev_analysis_enabled{false};
    std::string entrance_ev_python{"/usr/bin/python3"};
    std::string entrance_ev_worker_script{
        "tools/cv/low_quality_presence_v1/runtime/pi_worker.py"};
    std::string entrance_ev_model_bundle{
        "tools/cv/low_quality_presence_v1/model_bundle"};
    std::string entrance_ev_template_cache{
        "tools/cv/low_quality_presence_v1/model_bundle/runtime_template_cache.json"};
    std::string entrance_ev_thresholds{
        "tools/cv/low_quality_presence_v1/model_bundle/default_thresholds.json"};
    int entrance_ev_timeout_ms{5000};
    int entrance_ev_queue_capacity{16};
    int entrance_ev_opencv_threads{1};

    // 화재 알림 (STM32 UART -> Pi -> Qt). 토픽/프레임 규격은 아직 미확정이므로
    // 임시로 정한 값이며 여기 한 곳에서만 바꾼다.
    bool fire_alarm_enabled;
    std::string fire_uart_device;
    int fire_uart_baud;
    int fire_uart_reopen_delay_ms;
    std::string fire_topic_prefix;
    std::string fire_command_topic_prefix;
    std::string fire_sensor_channel_map;

    // 홀센서 주차 점유 경로 (STM32 UART -> Pi). 화재 경로와 같은 STM32 UART 링크를
    // 공유하므로 별도 device 설정을 두지 않고 fire_uart_* 를 재사용한다.
    // 슬롯/센서 매핑은 코드가 아니라 아래 JSON 설정 파일에서 읽는다.
    bool parking_hall_enabled;
    std::string parking_slots_config_path;
    // DB/파일 작업이 센서 입력보다 느릴 때 메모리가 무제한 증가하지 않게 한다.
    int parking_hall_work_queue_capacity{100};

    // /dev/parking_alert 커널 문자 디바이스 연동. 드라이버가 없더라도
    // 주차/MQTT 처리는 계속하고 경고 상태 투영만 비활성화한다.
    bool parking_alert_driver_enabled{false};
    std::string parking_alert_device_path{"/dev/parking_alert"};
    std::string parking_alert_slot_map{"EV01:0,EV02:1,EV03:2,EV04:3"};

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
    bool telegram_enabled;
    std::string telegram_bot_token;
    std::string telegram_channel;
    int telegram_connect_timeout_ms;
    int telegram_request_timeout_ms;
    int telegram_retry_count;
    int telegram_retry_delay_ms;
    int telegram_queue_capacity;

    bool parking_timer_enabled;
    // 위반 판정과 OVERSTAY_EVIDENCE가 함께 사용하는 단일 부트스트랩 값.
    // 서버 시작 후에는 SQLite SYSTEM_SETTINGS 값이 우선한다.
    int parking_overstay_threshold_seconds;

    bool http_api_enabled;
    std::string http_listen_address;
    int http_port;
    std::string http_tls_certificate_path;
    std::string http_tls_private_key_path;
    std::string http_data_root;
    int http_max_image_mb;
    bool http_require_tls;

    int auth_session_ttl_seconds;
    int auth_login_window_seconds;
    int auth_login_max_failures;
    int auth_login_cooldown_seconds;

    std::vector<RtspChannelConfig> rtsp_channels;
    std::vector<IvaAreaConfig> iva_areas;

    static AppConfig loadFromEnv();  // 미설정 값에는 안전한 기본값을 적용한다.
};

}

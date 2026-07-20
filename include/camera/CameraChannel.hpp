#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace camera {

// RTSP 채널 하나가 공유하는 상태이다.
// 수신 스레드는 프레임을 쓰고, MQTT/스냅샷 스레드는 최신 프레임을 읽는다.
struct CameraChannel {
    std::string camera_id;
    std::string channel_id;
    std::string rtsp_url;

    cv::VideoCapture cap;  // FFmpeg 백엔드를 사용하는 RTSP 입력 객체

    // cv::Mat은 내부 버퍼를 공유할 수 있으므로 접근할 때 frame_mutex가 필요하다.
    cv::Mat latest_full_frame;
    cv::Mat latest_preview_frame;

    std::mutex frame_mutex;
    std::thread worker;

    // 단순 상태/통계 값은 mutex 대신 atomic으로 다른 스레드에서 안전하게 읽는다.
    std::atomic<bool> opened{false};
    std::atomic<unsigned long long> frame_count{0};
    std::atomic<int> read_failures{0};

    std::atomic<int> full_width{0};
    std::atomic<int> full_height{0};
    std::atomic<int> preview_width{0};
    std::atomic<int> preview_height{0};
};

}

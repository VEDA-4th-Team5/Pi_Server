#pragma once

#include "camera/CameraChannel.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace camera {

class RtspStreamReceiver {
public:
    RtspStreamReceiver(
        std::vector<std::shared_ptr<CameraChannel>>& channels,
        int preview_width,
        int preview_height,
        int rtsp_retry_delay_ms,
        int empty_frame_delay_ms,
        int max_consecutive_read_failures,
        std::atomic<bool>& running
    );

    void start();
    void stop();
    bool waitForInitialFrames(int timeout_sec);

private:
    void captureLoop(std::shared_ptr<CameraChannel> channel);

private:
    std::vector<std::shared_ptr<CameraChannel>>& channels_;
    int preview_width_;
    int preview_height_;
    int rtsp_retry_delay_ms_;
    int empty_frame_delay_ms_;
    int max_consecutive_read_failures_;
    std::atomic<bool>& running_;
};

}

#include "camera/RtspStreamReceiver.hpp"

#include "util/Logger.hpp"
#include "util/UrlMasker.hpp"

#include <opencv2/imgproc.hpp>

#include <chrono>
#include <sstream>
#include <thread>

namespace camera {

RtspStreamReceiver::RtspStreamReceiver(
    std::vector<std::shared_ptr<CameraChannel>>& channels,
    int preview_width,
    int preview_height,
    int rtsp_retry_delay_ms,
    int empty_frame_delay_ms,
    int max_consecutive_read_failures,
    std::atomic<bool>& running
)
    : channels_(channels),
      preview_width_(preview_width),
      preview_height_(preview_height),
      rtsp_retry_delay_ms_(rtsp_retry_delay_ms),
      empty_frame_delay_ms_(empty_frame_delay_ms),
      max_consecutive_read_failures_(max_consecutive_read_failures),
      running_(running) {
}

void RtspStreamReceiver::start() {
    // 한 채널의 read() 지연이 다른 채널을 막지 않도록 채널마다 스레드를 둔다.
    for (auto& channel : channels_) {
        channel->worker = std::thread(&RtspStreamReceiver::captureLoop, this, channel);
    }
}

void RtspStreamReceiver::stop() {
    for (auto& channel : channels_) {
        if (channel->worker.joinable()) {
            channel->worker.join();
        }
    }
}

bool RtspStreamReceiver::waitForInitialFrames(int timeout_sec) {
    {
        std::ostringstream oss;
        oss << "waiting for initial FULL RTSP frame before MQTT subscribe... timeout="
            << timeout_sec << "s";
        util::logInfo(oss.str());
    }

    auto start = std::chrono::steady_clock::now();

    while (running_.load()) {
        bool all_ready = true;

        for (const auto& channel : channels_) {
            // 수신 스레드가 동시에 Mat을 교체할 수 있으므로 잠근 뒤 검사한다.
            std::lock_guard<std::mutex> lock(channel->frame_mutex);

            if (channel->latest_full_frame.empty()) {
                all_ready = false;
                break;
            }
        }

        if (all_ready) {
            util::logInfo("initial FULL RTSP frame ready for all configured channels");

            for (const auto& channel : channels_) {
                std::ostringstream oss;
                oss << "channel status: "
                    << channel->channel_id
                    << " opened=" << (channel->opened.load() ? "true" : "false")
                    << " frame_count=" << channel->frame_count.load()
                    << " read_failures=" << channel->read_failures.load()
                    << " full=" << channel->full_width.load() << "x" << channel->full_height.load()
                    << " preview=" << channel->preview_width.load() << "x" << channel->preview_height.load();

                util::logInfo(oss.str());
            }

            return true;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();

        if (elapsed >= timeout_sec) {
            util::logError("initial FULL RTSP frame timeout");

            for (const auto& channel : channels_) {
                std::ostringstream oss;
                oss << "channel status: "
                    << channel->channel_id
                    << " opened=" << (channel->opened.load() ? "true" : "false")
                    << " frame_count=" << channel->frame_count.load()
                    << " read_failures=" << channel->read_failures.load()
                    << " full=" << channel->full_width.load() << "x" << channel->full_height.load()
                    << " url=" << util::hideUrlForLog(channel->rtsp_url);

                util::logError(oss.str());
            }

            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return false;
}

void RtspStreamReceiver::captureLoop(std::shared_ptr<CameraChannel> channel) {
    // 일시적인 빈 프레임마다 재연결하지 않고 연속 실패 횟수를 누적한다.
    int consecutive_failures = 0;

    while (running_.load()) {
        if (!channel->cap.isOpened()) {
            std::ostringstream oss;
            oss << "Opening RTSP "
                << channel->channel_id
                << " url="
                << util::hideUrlForLog(channel->rtsp_url);

            util::logInfo(oss.str());

            channel->cap.open(channel->rtsp_url, cv::CAP_FFMPEG);

            if (!channel->cap.isOpened()) {
                channel->opened.store(false);
                util::logWarn("RTSP open failed: " + channel->channel_id);
                std::this_thread::sleep_for(std::chrono::milliseconds(rtsp_retry_delay_ms_));
                continue;
            }

            channel->opened.store(true);
            consecutive_failures = 0;
            util::logInfo("RTSP opened: " + channel->channel_id);
        }

        cv::Mat frame;
        bool ok = channel->cap.read(frame);

        if (!ok || frame.empty()) {
            consecutive_failures++;
            channel->read_failures.store(consecutive_failures);

            if (consecutive_failures >= max_consecutive_read_failures_) {
                std::ostringstream oss;
                oss << "frame read failed repeatedly: "
                    << channel->channel_id
                    << " failures=" << consecutive_failures
                    << " -> reconnect RTSP";

                util::logWarn(oss.str());

                channel->opened.store(false);
                channel->cap.release();
                consecutive_failures = 0;

                std::this_thread::sleep_for(std::chrono::milliseconds(rtsp_retry_delay_ms_));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(empty_frame_delay_ms_));
            }

            continue;
        }

        consecutive_failures = 0;
        channel->read_failures.store(0);

        // 원본은 증거 이미지 저장용, preview는 향후 관제/처리용 경량 프레임이다.
        cv::Mat preview;
        cv::resize(frame, preview, cv::Size(preview_width_, preview_height_));

        unsigned long long count = channel->frame_count.fetch_add(1) + 1;

        {
            std::lock_guard<std::mutex> lock(channel->frame_mutex);

            // clone()으로 VideoCapture의 재사용 버퍼와 수명/데이터 경쟁을 분리한다.
            channel->latest_full_frame = frame.clone();
            channel->latest_preview_frame = preview.clone();

            channel->full_width.store(frame.cols);
            channel->full_height.store(frame.rows);
            channel->preview_width.store(preview.cols);
            channel->preview_height.store(preview.rows);
        }

        if (count == 1) {
            std::ostringstream oss;
            oss << "first FULL RTSP frame ready: "
                << channel->channel_id
                << " full=" << frame.cols << "x" << frame.rows
                << " preview=" << preview.cols << "x" << preview.rows
                << " frame_count=" << count;

            util::logInfo(oss.str());
        }
    }

    if (channel->cap.isOpened()) {
        channel->cap.release();
    }

    channel->opened.store(false);
    util::logInfo("camera thread stopped: " + channel->channel_id);
}

}

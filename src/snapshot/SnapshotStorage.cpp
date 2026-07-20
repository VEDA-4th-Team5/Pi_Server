#include "snapshot/SnapshotStorage.hpp"

#include "util/Logger.hpp"
#include "util/TimeUtil.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::string channelDirectoryName(const std::string& channel_id) {
    if (channel_id.size() > 2 && channel_id.rfind("ch", 0) == 0) {
        std::size_t number = channel_id.find_first_not_of('0', 2);
        if (number == std::string::npos) return "ch0";
        return "ch" + channel_id.substr(number);
    }
    return channel_id.empty() ? "unknown" : channel_id;
}

}

namespace snapshot {

SnapshotStorage::SnapshotStorage(
    const std::string& snapshot_dir,
    int snapshot_frame_wait_ms,
    std::atomic<bool>& running
)
    : snapshot_dir_(snapshot_dir),
      snapshot_frame_wait_ms_(snapshot_frame_wait_ms),
      running_(running) {
}

std::string SnapshotStorage::saveFullSizeSnapshot(
    const std::shared_ptr<camera::CameraChannel>& channel
) {
    if (!channel) {
        util::logWarn("No camera channel available for full-size snapshot");
        return "";
    }

    cv::Mat full_copy = waitForFullFrame(channel);
    if (full_copy.empty()) return "";

    fs::path channel_dir =
        fs::path(snapshot_dir_) / channelDirectoryName(channel->channel_id);
    fs::create_directories(channel_dir);

    std::string filename = channel->camera_id + "_" + channel->channel_id +
        "_FULL_" + std::to_string(full_copy.cols) + "x" +
        std::to_string(full_copy.rows) + "_" + util::nowStringForFilename() + ".jpg";
    std::string path = (channel_dir / filename).string();
    if (!cv::imwrite(path, full_copy)) {
        util::logError("Full-size snapshot save failed: " + path);
        return "";
    }
    return path;
}

std::string SnapshotStorage::saveIvaAreaSnapshot(
    const std::shared_ptr<camera::CameraChannel>& channel,
    const std::string& slot_id,
    const NormalizedRoi& roi
) {
    if (!channel || slot_id.empty()) return "";
    cv::Mat frame = waitForFullFrame(channel);
    if (frame.empty()) return "";

    // 잘못된 설정이 OpenCV assertion을 일으키지 않도록 0~1 범위로 제한한다.
    double x = std::max(0.0, std::min(1.0, roi.x));
    double y = std::max(0.0, std::min(1.0, roi.y));
    double right = std::max(x, std::min(1.0, x + roi.width));
    double bottom = std::max(y, std::min(1.0, y + roi.height));
    int px = static_cast<int>(x * frame.cols);
    int py = static_cast<int>(y * frame.rows);
    int pw = std::min(frame.cols - px,
                      std::max(1, static_cast<int>((right - x) * frame.cols)));
    int ph = std::min(frame.rows - py,
                      std::max(1, static_cast<int>((bottom - y) * frame.rows)));
    if (px < 0 || py < 0 || px >= frame.cols || py >= frame.rows || pw <= 0 || ph <= 0) {
        util::logError("Invalid IVA ROI for slot=" + slot_id);
        return "";
    }

    cv::Mat cropped = frame(cv::Rect(px, py, pw, ph)).clone();
    // Snapshot은 물리 카메라 채널을 최상위 기준으로 정리한다.
    // 현재 IVA 담당 ch01은 snapshots/ch1/EV01~EV04 아래에 ROI 장면을 저장하며,
    // 향후 ch02~ch04도 같은 규칙을 그대로 사용할 수 있다.
    fs::path directory = fs::path(snapshot_dir_) /
        channelDirectoryName(channel->channel_id) / slot_id / "scene";
    fs::create_directories(directory);
    std::string filename = channel->camera_id + "_" + channel->channel_id + "_" +
        slot_id + "_IVA_ROI_" + std::to_string(cropped.cols) + "x" +
        std::to_string(cropped.rows) + "_" + util::nowStringForFilename() + ".jpg";
    std::string path = (directory / filename).string();
    if (!cv::imwrite(path, cropped)) {
        util::logError("IVA area snapshot save failed: " + path);
        return "";
    }
    return path;
}

cv::Mat SnapshotStorage::waitForFullFrame(
    const std::shared_ptr<camera::CameraChannel>& channel
) {
    if (!channel) return {};
    auto start = std::chrono::steady_clock::now();
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lock(channel->frame_mutex);
            if (!channel->latest_full_frame.empty())
                return channel->latest_full_frame.clone();
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        if (elapsed_ms >= snapshot_frame_wait_ms_) {
            std::ostringstream oss;
            oss << "latest_full_frame is empty after wait: "
                << channel->channel_id
                << " frame_count=" << channel->frame_count.load()
                << " read_failures=" << channel->read_failures.load();

            util::logWarn(oss.str());
            return {};
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return {};
}

}

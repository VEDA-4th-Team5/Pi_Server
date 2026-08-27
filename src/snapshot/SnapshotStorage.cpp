#include "snapshot/SnapshotStorage.hpp"

#include "util/Logger.hpp"
#include "util/TimeUtil.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr int kMinimumCropPixels = 8;

std::string channelDirectoryName(const std::string& channel_id) {
    if (channel_id.size() > 2 && channel_id.rfind("ch", 0) == 0) {
        std::size_t number = channel_id.find_first_not_of('0', 2);
        if (number == std::string::npos) return "ch0";
        return "ch" + channel_id.substr(number);
    }
    return channel_id.empty() ? "unknown" : channel_id;
}

bool writeBytes(const fs::path& path,
                const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

std::string stageDirectoryName(const std::string& value) {
    if (value == "PARKING_ENTRY_IMAGE") return "parking_entry";
    if (value == "OCCUPANCY_START_EVIDENCE") return "occupancy_start";
    if (value == "OVERSTAY_EVIDENCE") return "overstay";
    // 점유 감지는 Hall 또는 IVA가 시작할 수 있으므로 저장 경로에는
    // 특정 센서 이름을 노출하지 않는다. 이름순으로도 세션 단계가
    // occupancy_start -> occupied_30s -> occupied_60s -> overstay가 된다.
    if (value == "HALL_30S") return "occupied_30s";
    if (value == "HALL_60S") return "occupied_60s";
    return "capture";
}

bool validNormalizedRoi(const snapshot::NormalizedRoi& roi) {
    return std::isfinite(roi.x) && std::isfinite(roi.y) &&
           std::isfinite(roi.width) && std::isfinite(roi.height) &&
           roi.x >= 0.0 && roi.y >= 0.0 && roi.width > 0.0 &&
           roi.height > 0.0 && roi.x < 1.0 && roi.y < 1.0 &&
           roi.x + roi.width <= 1.0 && roi.y + roi.height <= 1.0;
}

std::optional<std::vector<unsigned char>> cropJpeg(
    const std::vector<unsigned char>& jpeg,
    const snapshot::NormalizedRoi& roi) {
    if (jpeg.empty() || !validNormalizedRoi(roi)) return std::nullopt;
    const cv::Mat image = cv::imdecode(jpeg, cv::IMREAD_COLOR);
    if (image.empty()) return std::nullopt;

    const int left = std::clamp(
        static_cast<int>(std::lround(roi.x * image.cols)), 0, image.cols - 1);
    const int top = std::clamp(
        static_cast<int>(std::lround(roi.y * image.rows)), 0, image.rows - 1);
    const int right = std::clamp(
        static_cast<int>(std::lround((roi.x + roi.width) * image.cols)),
        left + 1, image.cols);
    const int bottom = std::clamp(
        static_cast<int>(std::lround((roi.y + roi.height) * image.rows)),
        top + 1, image.rows);
    if (right - left < kMinimumCropPixels ||
        bottom - top < kMinimumCropPixels) {
        return std::nullopt;
    }
    const cv::Mat cropped = image(cv::Rect(left, top, right - left,
                                           bottom - top)).clone();
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".jpg", cropped, encoded,
                      {cv::IMWRITE_JPEG_QUALITY, 95})) {
        return std::nullopt;
    }
    return encoded;
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
    return saveAreaSnapshot(channel, slot_id, roi, {}, -1, "iva");
}

std::string SnapshotStorage::saveEvidenceSnapshot(
    const std::shared_ptr<camera::CameraChannel>& channel,
    const std::int64_t session_id,
    const std::string& slot_id,
    const std::string& evidence_reason,
    const NormalizedRoi& roi
) {
    if (session_id < 0 || evidence_reason.empty()) return "";
    const std::string prefix = "session_" + std::to_string(session_id) +
        "_slot_" + slot_id + "_" + evidence_reason;
    return saveAreaSnapshot(channel, slot_id, roi, prefix, session_id,
                            stageDirectoryName(evidence_reason) + "/original");
}

std::string SnapshotStorage::saveHallCaptureSnapshot(
    const std::shared_ptr<camera::CameraChannel>& channel,
    const std::int64_t session_id,
    const std::string& slot_id,
    const std::string& capture_stage,
    const NormalizedRoi& roi
) {
    if (session_id < 0 || capture_stage.empty()) return "";
    const std::string prefix = "session_" + std::to_string(session_id) +
        "_slot_" + slot_id + "_" + capture_stage;
    return saveAreaSnapshot(channel, slot_id, roi, prefix, session_id,
                            stageDirectoryName(capture_stage) + "/original");
}

StoredImagePair SnapshotStorage::saveCameraApiHallCapture(
    const std::string& channel_id,
    const std::int64_t session_id,
    const std::string& slot_id,
    const std::string& capture_stage,
    const NormalizedRoi& roi,
    const std::vector<unsigned char>& original_jpeg,
    const std::vector<unsigned char>& enhanced_jpeg
) {
    if (channel_id.empty() || session_id < 0 || slot_id.empty() ||
        capture_stage.empty() || original_jpeg.empty() ||
        enhanced_jpeg.empty()) {
        util::logError("camera API snapshot contains an empty field or JPEG");
        return {};
    }
    const auto cropped_original = cropJpeg(original_jpeg, roi);
    const auto cropped_enhanced = cropJpeg(enhanced_jpeg, roi);
    if (!cropped_original || !cropped_enhanced) {
        util::logError("camera API ROI crop failed: session=" +
                       std::to_string(session_id) + " slot=" + slot_id);
        return {};
    }

    const fs::path stage_dir = fs::path(snapshot_dir_) /
        channelDirectoryName(channel_id) / slot_id /
        stageDirectoryName(capture_stage);
    const fs::path original_dir = stage_dir / "original";
    const fs::path enhanced_dir = stage_dir / "enhanced";
    std::error_code error;
    fs::create_directories(original_dir, error);
    if (error) {
        util::logError("camera API original directory create failed: " +
                       error.message());
        return {};
    }
    fs::create_directories(enhanced_dir, error);
    if (error) {
        util::logError("camera API enhanced directory create failed: " +
                       error.message());
        return {};
    }

    const std::string unique = util::nowStringForFilename() + "_" +
        std::to_string(next_file_sequence_.fetch_add(1));
    const std::string prefix = "session_" + std::to_string(session_id) +
        "_slot_" + slot_id + "_" + capture_stage + "_CAMERA_API_" + unique;
    const fs::path original_path = original_dir / (prefix + "_original.jpg");
    const fs::path enhanced_path = enhanced_dir / (prefix + "_enhanced.jpg");
    const fs::path original_temp = original_path.string() + ".tmp";
    const fs::path enhanced_temp = enhanced_path.string() + ".tmp";

    if (!writeBytes(original_temp, *cropped_original) ||
        !writeBytes(enhanced_temp, *cropped_enhanced)) {
        fs::remove(original_temp, error);
        fs::remove(enhanced_temp, error);
        util::logError("camera API JPEG file write failed: session=" +
                       std::to_string(session_id) + " slot=" + slot_id);
        return {};
    }
    fs::rename(original_temp, original_path, error);
    if (error) {
        const std::string message = error.message();
        std::error_code ignored;
        fs::remove(original_temp, ignored);
        fs::remove(enhanced_temp, ignored);
        util::logError("camera API original JPEG commit failed: " +
                       message);
        return {};
    }
    fs::rename(enhanced_temp, enhanced_path, error);
    if (error) {
        const std::string message = error.message();
        std::error_code ignored;
        fs::remove(original_path, ignored);
        fs::remove(enhanced_temp, ignored);
        util::logError("camera API enhanced JPEG commit failed: " +
                       message);
        return {};
    }
    return {original_path.string(), enhanced_path.string()};
}

StoredImagePair SnapshotStorage::saveCameraApiIvaSnapshot(
    const std::string& channel_id,
    const std::string& slot_id,
    const NormalizedRoi& roi,
    const std::vector<unsigned char>& original_jpeg,
    const std::vector<unsigned char>& enhanced_jpeg
) {
    if (channel_id.empty() || slot_id.empty() || original_jpeg.empty() ||
        enhanced_jpeg.empty()) {
        util::logError("camera API IVA snapshot contains an empty field or JPEG");
        return {};
    }
    const auto cropped_original = cropJpeg(original_jpeg, roi);
    const auto cropped_enhanced = cropJpeg(enhanced_jpeg, roi);
    if (!cropped_original || !cropped_enhanced) {
        util::logError("camera API IVA ROI crop failed: slot=" + slot_id);
        return {};
    }

    const fs::path iva_dir = fs::path(snapshot_dir_) /
        channelDirectoryName(channel_id) / slot_id / "events" / "iva";
    const fs::path original_dir = iva_dir / "original";
    const fs::path enhanced_dir = iva_dir / "enhanced";
    std::error_code error;
    fs::create_directories(original_dir, error);
    if (error) {
        util::logError("camera API IVA original directory create failed: " +
                       error.message());
        return {};
    }
    fs::create_directories(enhanced_dir, error);
    if (error) {
        util::logError("camera API IVA enhanced directory create failed: " +
                       error.message());
        return {};
    }

    const std::string unique = util::nowStringForFilename() + "_" +
        std::to_string(next_file_sequence_.fetch_add(1));
    const std::string prefix = "slot_" + slot_id +
        "_IVA_CAMERA_API_" + unique;
    const fs::path original_path = original_dir / (prefix + "_original.jpg");
    const fs::path enhanced_path = enhanced_dir / (prefix + "_enhanced.jpg");
    const fs::path original_temp = original_path.string() + ".tmp";
    const fs::path enhanced_temp = enhanced_path.string() + ".tmp";

    if (!writeBytes(original_temp, *cropped_original) ||
        !writeBytes(enhanced_temp, *cropped_enhanced)) {
        fs::remove(original_temp, error);
        fs::remove(enhanced_temp, error);
        util::logError("camera API IVA JPEG file write failed: slot=" +
                       slot_id);
        return {};
    }
    fs::rename(original_temp, original_path, error);
    if (error) {
        const std::string message = error.message();
        std::error_code ignored;
        fs::remove(original_temp, ignored);
        fs::remove(enhanced_temp, ignored);
        util::logError("camera API IVA original JPEG commit failed: " +
                       message);
        return {};
    }
    fs::rename(enhanced_temp, enhanced_path, error);
    if (error) {
        const std::string message = error.message();
        std::error_code ignored;
        fs::remove(original_path, ignored);
        fs::remove(enhanced_temp, ignored);
        util::logError("camera API IVA enhanced JPEG commit failed: " +
                       message);
        return {};
    }
    return {original_path.string(), enhanced_path.string()};
}

std::string SnapshotStorage::saveAreaSnapshot(
    const std::shared_ptr<camera::CameraChannel>& channel,
    const std::string& slot_id,
    const NormalizedRoi& roi,
    const std::string& filename_prefix,
    const std::int64_t session_id,
    const std::string& stage_directory
) {
    if (!channel || slot_id.empty()) return "";
    if (roi.x < 0.0 || roi.y < 0.0 || roi.width <= 0.0 || roi.height <= 0.0 ||
        roi.x >= 1.0 || roi.y >= 1.0 || roi.x + roi.width > 1.0 ||
        roi.y + roi.height > 1.0) {
        util::logError("Invalid IVA ROI for slot=" + slot_id);
        return "";
    }
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
    if (pw < kMinimumCropPixels || ph < kMinimumCropPixels) {
        util::logError("IVA ROI is too small after pixel conversion: slot=" +
                       slot_id + " size=" + std::to_string(pw) + "x" +
                       std::to_string(ph));
        return "";
    }

    cv::Mat cropped = frame(cv::Rect(px, py, pw, ph)).clone();
    // 슬롯별 고정 촬영 단계 디렉터리를 사용한다. 세션 구분은 파일명의
    // session_<id>와 IMAGE_LOG.session_id로 유지하므로 조기 출차 시 해당 세션 파일만
    // 안전하게 정리할 수 있다. 세션 없는 진단 IVA 이미지는 events/iva에 격리한다.
    fs::path directory = fs::path(snapshot_dir_) /
        channelDirectoryName(channel->channel_id) / slot_id;
    if (session_id >= 0) {
        directory /= stage_directory.empty() ? "capture" : stage_directory;
    } else {
        directory /= "events";
        directory /= stage_directory.empty() ? "iva" : stage_directory;
    }
    fs::create_directories(directory);
    std::string filename;
    if (filename_prefix.empty()) {
        filename = channel->camera_id + "_" + channel->channel_id + "_" +
            slot_id + "_IVA_ROI_" + std::to_string(cropped.cols) + "x" +
            std::to_string(cropped.rows) + "_" + util::nowStringForFilename() +
            ".jpg";
    } else {
        filename = filename_prefix + "_" + util::nowStringForFilename() +
            "_" + std::to_string(next_file_sequence_.fetch_add(1)) + ".jpg";
    }
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

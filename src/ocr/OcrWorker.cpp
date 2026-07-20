#include "ocr/OcrWorker.hpp"

#include "ocr/PlateImageEnhancer.hpp"
#include "ocr/PlateNormalizer.hpp"
#include "util/Logger.hpp"

#include <curl/curl.h>
#include <chrono>

namespace ocr {

OcrWorker::OcrWorker(GeminiOcrClient client,
                     database::EventDatabase& database,
                     bool preprocess_enabled)
    : client_(std::move(client)), database_(database),
      preprocess_enabled_(preprocess_enabled) {
}

OcrWorker::~OcrWorker() { stop(); }

bool OcrWorker::enabled() const { return client_.configured(); }

void OcrWorker::start() {
    if (!enabled()) {
        util::logWarn("Gemini OCR disabled: GEMINI_API_KEY is not configured");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    stopping_ = false;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        util::logError("Gemini OCR curl global initialization failed");
        return;
    }
    started_ = true;
    worker_ = std::thread(&OcrWorker::run, this);
    util::logInfo("Gemini OCR worker started");
}

void OcrWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    curl_global_cleanup();
}

void OcrWorker::enqueue(int session_id, const std::string& slot_id,
                        const std::string& image_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || session_id < 0 || image_path.empty()) return;
    if (queue_.size() >= 32) {
        util::logWarn("Gemini OCR queue full; image skipped: " + image_path);
        return;
    }
    if (!accepted_images_.insert(image_path).second) {
        util::logInfo("duplicate OCR image suppressed: " + image_path);
        return;
    }
    queue_.push({session_id, slot_id, image_path, false, ""});
    condition_.notify_one();
}

void OcrWorker::enqueueScene(const std::string& slot_id,
                             const std::string& image_path,
                             const std::string& enhanced_image_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || slot_id.empty() || image_path.empty() ||
        enhanced_image_path.empty()) return;
    if (queue_.size() >= 32 || !accepted_images_.insert(image_path).second) return;
    // IVA ROI 후보를 별도로 자르지 않고 원본과 이미 생성된 개선본을 함께 보낸다.
    queue_.push({-1, slot_id, image_path, false, enhanced_image_path});
    condition_.notify_one();
}

void OcrWorker::run() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            task = std::move(queue_.front());
            queue_.pop();
        }

        PlatePreprocessResult processed;
        std::string ocr_input = task.image_path;
        if (!task.provided_enhanced_path.empty())
            processed.enhanced_path = task.provided_enhanced_path;
        if (!preprocess_enabled_ && task.detect_candidate) {
            util::logInfo("IVA plate preprocessing disabled; OCR skipped: path=" +
                          task.image_path);
            continue;
        }
        if (preprocess_enabled_ && task.provided_enhanced_path.empty()) {
            processed = preprocessPlateImage(task.image_path, task.detect_candidate);
            if (task.detect_candidate && !processed.candidate_detected) {
                util::logInfo("No plausible plate candidate in IVA scene: path=" +
                              task.image_path);
                continue;
            }
            if (!processed.enhanced_path.empty()) {
                database_.attachEnhancedPlateImage(task.image_path,
                                                    processed.enhanced_path);
                util::logLine("PLATE_PREPROCESS", "original=" + task.image_path +
                              " enhanced=" + processed.enhanced_path);
            }
        }

        OcrResult result;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            result = client_.recognizePlate(
                ocr_input, processed.enhanced_path);
            if (result.success) break;
            // 잘못된 키/요청처럼 재시도로 회복되지 않는 4xx는 즉시 중단한다.
            const bool permanent_client_error =
                result.error.find("Gemini HTTP status 4") != std::string::npos &&
                result.error.find("Gemini HTTP status 429") == std::string::npos;
            if (attempt == 3 || permanent_client_error) break;
            const int delay_seconds = attempt;
            util::logWarn("Gemini OCR retry " + std::to_string(attempt + 1) +
                          "/3 after " + std::to_string(delay_seconds) +
                          "s: " + result.error);
            std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
        }
        if (!result.success) {
            util::logError("Gemini OCR failed: path=" + task.image_path +
                           " error=" + result.error);
            continue;
        }
        std::string plate = normalizePlateNumber(result.plate_number);
        if (!result.readable || !isPlausibleKoreanPlate(plate)) {
            util::logWarn("Gemini OCR unreadable: path=" + task.image_path);
            database_.applyPlateOcr(task.session_id, task.slot_id,
                                    task.image_path, "", result.confidence);
            continue;
        }
        std::string classification = database_.applyPlateOcr(
            task.session_id, task.slot_id, task.image_path, plate,
            result.confidence);
        util::logLine("PLATE_OCR", "slot=" + task.slot_id +
                      " plate=" + plate + " class=" + classification +
                      " confidence=" + std::to_string(result.confidence));
    }
}

}

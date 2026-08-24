/** @file OcrWorker.cpp @brief 이미지 전처리·Gemini OCR·DB 반영 비동기 worker 구현. */
#include "ocr/OcrWorker.hpp"

#include "ocr/PlateImageEnhancer.hpp"
#include "ocr/PlateNormalizer.hpp"
#include "util/Logger.hpp"
#include "util/TimeUtil.hpp"

#include <curl/curl.h>
#include <chrono>
#include <filesystem>

namespace ocr {
namespace {

bool usableImageFile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           std::filesystem::file_size(path, error) > 0 && !error;
}

}  // namespace

OcrWorker::OcrWorker(GeminiOcrClient client,
                     database::EventDatabase& database,
                     bool preprocess_enabled,
                     ResultCallback result_callback,
                     const std::int64_t entrance_match_window_ms,
                     const double entrance_match_min_confidence)
    : client_(std::move(client)), database_(database),
      preprocess_enabled_(preprocess_enabled),
      entrance_match_window_ms_(entrance_match_window_ms),
      entrance_match_min_confidence_(entrance_match_min_confidence),
      result_callback_(std::move(result_callback)) {
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
                        const std::string& image_path,
                        const std::string& enhanced_image_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || session_id < 0 || image_path.empty()) return;
    if (canceled_session_ids_.contains(session_id)) return;
    if (queue_.size() >= 32) {
        util::logWarn("Gemini OCR queue full; image skipped: " + image_path);
        return;
    }
    if (!accepted_images_.insert(image_path).second) {
        util::logInfo("duplicate OCR image suppressed: " + image_path);
        return;
    }
    queue_.push({session_id, slot_id, image_path, false,
                 enhanced_image_path, false, 0, false, {}, {}});
    condition_.notify_one();
}

void OcrWorker::cancelSession(const int session_id) {
    if (session_id < 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    canceled_session_ids_.insert(session_id);
}

void OcrWorker::enqueueScene(const std::string& slot_id,
                             const std::string& image_path,
                             const std::string& enhanced_image_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || slot_id.empty() || image_path.empty() ||
        enhanced_image_path.empty()) return;
    if (queue_.size() >= 32 || !accepted_images_.insert(image_path).second) return;
    // IVA ROI 후보를 별도로 자르지 않고 원본과 이미 생성된 개선본을 함께 보낸다.
    queue_.push({-1, slot_id, image_path, false, enhanced_image_path, false, 0,
                 false, {}, {}});
    condition_.notify_one();
}

void OcrWorker::setHallCaptureCallback(HallCaptureCallback callback) {
    std::lock_guard lock(mutex_);
    hall_callback_ = std::move(callback);
}

void OcrWorker::enqueueHallCapture(const HallCaptureTask& request) {
    HallCaptureCallback rejected;
    {
        std::lock_guard lock(mutex_);
        const bool acceptable = started_ && request.session_id >= 0 &&
                                !request.image_path.empty() &&
                                !canceled_session_ids_.contains(
                                    static_cast<int>(request.session_id));
        if (acceptable && queue_.size() < 32 &&
            accepted_images_.insert(request.image_path).second) {
            queue_.push({static_cast<int>(request.session_id), request.slot_id,
                         request.image_path, false, request.enhanced_path, true,
                         request.stage, false, {}, {}});
            condition_.notify_one();
            return;
        }
        rejected = hall_callback_;
    }
    util::logWarn("hall capture OCR rejected: path=" + request.image_path);
    if (rejected) {
        HallCaptureResult result;
        result.session_id = request.session_id;
        result.stage = request.stage;
        result.error_message = "OCR task was rejected by the bounded queue";
        result.processed_at = util::nowIsoString();
        rejected(result);
    }
}

bool OcrWorker::enqueueGeneric(const std::string& task_id,
                               const std::string& image_path,
                               GenericOcrCallback callback) {
    if (task_id.empty() || image_path.empty() || !callback) return false;
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || queue_.size() >= 32 ||
        !accepted_images_.insert(image_path).second) return false;
    Task task{-1, {}, image_path, false, {}, false, 0, true, task_id,
              std::move(callback)};
    queue_.push(std::move(task));
    condition_.notify_one();
    return true;
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
            if (task.session_id >= 0 &&
                canceled_session_ids_.contains(task.session_id)) {
                continue;
            }
        }

        HallCaptureResult hall_result;
        hall_result.session_id = task.session_id;
        hall_result.stage = task.hall_stage;
        hall_result.processed_at = util::nowIsoString();
        GenericOcrResult generic_result;
        GenericOcrResult* generic_output = nullptr;
        if (task.generic) {
            generic_result.task_id = task.generic_task_id;
            generic_output = &generic_result;
        }
        process(task, hall_result, generic_output);

        if (task.generic) {
            {
                std::lock_guard lock(mutex_);
                accepted_images_.erase(task.image_path);
            }
            try {
                task.generic_callback(generic_result);
            } catch (const std::exception& error) {
                util::logError("Generic OCR callback failed: " +
                               std::string(error.what()));
            } catch (...) {
                util::logError("Generic OCR callback failed: unknown error");
            }
            continue;
        }

        if (task.hall) {
            HallCaptureCallback callback;
            {
                std::lock_guard lock(mutex_);
                if (canceled_session_ids_.contains(task.session_id)) continue;
                callback = hall_callback_;
            }
            if (callback) {
                try {
                    callback(hall_result);
                } catch (const std::exception& error) {
                    util::logError("Hall OCR callback failed: " +
                                   std::string(error.what()));
                } catch (...) {
                    util::logError("Hall OCR callback failed: unknown error");
                }
            }
        }
    }
}

void OcrWorker::process(const Task& task, HallCaptureResult& hall_result,
                        GenericOcrResult* generic_result) {
        PlatePreprocessResult processed;
        bool owns_processed_enhanced = false;
        if (!task.provided_enhanced_path.empty())
            processed.enhanced_path = task.provided_enhanced_path;
        if (!preprocess_enabled_ && task.detect_candidate) {
            util::logInfo("IVA plate preprocessing disabled; OCR skipped: path=" +
                          task.image_path);
            return;
        }
        if (preprocess_enabled_ && task.provided_enhanced_path.empty()) {
            processed = preprocessPlateImage(task.image_path, task.detect_candidate);
            owns_processed_enhanced = !processed.enhanced_path.empty();
            if (task.detect_candidate && !processed.candidate_detected) {
                util::logInfo("No plausible plate candidate in IVA scene: path=" +
                              task.image_path);
                return;
            }
            if (!processed.enhanced_path.empty()) {
                if (!task.generic)
                    database_.attachEnhancedPlateImage(task.image_path,
                                                        processed.enhanced_path);
                util::logLine("PLATE_PREPROCESS", "original=" + task.image_path +
                              " enhanced=" + processed.enhanced_path);
            }
        }

        // Hall 30/60초 촬영은 카메라가 만든 개선본을 OCR의 단일 우선 입력으로
        // 사용한다. 개선본이 없거나 손상된 경우에만 원본으로 안전하게 돌아간다.
        // 일반 BestShot/IVA OCR은 기존 호환을 위해 원본+개선본 비교를 유지한다.
        std::string ocr_input = task.image_path;
        std::string secondary_input = processed.enhanced_path;
        if (task.hall) {
            std::string input_kind{"original_fallback"};
            if (usableImageFile(processed.enhanced_path)) {
                ocr_input = processed.enhanced_path;
                input_kind = task.provided_enhanced_path.empty()
                    ? "generated_enhanced"
                    : "camera_enhanced";
            }
            secondary_input.clear();
            const std::string selection =
                "session=" + std::to_string(task.session_id) +
                " slot=" + task.slot_id + " input=" + input_kind +
                " path=" + ocr_input;
            util::logLine("HALL_OCR", "OCR_INPUT " + selection);
            if (!database_.insertSystemEvent(
                    "HALL_OCR_INPUT_SELECTED", task.slot_id, selection)) {
                util::logWarn("Hall OCR input selection event was not stored: " +
                              selection);
            }
        }

        const auto session_canceled = [this, &task] {
            std::lock_guard<std::mutex> lock(mutex_);
            return task.session_id >= 0 &&
                   canceled_session_ids_.contains(task.session_id);
        };
        const auto remove_owned_enhanced = [&processed,
                                             &owns_processed_enhanced] {
            if (!owns_processed_enhanced || processed.enhanced_path.empty())
                return;
            std::error_code ignored;
            std::filesystem::remove(processed.enhanced_path, ignored);
        };
        if (session_canceled()) {
            remove_owned_enhanced();
            return;
        }

        OcrResult result;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            if (generic_result != nullptr) generic_result->attempts = attempt;
            if (session_canceled()) {
                remove_owned_enhanced();
                return;
            }
            result = client_.recognizePlate(ocr_input, secondary_input);
            if (result.success) break;
            // VACANT 처리 중 파일이 정리된 세션은 실패 재시도를 하지 않는다.
            if (session_canceled()) {
                remove_owned_enhanced();
                return;
            }
            if (attempt == 3 || !result.retryable()) break;
            const int delay_seconds = attempt;
            util::logWarn("Gemini OCR retry " + std::to_string(attempt + 1) +
                          "/3 after " + std::to_string(delay_seconds) +
                          "s kind=" + toString(result.error_kind) +
                          " status=" + std::to_string(result.http_status) +
                          " error=" + result.error);
            std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
        }
        if (!result.success) {
            hall_result.error_message = result.error;
            hall_result.raw_text = result.raw_text;
            if (generic_result != nullptr) {
                generic_result->error_message = result.error;
                generic_result->raw_text = result.raw_text;
                generic_result->error_kind = result.error_kind;
                generic_result->http_status = result.http_status;
            }
            util::logError("Gemini OCR failed: path=" + ocr_input +
                           " kind=" + toString(result.error_kind) +
                           " status=" + std::to_string(result.http_status) +
                           " error=" + result.error);
            return;
        }
        hall_result.ocr_succeeded = true;
        hall_result.raw_text = result.raw_text;
        if (generic_result != nullptr) {
            generic_result->ocr_succeeded = true;
            generic_result->raw_text = result.raw_text;
            generic_result->error_kind = result.error_kind;
            generic_result->http_status = result.http_status;
        }
        if (session_canceled()) {
            remove_owned_enhanced();
            return;
        }
        std::string plate = normalizePlateNumber(result.plate_number);
        if (!result.readable || !isPlausibleKoreanPlate(plate)) {
            // 응답은 받았으나 번호판을 읽지 못한 경우다. 조명 보정으로 다시
            // 시도해볼 값어치가 있어 요청 실패와 구분해 표시한다.
            hall_result.plate_unreadable = true;
            if (generic_result != nullptr)
                generic_result->plate_unreadable = true;
            util::logWarn("Gemini OCR unreadable: path=" + ocr_input);
            if (task.generic) return;
            database_.applyPlateOcr(task.session_id, task.slot_id,
                                    task.image_path, "", result.confidence);
            return;
        }
        if (generic_result != nullptr) {
            generic_result->recognized = true;
            generic_result->plate_number = plate;
            generic_result->confidence = result.confidence;
            return;
        }
        const database::ParkingPlateResolution resolution =
            database_.applyPlateOcrWithEntrance(
                task.session_id, task.slot_id, task.image_path, plate,
                result.confidence, entrance_match_window_ms_,
                entrance_match_min_confidence_);
        const std::string canonicalPlate = resolution.canonical_plate.empty()
            ? plate : resolution.canonical_plate;
        const std::string classification = resolution.classification;
        util::logLine("PLATE_OCR", "slot=" + task.slot_id +
                      " observed=" + plate + " canonical=" + canonicalPlate +
                      " class=" + classification + " source=" +
                      resolution.source +
                      " confidence=" + std::to_string(result.confidence));
        hall_result.recognized = true;
        hall_result.plate_number = canonicalPlate;
        hall_result.confidence = result.confidence;
        hall_result.classification = classification;
        if (!task.hall && result_callback_ && task.session_id >= 0) {
            try {
                result_callback_({task.session_id, task.slot_id, canonicalPlate,
                                  classification, result.confidence});
            } catch (const std::exception& error) {
                util::logError("OCR result callback failed: " +
                               std::string(error.what()));
            } catch (...) {
                util::logError("OCR result callback failed: unknown error");
            }
        }
}

}

#pragma once

#include "database/EventDatabase.hpp"
#include "ocr/GeminiOcrClient.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>

namespace ocr {

/**
 * @brief 이미지 전처리와 Gemini OCR을 서버 주 흐름에서 분리한 단일 worker queue다.
 *
 * 동일 이미지 중복 enqueue를 억제하고, 결과를 EventDatabase에 연결한다.
 */
class OcrWorker {
public:
    struct RecognitionResult {
        int session_id{-1};
        std::string slot_id;
        std::string plate_number;
        std::string classification;
        double confidence{};
    };

    using ResultCallback = std::function<void(const RecognitionResult&)>;

    OcrWorker(GeminiOcrClient client, database::EventDatabase& database,
              bool preprocess_enabled, ResultCallback result_callback = {});
    ~OcrWorker();

    /** @brief OCR worker thread를 시작한다. */
    void start();
    /** @brief 남은 worker를 깨워 종료하고 join한다. */
    void stop();
    /** @brief BestShot 이미지를 기존 session의 OCR 작업으로 등록한다. */
    void enqueue(int session_id, const std::string& slot_id,
                 const std::string& image_path);
    /** @brief 세션이 아직 없는 IVA scene을 후보 탐색 OCR 작업으로 등록한다. */
    void enqueueScene(const std::string& slot_id,
                      const std::string& image_path,
                      const std::string& enhanced_image_path);
    /** @brief 조기 출차 세션의 대기/진행 OCR 결과가 DB와 타이머에 반영되지 않게 한다. */
    void cancelSession(int session_id);
    /** @brief Gemini client가 실제 요청 가능한 상태인지 반환한다. */
    bool enabled() const;

private:
    struct Task {
        int session_id;
        std::string slot_id;
        std::string image_path;
        bool detect_candidate;
        std::string provided_enhanced_path;
    };

    /** @brief queue 대기, 전처리, OCR, 정규화와 DB 반영을 반복한다. */
    void run();

    GeminiOcrClient client_;
    database::EventDatabase& database_;
    bool preprocess_enabled_;
    ResultCallback result_callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Task> queue_;
    std::unordered_set<std::string> accepted_images_;
    std::unordered_set<int> canceled_session_ids_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
};

}

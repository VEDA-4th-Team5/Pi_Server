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

// 홀센서 세션의 30초/60초 촬영본 한 장 (EVDA-136).
// session_key/stage는 이 워커가 해석하지 않고 결과에 그대로 되돌려 주는 값이다.
// 상위 계층(parking::HallCaptureCoordinator)이 그것으로 재시도 정책을 판단한다.
struct HallCaptureTask {
    std::string session_key;   // 상위 계층의 문자열 세션 ID
    int session_id{-1};        // PARKING_SESSION.session_id
    int stage{0};              // 0 = 30초, 1 = 60초
    std::string slot_id;
    std::string image_path;
    std::string enhanced_path;  // 이미 개선본이 있으면 전달, 없으면 비움
};

struct HallCaptureResult {
    std::string session_key;
    int stage{0};
    bool recognized{false};  // 그럴듯한 번호판을 실제로 읽었을 때만 true
    std::string plate_number;
    double confidence{0.0};
    std::string classification;  // EV / PHEV / NON_EV / UNKNOWN / OCR_FAILED
};

// 홀 촬영 OCR이 끝날 때마다 정확히 한 번 호출된다. 전처리 생략·후보 미검출·
// HTTP 실패처럼 일찍 끝나는 경로에서도 반드시 불린다. 그러지 않으면 상위
// 재시도 정책이 응답을 영원히 기다린다.
using HallCaptureCallback = std::function<void(const HallCaptureResult&)>;

class OcrWorker {
public:
    OcrWorker(GeminiOcrClient client, database::EventDatabase& database,
              bool preprocess_enabled);
    ~OcrWorker();

    void start();
    void stop();
    void enqueue(int session_id, const std::string& slot_id,
                 const std::string& image_path);
    void enqueueScene(const std::string& slot_id,
                      const std::string& image_path,
                      const std::string& enhanced_image_path);
    // 언제든 지정할 수 있다. 콜백은 OCR 워커 스레드에서 불리므로 블로킹하면 안 된다.
    void setHallCaptureCallback(HallCaptureCallback callback);
    void enqueueHallCapture(const HallCaptureTask& task);
    bool enabled() const;

private:
    struct Task {
        int session_id;
        std::string slot_id;
        std::string image_path;
        bool detect_candidate;
        std::string provided_enhanced_path;
        bool hall{false};
        std::string hall_session_key;
        int hall_stage{0};
    };

    void run();
    // 한 건을 처리하고 홀 경로면 result를 채운다.
    void process(const Task& task, HallCaptureResult& result);

    GeminiOcrClient client_;
    database::EventDatabase& database_;
    bool preprocess_enabled_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Task> queue_;
    std::unordered_set<std::string> accepted_images_;
    HallCaptureCallback hall_callback_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
};

}

#pragma once

#include "database/EventDatabase.hpp"
#include "ocr/GeminiOcrClient.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>

namespace ocr {

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
    bool enabled() const;

private:
    struct Task {
        int session_id;
        std::string slot_id;
        std::string image_path;
        bool detect_candidate;
        std::string provided_enhanced_path;
    };

    void run();

    GeminiOcrClient client_;
    database::EventDatabase& database_;
    bool preprocess_enabled_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Task> queue_;
    std::unordered_set<std::string> accepted_images_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
};

}

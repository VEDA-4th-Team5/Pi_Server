#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace entrance {

struct EntranceEvTask {
    std::string requestId;
    std::string imagePath;
    std::string outputDirectory;
    std::string sourceMode{"rgb"};
};

struct EntranceEvResult {
    std::string requestId;
    std::optional<bool> isEv;
    std::string decision{"REVIEW"};
    std::string reason;
    std::string modelVersion;
    double processingMs{};
    std::string resultPath;
    std::string error;
    bool runtimeSucceeded{};
};

using EntranceEvCallback = std::function<void(const EntranceEvResult&)>;

/** Python EV 사전 판별 프로세스 하나와 bounded 작업 큐를 소유한다. */
class EntranceEvWorker {
public:
    struct Config {
        std::string pythonExecutable;
        std::string workerScript;
        std::string modelBundleDirectory;
        std::string templateCachePath;
        std::string thresholdsPath;
        int timeoutMs{5000};
        std::size_t queueCapacity{16};
        int opencvThreads{1};
    };

    explicit EntranceEvWorker(Config config);
    ~EntranceEvWorker();

    EntranceEvWorker(const EntranceEvWorker&) = delete;
    EntranceEvWorker& operator=(const EntranceEvWorker&) = delete;

    bool start();
    void stop();
    bool enqueue(EntranceEvTask task, EntranceEvCallback callback);
    [[nodiscard]] std::size_t queuedCount() const;

private:
    struct QueuedTask {
        EntranceEvTask task;
        EntranceEvCallback callback;
    };

    void run();
    EntranceEvResult process(const EntranceEvTask& task);
    bool spawnChild();
    void stopChild() noexcept;
    bool writeRequest(const std::string& request);
    std::optional<std::string> readResponseLine(int timeoutMs);
    static EntranceEvResult reviewResult(const EntranceEvTask& task,
                                         std::string error);

    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedTask> queue_;
    std::thread worker_;
    bool started_{};
    bool stopping_{};
    int childPid_{-1};
    int childInputFd_{-1};
    int childOutputFd_{-1};
    std::string responseBuffer_;
};

}  // namespace entrance

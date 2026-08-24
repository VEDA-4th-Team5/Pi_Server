#include "entrance/EntranceEvWorker.hpp"

#include "util/Logger.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace entrance {
namespace {

constexpr std::size_t kMaximumResponseBytes = 256U * 1024U;

void closeFd(int& descriptor) noexcept {
    if (descriptor >= 0) close(descriptor);
    descriptor = -1;
}

std::vector<char*> argumentPointers(std::vector<std::string>& arguments) {
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return pointers;
}

}  // namespace

EntranceEvWorker::EntranceEvWorker(Config config)
    : config_(std::move(config)) {}

EntranceEvWorker::~EntranceEvWorker() { stop(); }

bool EntranceEvWorker::start() {
    std::lock_guard lock(mutex_);
    if (started_) return true;
    if (config_.pythonExecutable.empty() || config_.workerScript.empty() ||
        config_.modelBundleDirectory.empty() || config_.timeoutMs <= 0 ||
        config_.queueCapacity == 0 || config_.opencvThreads <= 0 ||
        !fs::is_regular_file(config_.workerScript) ||
        !fs::is_directory(config_.modelBundleDirectory)) {
        util::logError("Entrance EV worker configuration is invalid");
        return false;
    }
    if (!spawnChild()) return false;
    stopping_ = false;
    started_ = true;
    worker_ = std::thread(&EntranceEvWorker::run, this);
    return true;
}

void EntranceEvWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_) {
            stopChild();
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    stopChild();
    std::lock_guard lock(mutex_);
    started_ = false;
    stopping_ = false;
}

bool EntranceEvWorker::enqueue(EntranceEvTask task,
                               EntranceEvCallback callback) {
    if (task.requestId.empty() || task.imagePath.empty() ||
        task.outputDirectory.empty() || !callback) return false;
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || queue_.size() >= config_.queueCapacity)
        return false;
    queue_.push_back({std::move(task), std::move(callback)});
    condition_.notify_one();
    return true;
}

std::size_t EntranceEvWorker::queuedCount() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void EntranceEvWorker::run() {
    sigset_t blockedSignals;
    sigemptyset(&blockedSignals);
    sigaddset(&blockedSignals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);

    while (true) {
        QueuedTask queued;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            queued = std::move(queue_.front());
            queue_.pop_front();
        }

        const EntranceEvResult result = process(queued.task);
        try {
            queued.callback(result);
        } catch (const std::exception& error) {
            util::logError("Entrance EV callback failed: " +
                           std::string(error.what()));
        } catch (...) {
            util::logError("Entrance EV callback failed: unknown error");
        }
    }
}

EntranceEvResult EntranceEvWorker::process(const EntranceEvTask& task) {
    if (childPid_ < 0 && !spawnChild())
        return reviewResult(task, "worker_start_failed");

    const std::string request = json{
        {"schema", "entrance_ev_analysis_request_v1"},
        {"request_id", task.requestId},
        {"image_path", task.imagePath},
        {"output_dir", task.outputDirectory},
        {"source_mode", task.sourceMode}}.dump() + "\n";
    if (!writeRequest(request)) {
        stopChild();
        return reviewResult(task, "worker_write_failed");
    }
    const auto response = readResponseLine(config_.timeoutMs);
    if (!response.has_value()) {
        stopChild();
        return reviewResult(task, "worker_timeout_or_closed");
    }

    try {
        const json payload = json::parse(*response);
        if (!payload.is_object() ||
            payload.value("schema", "") !=
                "low_quality_presence_runtime_result_v1" ||
            payload.value("request_id", "") != task.requestId) {
            return reviewResult(task, "worker_response_contract_mismatch");
        }

        EntranceEvResult result;
        result.requestId = task.requestId;
        result.decision = payload.value("ev_prescreen_decision", "REVIEW");
        result.reason = payload.value("reason", "");
        result.modelVersion = payload.value("model_version", "");
        result.processingMs = payload.value("processing_ms", 0.0);
        result.resultPath = payload.value("result_path", "");
        result.error = payload.value("error", "");
        result.runtimeSucceeded = payload.value("status", "") == "ok" &&
                                  result.error.empty();
        if (!result.runtimeSucceeded) {
            result.decision = "REVIEW";
            return result;
        }
        if (result.decision == "EV_CANDIDATE") {
            result.isEv = true;
        } else if (result.decision == "NON_EV_CANDIDATE") {
            result.isEv = false;
        } else if (result.decision == "REVIEW") {
            result.isEv.reset();
        } else {
            stopChild();
            return reviewResult(task, "worker_decision_invalid");
        }
        return result;
    } catch (const std::exception& error) {
        const std::string failure =
            "worker_json_invalid:" + std::string(error.what());
        stopChild();
        return reviewResult(task, failure);
    }
}

bool EntranceEvWorker::spawnChild() {
    stopChild();
    int requestPipe[2]{-1, -1};
    int responsePipe[2]{-1, -1};
    if (pipe2(requestPipe, O_CLOEXEC) != 0 ||
        pipe2(responsePipe, O_CLOEXEC) != 0) {
        if (requestPipe[0] >= 0) close(requestPipe[0]);
        if (requestPipe[1] >= 0) close(requestPipe[1]);
        if (responsePipe[0] >= 0) close(responsePipe[0]);
        if (responsePipe[1] >= 0) close(responsePipe[1]);
        util::logError("Entrance EV worker pipe creation failed: " +
                       std::string(std::strerror(errno)));
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, requestPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, responsePipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, requestPipe[1]);
    posix_spawn_file_actions_addclose(&actions, responsePipe[0]);

    std::vector<std::string> arguments{
        config_.pythonExecutable,
        config_.workerScript,
        "--model-bundle-dir", config_.modelBundleDirectory,
        "--template-cache", config_.templateCachePath,
        "--thresholds", config_.thresholdsPath,
        "--opencv-threads", std::to_string(config_.opencvThreads)};
    auto pointers = argumentPointers(arguments);
    pid_t child{};
    const int result = config_.pythonExecutable.find('/') == std::string::npos
        ? posix_spawnp(&child, config_.pythonExecutable.c_str(), &actions,
                       nullptr, pointers.data(), environ)
        : posix_spawn(&child, config_.pythonExecutable.c_str(), &actions,
                      nullptr, pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(requestPipe[0]);
    close(responsePipe[1]);
    if (result != 0) {
        close(requestPipe[1]);
        close(responsePipe[0]);
        util::logError("Entrance EV worker spawn failed: " +
                       std::string(std::strerror(result)));
        return false;
    }

    childPid_ = static_cast<int>(child);
    childInputFd_ = requestPipe[1];
    childOutputFd_ = responsePipe[0];
    responseBuffer_.clear();
    const auto ready = readResponseLine(std::max(config_.timeoutMs, 15000));
    try {
        if (!ready.has_value())
            throw std::runtime_error("worker readiness timeout");
        const json payload = json::parse(*ready);
        if (!payload.is_object() ||
            payload.value("schema", "") != "entrance_ev_worker_ready_v1" ||
            payload.value("status", "") != "ready") {
            throw std::runtime_error("worker readiness contract mismatch");
        }
    } catch (const std::exception& error) {
        util::logError("Entrance EV worker readiness failed: " +
                       std::string(error.what()));
        stopChild();
        return false;
    }
    return true;
}

void EntranceEvWorker::stopChild() noexcept {
    closeFd(childInputFd_);
    closeFd(childOutputFd_);
    responseBuffer_.clear();
    if (childPid_ < 0) return;
    const pid_t child = static_cast<pid_t>(childPid_);
    childPid_ = -1;
    int status{};
    for (int attempt = 0; attempt < 10; ++attempt) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        usleep(20000);
    }
    kill(child, SIGTERM);
    for (int attempt = 0; attempt < 25; ++attempt) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        usleep(20000);
    }
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
}

bool EntranceEvWorker::writeRequest(const std::string& request) {
    std::size_t written{};
    while (written < request.size()) {
        const ssize_t result = write(childInputFd_, request.data() + written,
                                     request.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::optional<std::string> EntranceEvWorker::readResponseLine(
    const int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (true) {
        const auto newline = responseBuffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = responseBuffer_.substr(0, newline);
            responseBuffer_.erase(0, newline + 1U);
            return line;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return std::nullopt;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd descriptor{childOutputFd_, POLLIN | POLLHUP | POLLERR, 0};
        const int polled = poll(&descriptor, 1,
                                static_cast<int>(remaining.count()));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) return std::nullopt;

        std::array<char, 4096> buffer{};
        const ssize_t count = read(childOutputFd_, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::nullopt;
        responseBuffer_.append(buffer.data(), static_cast<std::size_t>(count));
        if (responseBuffer_.size() > kMaximumResponseBytes)
            return std::nullopt;
    }
}

EntranceEvResult EntranceEvWorker::reviewResult(const EntranceEvTask& task,
                                                std::string error) {
    EntranceEvResult result;
    result.requestId = task.requestId;
    result.reason = error;
    result.error = std::move(error);
    return result;
}

}  // namespace entrance

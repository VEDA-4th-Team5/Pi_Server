#include "entrance/EntranceEvWorker.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int runFakeWorker() {
    std::cout << json{{"schema", "entrance_ev_worker_ready_v1"},
                      {"status", "ready"}}.dump()
              << '\n' << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        const json request = json::parse(line);
        const std::string requestId = request.value("request_id", "");
        if (requestId == "timeout") {
            std::this_thread::sleep_for(400ms);
        }
        if (requestId == "malformed") {
            std::cout << "not-json\n" << std::flush;
            continue;
        }
        const bool nonEv = requestId.find("non-ev") != std::string::npos;
        const json response{
            {"schema", "low_quality_presence_runtime_result_v1"},
            {"request_id", requestId},
            {"status", "ok"},
            {"ev_prescreen_decision",
             nonEv ? "NON_EV_CANDIDATE" : "EV_CANDIDATE"},
            {"reason", "fake_policy"},
            {"model_version", "fake-v1"},
            {"processing_ms", 12.5},
            {"result_path", request.value("output_dir", "") + "/result.json"},
            {"error", ""}};
        std::cout << response.dump() << '\n' << std::flush;
    }
    return EXIT_SUCCESS;
}

void workerRecoversAfterTimeout(const std::filesystem::path& executable) {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-ev-worker-test";
    std::filesystem::create_directories(root / "model");
    const auto fakeScript = root / "fake-worker.py";
    std::ofstream(fakeScript) << "fake";

    entrance::EntranceEvWorker::Config config;
    config.pythonExecutable = executable.string();
    config.workerScript = fakeScript.string();
    config.modelBundleDirectory = (root / "model").string();
    config.templateCachePath = (root / "cache.json").string();
    config.thresholdsPath = (root / "thresholds.json").string();
    config.timeoutMs = 150;
    config.queueCapacity = 8;
    entrance::EntranceEvWorker worker(std::move(config));
    require(worker.start(), "fake EV worker did not start");

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<entrance::EntranceEvResult> results;
    const auto enqueue = [&](const std::string& id) {
        entrance::EntranceEvTask task{id, "plate.jpg", (root / id).string(), "rgb"};
        require(worker.enqueue(
                    std::move(task),
                    [&](const entrance::EntranceEvResult& result) {
                        {
                            std::lock_guard lock(mutex);
                            results.push_back(result);
                        }
                        condition.notify_one();
                    }),
                "EV analysis task was rejected");
    };

    enqueue("ev-1");
    enqueue("non-ev-1");
    enqueue("malformed");
    enqueue("ev-after-malformed");
    enqueue("timeout");
    enqueue("ev-after-restart");
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 3s, [&] { return results.size() == 6; }),
                "EV worker callbacks timed out");
    }
    worker.stop();

    require(results[0].runtimeSucceeded && results[0].isEv == true,
            "EV candidate was not parsed");
    require(results[1].runtimeSucceeded && results[1].isEv == false,
            "non-EV candidate was not parsed");
    require(!results[2].runtimeSucceeded && !results[2].isEv.has_value() &&
                results[2].decision == "REVIEW",
            "malformed response did not become REVIEW");
    require(results[3].runtimeSucceeded && results[3].isEv == true,
            "worker did not restart after malformed response");
    require(!results[4].runtimeSucceeded && !results[4].isEv.has_value() &&
                results[4].decision == "REVIEW",
            "timeout did not become REVIEW");
    require(results[5].runtimeSucceeded && results[5].isEv == true,
            "worker did not restart after timeout");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void realWorkerSmoke(const std::filesystem::path& projectRoot) {
    const auto packageRoot =
        projectRoot / "tools/cv/low_quality_presence_v1";
    const auto outputRoot = std::filesystem::temp_directory_path() /
                            "entrance-ev-worker-real-smoke";

    entrance::EntranceEvWorker::Config config;
    config.pythonExecutable = "/usr/bin/python3";
    config.workerScript = (packageRoot / "runtime/pi_worker.py").string();
    config.modelBundleDirectory = (packageRoot / "model_bundle").string();
    config.templateCachePath =
        (packageRoot / "model_bundle/runtime_template_cache.json").string();
    config.thresholdsPath =
        (packageRoot / "model_bundle/default_thresholds.json").string();
    config.timeoutMs = 5000;
    config.queueCapacity = 1;
    entrance::EntranceEvWorker worker(std::move(config));
    require(worker.start(), "real EV worker did not become ready");

    std::mutex mutex;
    std::condition_variable condition;
    std::optional<entrance::EntranceEvResult> received;
    entrance::EntranceEvTask task{
        "real-smoke",
        (packageRoot / "examples/runtime_smoke/baseline_rectified.jpg").string(),
        outputRoot.string(),
        "rgb"};
    require(worker.enqueue(
                std::move(task),
                [&](const entrance::EntranceEvResult& result) {
                    {
                        std::lock_guard lock(mutex);
                        received = result;
                    }
                    condition.notify_one();
                }),
            "real EV task was rejected");
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 15s,
                                   [&] { return received.has_value(); }),
                "real EV worker callback timed out");
    }
    worker.stop();
    require(received->runtimeSucceeded && received->isEv == true &&
                received->decision == "EV_CANDIDATE" &&
                std::filesystem::is_regular_file(received->resultPath),
            "real EV worker result contract failed");

    std::error_code ignored;
    std::filesystem::remove_all(outputRoot, ignored);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 &&
        std::filesystem::path(argv[1]).filename() == "fake-worker.py") {
        return runFakeWorker();
    }
    try {
        if (argc == 3 && std::string(argv[1]) == "--real-worker") {
            realWorkerSmoke(std::filesystem::absolute(argv[2]));
            std::cout << "EntranceEvWorker real smoke passed\n";
            return EXIT_SUCCESS;
        }
        workerRecoversAfterTimeout(std::filesystem::absolute(argv[0]));
        std::cout << "EntranceEvWorkerTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceEvWorkerTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

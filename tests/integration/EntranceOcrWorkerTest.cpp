#include "database/EventDatabase.hpp"
#include "ocr/OcrWorker.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string successEnvelope() {
    return "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
           "\"{\\\"readable\\\":true,\\\"plate_number\\\":\\\"12가3456\\\","
           "\\\"confidence\\\":0.95}\"}]}}]}";
}

void transientFailureRetriesWithinExistingPolicy() {
    const auto root = std::filesystem::temp_directory_path() /
                      "entrance-ocr-worker-test";
    std::filesystem::create_directories(root);
    const auto image = root / "plate.jpg";
    {
        std::ofstream output(image, std::ios::binary);
        const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
        output.write(reinterpret_cast<const char*>(jpeg), sizeof(jpeg));
    }

    int calls{};
    ocr::GeminiOcrClient client(
        "test-key", "test-model", 1, 1, "",
        [&calls](const ocr::GeminiHttpRequest&) {
            ++calls;
            if (calls == 1)
                return ocr::GeminiHttpResponse{true, 429, "{}", {}};
            return ocr::GeminiHttpResponse{true, 200, successEnvelope(), {}};
        });
    database::EventDatabase unopenedDatabase;
    ocr::OcrWorker worker(std::move(client), unopenedDatabase, false);
    std::mutex mutex;
    std::condition_variable condition;
    bool completed{};
    ocr::GenericOcrResult result;
    worker.start();
    require(worker.enqueueGeneric(
                "cam01|ch02|123", image.string(),
                [&](const ocr::GenericOcrResult& value) {
                    {
                        std::lock_guard lock(mutex);
                        result = value;
                        completed = true;
                    }
                    condition.notify_one();
                }),
            "generic entrance OCR was not accepted");
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 5s, [&] { return completed; }),
                "generic entrance OCR callback timed out");
    }
    worker.stop();
    require(calls == 2 && result.attempts == 2 && result.recognized &&
                result.plate_number == "12가3456",
            "429 did not produce one bounded retry followed by success");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    try {
        transientFailureRetriesWithinExistingPolicy();
        std::cout << "EntranceOcrWorkerTest passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "EntranceOcrWorkerTest failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

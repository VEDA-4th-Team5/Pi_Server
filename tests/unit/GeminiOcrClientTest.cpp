#include "ocr/GeminiOcrClient.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string successEnvelope(const std::string& plate = "12가3456") {
    return std::string{"{\"candidates\":[{\"content\":{\"parts\":[{\"text\":"}
        + "\"{\\\"readable\\\":true,\\\"plate_number\\\":\\\"" +
        plate + "\\\",\\\"confidence\\\":0.95}\"}]}}]}";
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("gemini-client-test-" + std::to_string(unique));
    const fs::path jpeg = root / "plate.bin";
    fs::create_directories(root);
    {
        std::ofstream output(jpeg, std::ios::binary);
        const unsigned char data[] = {0xFF, 0xD8, 0xFF, 0xD9};
        output.write(reinterpret_cast<const char*>(data), sizeof(data));
    }

    try {
        int calls = 0;
        ocr::GeminiOcrClient successClient(
            "secret", "model", 1, 2, "",
            [&calls](const ocr::GeminiHttpRequest& request) {
                ++calls;
                require(request.apiKey == "secret", "API key transport input");
                require(request.body.find("image/jpeg") != std::string::npos,
                        "MIME must follow JPEG magic bytes");
                return ocr::GeminiHttpResponse{
                    true, 200, successEnvelope(), {}};
            });
        auto result = successClient.recognizePlate(jpeg.string());
        require(result.success && result.readable &&
                    result.plate_number == "12가3456" &&
                    !result.raw_text.empty() && calls == 1,
                "structured Gemini success response");

        for (const long status : {400L, 401L, 403L, 429L, 500L}) {
            calls = 0;
            ocr::GeminiOcrClient client(
                "secret", "model", 1, 2, "fallback",
                [&calls, status](const ocr::GeminiHttpRequest&) {
                    ++calls;
                    return ocr::GeminiHttpResponse{true, status, "{}", {}};
                });
            result = client.recognizePlate(jpeg.string());
            require(!result.success && calls == 1,
                    "HTTP failure made an unexpected fallback request");
            if (status == 401 || status == 403) {
                require(result.error_kind == ocr::OcrErrorKind::Authentication &&
                            !result.retryable(),
                        "authentication errors must not retry");
            } else if (status == 429) {
                require(result.error_kind == ocr::OcrErrorKind::RateLimited &&
                            result.retryable(),
                        "429 must be retryable");
            } else if (status >= 500) {
                require(result.error_kind == ocr::OcrErrorKind::Server &&
                            result.retryable(),
                        "5xx must be retryable");
            } else {
                require(result.error_kind == ocr::OcrErrorKind::Client &&
                            !result.retryable(),
                        "ordinary 4xx must not retry");
            }
        }

        calls = 0;
        ocr::GeminiOcrClient timeoutClient(
            "secret", "model", 1, 2, "",
            [&calls](const ocr::GeminiHttpRequest&) {
                ++calls;
                return ocr::GeminiHttpResponse{
                    false, 0, {}, "operation timed out"};
            });
        result = timeoutClient.recognizePlate(jpeg.string());
        require(!result.success && result.retryable() && calls == 1,
                "transport timeout classification");

        ocr::GeminiOcrClient malformedClient(
            "secret", "model", 1, 2, "",
            [](const ocr::GeminiHttpRequest&) {
                return ocr::GeminiHttpResponse{true, 200, "not-json", {}};
            });
        result = malformedClient.recognizePlate(jpeg.string());
        require(!result.success &&
                    result.error_kind == ocr::OcrErrorKind::ResponseParse &&
                    !result.retryable(),
                "malformed JSON must not retry");

        calls = 0;
        ocr::GeminiOcrClient fallbackClient(
            "secret", "missing-model", 1, 2, "fallback",
            [&calls](const ocr::GeminiHttpRequest&) {
                ++calls;
                if (calls == 1)
                    return ocr::GeminiHttpResponse{true, 404, "{}", {}};
                return ocr::GeminiHttpResponse{
                    true, 200, successEnvelope("34나5678"), {}};
            });
        result = fallbackClient.recognizePlate(jpeg.string());
        require(result.success && result.plate_number == "34나5678" &&
                    calls == 2,
                "404 model fallback");

        fs::remove_all(root);
        return 0;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        throw;
    }
}

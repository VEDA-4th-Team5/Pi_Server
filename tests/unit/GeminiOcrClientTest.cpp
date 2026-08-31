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

        // usageMetadata 가 있으면 실제 청구 토큰이 결과에 실려야 한다.
        ocr::GeminiOcrClient usageClient(
            "secret", "model", 1, 2, "",
            [](const ocr::GeminiHttpRequest&) {
                std::string body = successEnvelope();
                body.insert(body.size() - 1,
                    ",\"usageMetadata\":{\"promptTokenCount\":273,"
                    "\"candidatesTokenCount\":18,\"totalTokenCount\":291}");
                return ocr::GeminiHttpResponse{true, 200, body, {}};
            });
        result = usageClient.recognizePlate(jpeg.string());
        require(result.success && result.prompt_token_count == 273 &&
                    result.candidates_token_count == 18 &&
                    result.total_token_count == 291,
                "usageMetadata must be captured from the response");
        require(result.image_count == 1 && result.image_bytes == 4,
                "single image request must report one image and its bytes");

        // 이미지 2장이면 image_count 가 2 여야 한다(§7.2 중복 전송 추적용).
        ocr::GeminiOcrClient twoImageClient(
            "secret", "model", 1, 2, "",
            [](const ocr::GeminiHttpRequest&) {
                return ocr::GeminiHttpResponse{true, 200, successEnvelope(), {}};
            });
        result = twoImageClient.recognizePlate(jpeg.string(), jpeg.string());
        require(result.image_count == 2 && result.image_bytes == 8,
                "two image request must report both images");

        // usageMetadata 가 없으면 0 이 아니라 -1(미측정)로 남아야 한다.
        require(result.prompt_token_count == -1 &&
                    result.total_token_count == -1,
                "absent usageMetadata must stay -1, not 0");

        fs::remove_all(root);
        return 0;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        throw;
    }
}

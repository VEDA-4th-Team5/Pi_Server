#pragma once

#include <functional>
#include <string>

namespace ocr {

enum class OcrErrorKind {
    None,
    Configuration,
    Image,
    Transport,
    Authentication,
    RateLimited,
    Server,
    Client,
    ResponseParse
};

[[nodiscard]] const char* toString(OcrErrorKind kind) noexcept;

struct OcrResult {
    bool success{false};
    bool readable{false};
    std::string plate_number;
    double confidence{0.0};
    std::string raw_text;
    std::string error;
    OcrErrorKind error_kind{OcrErrorKind::None};
    long http_status{};

    [[nodiscard]] bool retryable() const noexcept {
        return error_kind == OcrErrorKind::Transport ||
               error_kind == OcrErrorKind::RateLimited ||
               error_kind == OcrErrorKind::Server;
    }
};

struct GeminiHttpRequest {
    std::string url;
    std::string apiKey;
    std::string body;
    long connectTimeoutSec{};
    long requestTimeoutSec{};
};

struct GeminiHttpResponse {
    bool completed{false};
    long status{};
    std::string body;
    std::string error;
};

// Plate BestShot JPEG를 Gemini generateContent REST API로 보내 번호판을 읽는다.
class GeminiOcrClient {
public:
    using HttpTransport =
        std::function<GeminiHttpResponse(const GeminiHttpRequest&)>;

    GeminiOcrClient(std::string api_key, std::string model,
                    long connect_timeout_sec, long request_timeout_sec,
                    std::string fallback_model = "",
                    HttpTransport transport = {});
    OcrResult recognizePlate(const std::string& image_path,
                             const std::string& enhanced_image_path = "") const;
    bool configured() const;

private:
    OcrResult recognizePlateWithModel(const std::string& model,
                             const std::string& image_path,
                             const std::string& enhanced_image_path) const;
    std::string api_key_;
    std::string model_;
    std::string fallback_model_;
    long connect_timeout_sec_;
    long request_timeout_sec_;
    HttpTransport transport_;
};

}

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

    // Gemini 응답의 usageMetadata. 응답에 없거나 본문 파싱 전에 실패하면 -1로 남는다.
    // 0은 "모델이 0토큰을 청구했다"는 뜻이라 미측정과 구분해야 한다.
    int prompt_token_count{-1};
    int candidates_token_count{-1};
    int total_token_count{-1};
    // 이 요청에 실제로 실린 이미지 수와 원본 바이트 합(base64 이전).
    // 토큰이 파일 크기가 아니라 해상도로 정해진다는 점을 로그에서 바로 대조하려는 값이다.
    int image_count{0};
    long long image_bytes{0};

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

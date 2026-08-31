#include "ocr/GeminiOcrClient.hpp"

#include "util/Logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::string base64Encode(const std::vector<unsigned char>& input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        unsigned int value = static_cast<unsigned int>(input[i]) << 16;
        if (i + 1 < input.size()) value |= static_cast<unsigned int>(input[i + 1]) << 8;
        if (i + 2 < input.size()) value |= static_cast<unsigned int>(input[i + 2]);
        output.push_back(table[(value >> 18) & 0x3F]);
        output.push_back(table[(value >> 12) & 0x3F]);
        output.push_back(i + 1 < input.size() ? table[(value >> 6) & 0x3F] : '=');
        output.push_back(i + 2 < input.size() ? table[value & 0x3F] : '=');
    }
    return output;
}

std::size_t writeResponse(char* data, std::size_t size, std::size_t count,
                          void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    output->append(data, size * count);
    return size * count;
}

bool appendImagePart(nlohmann::json& parts, const std::string& image_path,
                     ocr::OcrResult& result) {
    std::ifstream input(image_path, std::ios::binary);
    if (!input) {
        result.error = "Cannot open plate image: " + image_path;
        result.error_kind = ocr::OcrErrorKind::Image;
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 15U * 1024U * 1024U) {
        result.error = "Plate image is empty or too large: " + image_path;
        result.error_kind = ocr::OcrErrorKind::Image;
        return false;
    }
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xFF &&
                      bytes[1] == 0xD8 && bytes[2] == 0xFF;
    static constexpr unsigned char png_signature[] =
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const bool png = bytes.size() >= sizeof(png_signature) &&
        std::equal(std::begin(png_signature), std::end(png_signature),
                   bytes.begin());
    if (!jpeg && !png) {
        result.error = "Unsupported plate image encoding: " + image_path;
        result.error_kind = ocr::OcrErrorKind::Image;
        return false;
    }
    parts.push_back({{"inline_data", {{"mime_type", png ? "image/png" : "image/jpeg"},
                                       {"data", base64Encode(bytes)}}}});
    result.image_count += 1;
    result.image_bytes += static_cast<long long>(bytes.size());
    return true;
}

// 요청 1건이 실제로 청구한 토큰을 남긴다. 태그가 LOG_GEMINI_USAGE 로 매핑되므로
// 필요 없을 때 끌 수 있다. 토큰 값이 -1 이면 응답에 usageMetadata 가 없었다는 뜻이다.
void logUsage(const std::string& model, const ocr::OcrResult& result) {
    if (!util::logEnabled("GEMINI_USAGE")) return;
    util::logLine("GEMINI_USAGE",
        "model=" + model +
        " images=" + std::to_string(result.image_count) +
        " image_bytes=" + std::to_string(result.image_bytes) +
        " prompt=" + std::to_string(result.prompt_token_count) +
        " candidates=" + std::to_string(result.candidates_token_count) +
        " total=" + std::to_string(result.total_token_count) +
        " http=" + std::to_string(result.http_status) +
        " ok=" + (result.success ? "true" : "false"));
}

ocr::GeminiHttpResponse performCurlRequest(
    const ocr::GeminiHttpRequest& request) {
    ocr::GeminiHttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.error = "curl_easy_init failed";
        return response;
    }
    const std::string key_header = "x-goog-api-key: " + request.apiKey;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, key_header.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request.body.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.requestTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pi-server-gemini-ocr/1.0");

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        response.error = curl_easy_strerror(code);
        return response;
    }
    response.completed = true;
    return response;
}

}

namespace ocr {

const char* toString(const OcrErrorKind kind) noexcept {
    switch (kind) {
        case OcrErrorKind::None: return "NONE";
        case OcrErrorKind::Configuration: return "CONFIGURATION";
        case OcrErrorKind::Image: return "IMAGE";
        case OcrErrorKind::Transport: return "TRANSPORT";
        case OcrErrorKind::Authentication: return "AUTHENTICATION";
        case OcrErrorKind::RateLimited: return "RATE_LIMITED";
        case OcrErrorKind::Server: return "SERVER";
        case OcrErrorKind::Client: return "CLIENT";
        case OcrErrorKind::ResponseParse: return "RESPONSE_PARSE";
    }
    return "UNKNOWN";
}

GeminiOcrClient::GeminiOcrClient(std::string api_key, std::string model,
                                 long connect_timeout_sec,
                                 long request_timeout_sec,
                                 std::string fallback_model,
                                 HttpTransport transport)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      fallback_model_(std::move(fallback_model)),
      connect_timeout_sec_(connect_timeout_sec),
      request_timeout_sec_(request_timeout_sec),
      transport_(std::move(transport)) {
}

bool GeminiOcrClient::configured() const {
    return !api_key_.empty() && !model_.empty();
}

OcrResult GeminiOcrClient::recognizePlate(
    const std::string& image_path,
    const std::string& enhanced_image_path) const {
    OcrResult result = recognizePlateWithModel(
        model_, image_path, enhanced_image_path);
    // 대체 모델은 모델 ID가 더 이상 제공되지 않는 404에만 사용한다.
    // 인증 실패나 429/5xx/timeout은 worker의 제한된 backoff 정책이 처리해
    // 한 attempt 안에서 요청 수가 예상보다 늘어나지 않게 한다.
    const bool fallback_allowed =
        result.error_kind == OcrErrorKind::Client &&
        result.http_status == 404;
    if (!result.success && fallback_allowed && !fallback_model_.empty() &&
        fallback_model_ != model_) {
        result = recognizePlateWithModel(
            fallback_model_, image_path, enhanced_image_path);
    }
    return result;
}

OcrResult GeminiOcrClient::recognizePlateWithModel(
    const std::string& model,
    const std::string& image_path,
    const std::string& enhanced_image_path) const {
    OcrResult result;
    if (!configured()) {
        result.error = "Gemini API is not configured";
        result.error_kind = OcrErrorKind::Configuration;
        return result;
    }

    nlohmann::json request;
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back({{"text",
        "대한민국 자동차 번호판 OCR 작업이다. 첫 이미지가 주 OCR 대상이며, "
        "두 번째 이미지가 있으면 같은 장면의 원본 또는 개선본이다. "
        "이미지가 두 장이면 함께 비교해 "
        "번호판 문자만 판독하라. 로/토/도/아, 고/호, 1/7, 3/8처럼 "
        "획이 비슷한 문자를 특히 주의하고 추측하지 마라. 읽을 수 없으면 "
        "readable=false로 반환하고 plate_number는 공백과 하이픈 없이 반환하라."}});
    if (!appendImagePart(parts, image_path, result)) return result;
    if (!enhanced_image_path.empty() &&
        !appendImagePart(parts, enhanced_image_path, result))
        return result;
    request["contents"] = nlohmann::json::array({{
        {"role", "user"},
        {"parts", std::move(parts)}
    }});
    request["generationConfig"] = {
        {"temperature", 0},
        {"responseMimeType", "application/json"},
        {"responseSchema", {
            {"type", "OBJECT"},
            {"properties", {
                {"readable", {{"type", "BOOLEAN"}}},
                {"plate_number", {{"type", "STRING"}}},
                {"confidence", {{"type", "NUMBER"}}}
            }},
            {"required", nlohmann::json::array({"readable", "plate_number", "confidence"})}
        }}
    };

    const std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" +
                            model + ":generateContent";
    const std::string body = request.dump();
    const GeminiHttpRequest http_request{
        url, api_key_, body, connect_timeout_sec_, request_timeout_sec_};
    const GeminiHttpResponse response = transport_
        ? transport_(http_request) : performCurlRequest(http_request);
    result.http_status = response.status;
    if (!response.completed) {
        result.error = response.error.empty()
            ? "Gemini transport failed" : response.error;
        result.error_kind = OcrErrorKind::Transport;
        return result;
    }
    if (response.status < 200 || response.status >= 300) {
        result.error = "Gemini HTTP status " +
                       std::to_string(response.status);
        if (response.status == 401 || response.status == 403) {
            result.error_kind = OcrErrorKind::Authentication;
        } else if (response.status == 429) {
            result.error_kind = OcrErrorKind::RateLimited;
        } else if (response.status >= 500) {
            result.error_kind = OcrErrorKind::Server;
        } else {
            result.error_kind = OcrErrorKind::Client;
        }
        return result;
    }

    try {
        nlohmann::json envelope = nlohmann::json::parse(response.body);
        // 토큰은 응답이 도착한 시점에 이미 청구됐다. 그러니 candidates 파싱보다
        // 먼저 읽어서, 응답 형식이 바뀌어 아래에서 실패하더라도 사용량은 남긴다.
        const auto usage = envelope.find("usageMetadata");
        if (usage != envelope.end() && usage->is_object()) {
            result.prompt_token_count = usage->value("promptTokenCount", -1);
            result.candidates_token_count =
                usage->value("candidatesTokenCount", -1);
            result.total_token_count = usage->value("totalTokenCount", -1);
        }
        std::string text = envelope.at("candidates").at(0).at("content")
                               .at("parts").at(0).at("text").get<std::string>();
        result.raw_text = text;
        nlohmann::json value = nlohmann::json::parse(text);
        result.readable = value.value("readable", false);
        result.plate_number = value.value("plate_number", "");
        result.confidence = std::clamp(value.value("confidence", 0.0), 0.0, 1.0);
        result.success = true;
        result.error_kind = OcrErrorKind::None;
    } catch (const std::exception& error) {
        result.error = std::string("Gemini response parse failed: ") + error.what();
        result.error_kind = OcrErrorKind::ResponseParse;
    }
    // 2xx 응답 1건당 정확히 한 줄. fallback 모델로 재시도하면 각 호출이 따로 남아
    // 실제 청구 건수와 로그 줄 수가 일치한다.
    logUsage(model, result);
    return result;
}

}

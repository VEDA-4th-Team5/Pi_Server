#include "ocr/GeminiOcrClient.hpp"

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
                     std::string& error) {
    std::ifstream input(image_path, std::ios::binary);
    if (!input) {
        error = "Cannot open plate image: " + image_path;
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 15U * 1024U * 1024U) {
        error = "Plate image is empty or too large: " + image_path;
        return false;
    }
    const bool png = image_path.size() >= 4 &&
                     image_path.substr(image_path.size() - 4) == ".png";
    parts.push_back({{"inline_data", {{"mime_type", png ? "image/png" : "image/jpeg"},
                                       {"data", base64Encode(bytes)}}}});
    return true;
}

}

namespace ocr {

GeminiOcrClient::GeminiOcrClient(std::string api_key, std::string model,
                                 long connect_timeout_sec,
                                 long request_timeout_sec,
                                 std::string fallback_model)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      fallback_model_(std::move(fallback_model)),
      connect_timeout_sec_(connect_timeout_sec),
      request_timeout_sec_(request_timeout_sec) {
}

bool GeminiOcrClient::configured() const {
    return !api_key_.empty() && !model_.empty();
}

OcrResult GeminiOcrClient::recognizePlate(
    const std::string& image_path,
    const std::string& enhanced_image_path) const {
    OcrResult result = recognizePlateWithModel(
        model_, image_path, enhanced_image_path);
    if (!result.success && !fallback_model_.empty() &&
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
        return result;
    }

    nlohmann::json request;
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back({{"text",
        "대한민국 자동차 번호판 OCR 작업이다. 첫 이미지는 카메라 원본이고, "
        "두 번째 이미지가 있으면 같은 원본을 OpenCV로 확대·대비·선명화한 것이다. "
        "두 이미지를 함께 비교해 "
        "번호판 문자만 판독하라. 로/토/도/아, 고/호, 1/7, 3/8처럼 "
        "획이 비슷한 문자를 특히 주의하고 추측하지 마라. 읽을 수 없으면 "
        "readable=false로 반환하고 plate_number는 공백과 하이픈 없이 반환하라."}});
    if (!appendImagePart(parts, image_path, result.error)) return result;
    if (!enhanced_image_path.empty() &&
        !appendImagePart(parts, enhanced_image_path, result.error))
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

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = "curl_easy_init failed";
        return result;
    }
    const std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" +
                            model + ":generateContent";
    const std::string key_header = "x-goog-api-key: " + api_key_;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, key_header.c_str());
    std::string response;
    const std::string body = request.dump();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_sec_);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout_sec_);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pi-server-gemini-ocr/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
        return result;
    }
    if (http_status < 200 || http_status >= 300) {
        result.error = "Gemini HTTP status " + std::to_string(http_status);
        return result;
    }

    try {
        nlohmann::json envelope = nlohmann::json::parse(response);
        std::string text = envelope.at("candidates").at(0).at("content")
                               .at("parts").at(0).at("text").get<std::string>();
        nlohmann::json value = nlohmann::json::parse(text);
        result.readable = value.value("readable", false);
        result.plate_number = value.value("plate_number", "");
        result.confidence = std::clamp(value.value("confidence", 0.0), 0.0, 1.0);
        result.success = true;
    } catch (const std::exception& error) {
        result.error = std::string("Gemini response parse failed: ") + error.what();
    }
    return result;
}

}

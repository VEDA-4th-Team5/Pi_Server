#include "camera/CameraSnapshotApiClient.hpp"

#include "util/Logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace camera {
namespace {

std::once_flag curlInitOnce;
CURLcode curlInitResult = CURLE_FAILED_INIT;

std::size_t appendBody(char* data, const std::size_t size,
                       const std::size_t count, void* userdata) {
    const std::size_t bytes = size * count;
    auto* output = static_cast<std::vector<unsigned char>*>(userdata);
    const auto* first = reinterpret_cast<unsigned char*>(data);
    output->insert(output->end(), first, first + bytes);
    return bytes;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    if (base.empty()) return {};
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        return path;
    if (base.back() == '/' && !path.empty() && path.front() == '/')
        return base.substr(0, base.size() - 1) + path;
    if (base.back() != '/' && (path.empty() || path.front() != '/'))
        return base + '/' + path;
    return base + path;
}

std::string bodyText(const std::vector<unsigned char>& body) {
    return {body.begin(), body.end()};
}

bool isJpeg(const std::vector<unsigned char>& bytes) {
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
           bytes[2] == 0xFF;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

}  // namespace

CameraSnapshotApiClient::CameraSnapshotApiClient(
    CameraSnapshotApiConfig config)
    : config_(std::move(config)) {
    while (config_.openApiBase.size() > 1 && config_.openApiBase.back() == '/')
        config_.openApiBase.pop_back();
    while (config_.imageBase.size() > 1 && config_.imageBase.back() == '/')
        config_.imageBase.pop_back();
}

bool CameraSnapshotApiClient::configured() const noexcept {
    return !config_.openApiBase.empty() && !config_.imageBase.empty() &&
           config_.imageServerPort >= 1024 && config_.imageServerPort <= 65535;
}

const std::string& CameraSnapshotApiClient::lastError() const noexcept {
    return lastError_;
}

const std::vector<int>& CameraSnapshotApiClient::channels() const noexcept {
    return channels_;
}

const std::vector<std::string>& CameraSnapshotApiClient::filters() const noexcept {
    return filters_;
}

void CameraSnapshotApiClient::setError(std::string message) {
    lastError_ = std::move(message);
    util::logError("camera snapshot API: " + lastError_);
}

bool CameraSnapshotApiClient::initialize() {
    std::lock_guard lock(requestMutex_);
    return initializeUnlocked();
}

bool CameraSnapshotApiClient::initializeUnlocked() {
    initialized_ = false;
    channels_.clear();
    filters_.clear();
    lastError_.clear();
    if (!configured()) {
        setError("OPEN_API_BASE, IMAGE_BASE or image server port is invalid");
        return false;
    }
    std::call_once(curlInitOnce, [] {
        curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (curlInitResult != CURLE_OK) {
        setError("libcurl global initialization failed");
        return false;
    }
    if (!startImageServer() || !discoverChannels() || !discoverFilters())
        return false;
    initialized_ = true;
    util::logInfo("camera snapshot API ready: channels=" +
                  std::to_string(channels_.size()) + " filters=" +
                  std::to_string(filters_.size()));
    return true;
}

bool CameraSnapshotApiClient::startImageServer() {
    const nlohmann::json body{{"port", config_.imageServerPort}};
    const auto response = request("POST", joinUrl(config_.openApiBase,
                                                   "/startserver"),
                                  body.dump(), config_.requestTimeoutMs);
    if (!response.transportError.empty()) {
        setError("startserver transport error: " + response.transportError);
        return false;
    }
    if (response.status == 200 || response.status == 202) return true;
    setError("startserver HTTP " + std::to_string(response.status) +
             ": " + bodyText(response.body));
    return false;
}

bool CameraSnapshotApiClient::discoverChannels() {
    const auto response = request("GET", joinUrl(config_.openApiBase,
                                                  "/channels"), {},
                                  config_.requestTimeoutMs);
    if (!response.transportError.empty()) {
        setError("channels transport error: " + response.transportError);
        return false;
    }
    if (response.status != 200) {
        setError("channels HTTP " + std::to_string(response.status));
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(bodyText(response.body));
        if (!json.value("success", false) || !json.contains("channels") ||
            !json.at("channels").is_array()) {
            throw std::runtime_error("invalid channels response");
        }
        for (const auto& entry : json.at("channels"))
            channels_.push_back(entry.at("id").get<int>());
        if (channels_.empty()) throw std::runtime_error("empty channels list");
    } catch (const std::exception& error) {
        setError(std::string("channels parse failed: ") + error.what());
        return false;
    }
    return true;
}

bool CameraSnapshotApiClient::discoverFilters() {
    const auto response = request("GET", joinUrl(config_.openApiBase,
                                                  "/filters"), {},
                                  config_.requestTimeoutMs);
    if (!response.transportError.empty()) {
        setError("filters transport error: " + response.transportError);
        return false;
    }
    if (response.status != 200) {
        setError("filters HTTP " + std::to_string(response.status));
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(bodyText(response.body));
        if (!json.value("success", false) || !json.contains("filters") ||
            !json.at("filters").is_array()) {
            throw std::runtime_error("invalid filters response");
        }
        for (const auto& entry : json.at("filters"))
            filters_.push_back(entry.at("id").get<std::string>());
    } catch (const std::exception& error) {
        setError(std::string("filters parse failed: ") + error.what());
        return false;
    }
    return true;
}

bool CameraSnapshotApiClient::generate(const int channel,
                                       CameraGeneratedImages& images) {
    std::lock_guard lock(requestMutex_);
    images = {};
    lastError_.clear();
    if (!initialized_ && !initializeUnlocked()) return false;
    if (std::find(channels_.begin(), channels_.end(), channel) ==
        channels_.end()) {
        setError("requested channel is not advertised: " +
                 std::to_string(channel));
        return false;
    }

    const nlohmann::json requestBody{
        {"channel", channel},
        {"outputs", nlohmann::json::array({
            {{"id", "original"}, {"type", "original"}},
            {{"id", "enhanced"}, {"type", "auto"}}})}};

    nlohmann::json responseJson;
    for (int attempt = 0; attempt <= config_.maxRetries; ++attempt) {
        const auto response = request(
            "POST", joinUrl(config_.openApiBase, "/images/generate"),
            requestBody.dump(), config_.requestTimeoutMs);
        if (!response.transportError.empty()) {
            if (attempt < config_.maxRetries) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retryDelayMs));
                continue;
            }
            setError("generate transport error: " + response.transportError);
            return false;
        }
        try {
            responseJson = nlohmann::json::parse(bodyText(response.body));
        } catch (const std::exception& error) {
            setError(std::string("generate response parse failed: ") +
                     error.what());
            return false;
        }
        const std::string errorCode = responseJson.value("error_code", "");
        const bool imageServerStopped = response.status == 409 &&
            errorCode == "IMAGE_SERVER_NOT_STARTED";
        const bool retryable = response.status == 503 ||
            (response.status == 502 && errorCode == "SNAPSHOT_FAILED") ||
            imageServerStopped;
        if (response.status == 200 && responseJson.value("success", false))
            break;
        if (retryable && attempt < config_.maxRetries) {
            if (imageServerStopped) {
                if (!startImageServer()) return false;
                util::logWarn(
                    "camera snapshot API image server stopped; restart requested");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retryDelayMs));
            continue;
        }
        setError("generate HTTP " + std::to_string(response.status) +
                 " error_code=" + errorCode + " message=" +
                 responseJson.value("message", ""));
        return false;
    }

    try {
        images.runId = responseJson.at("run_id").get<std::string>();
        images.channel = responseJson.at("channel").get<int>();
        images.detectedEnvironment =
            responseJson.value("detected_environment", "unknown");
        images.autoFilter = responseJson.value("auto_filter", "none");
        if (images.runId.empty() || !responseJson.contains("results") ||
            !responseJson.at("results").is_array()) {
            throw std::runtime_error("run_id or results missing");
        }

        bool foundOriginal = false;
        bool foundEnhanced = false;
        for (const auto& result : responseJson.at("results")) {
            const std::string id = result.at("id").get<std::string>();
            const std::string path = result.at("image_path").get<std::string>();
            const std::size_t bytes = result.value("jpeg_bytes", 0U);
            if (id == "original") {
                foundOriginal = downloadJpeg(path, bytes, images.originalJpeg);
            } else if (id == "enhanced") {
                foundEnhanced = downloadJpeg(path, bytes, images.enhancedJpeg);
            }
        }
        if (!foundOriginal || !foundEnhanced)
            throw std::runtime_error(lastError_.empty()
                ? "original/enhanced result missing" : lastError_);
    } catch (const std::exception& error) {
        setError(std::string("generate result failed: ") + error.what());
        return false;
    }
    return true;
}

bool CameraSnapshotApiClient::downloadJpeg(
    const std::string& path, const std::size_t expectedBytes,
    std::vector<unsigned char>& output) {
    for (int attempt = 0; attempt <= config_.maxRetries; ++attempt) {
        const auto response = request("GET", joinUrl(config_.imageBase, path),
                                      {}, config_.jpegTimeoutMs);
        if (response.transportError.empty() && response.status == 200) {
            const std::string type = lower(response.contentType);
            if (type.find("image/jpeg") == std::string::npos) {
                setError("JPEG content type mismatch: " + response.contentType);
                return false;
            }
            if (!isJpeg(response.body)) {
                setError("JPEG magic bytes are invalid");
                return false;
            }
            if (expectedBytes != 0 && response.body.size() != expectedBytes) {
                setError("JPEG byte length mismatch: expected=" +
                         std::to_string(expectedBytes) + " actual=" +
                         std::to_string(response.body.size()));
                return false;
            }
            output = response.body;
            return true;
        }
        if (response.status == 404) {
            setError("camera image run/result expired (HTTP 404)");
            return false;
        }
        if (attempt < config_.maxRetries) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retryDelayMs));
            continue;
        }
        setError(response.transportError.empty()
            ? "JPEG GET HTTP " + std::to_string(response.status)
            : "JPEG GET transport error: " + response.transportError);
        return false;
    }
    return false;
}

CameraSnapshotApiClient::HttpResponse CameraSnapshotApiClient::request(
    const std::string& method, const std::string& url,
    const std::string& jsonBody, const int timeoutMs) const {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.transportError = "curl_easy_init failed";
        return response;
    }
    struct curl_slist* headers = nullptr;
    if (method == "POST")
        headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config_.connectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "pi-server-camera-snapshot-api/1.0");
    if (!config_.username.empty()) {
        const std::string credentials = config_.username + ':' + config_.password;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(curl, CURLOPT_USERPWD, credentials.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(jsonBody.size()));
    }
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* contentType = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType) response.contentType = contentType;
    if (code != CURLE_OK) response.transportError = curl_easy_strerror(code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

}  // namespace camera

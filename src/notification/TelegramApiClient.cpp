#include "notification/TelegramApiClient.hpp"

#include "util/Logger.hpp"
#include "util/UrlMasker.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace {

size_t appendPayload(void* content, std::size_t size, std::size_t nmemb, void* output) {
    auto* string_payload = static_cast<std::string*>(output);
    const std::size_t real_size = size * nmemb;
    string_payload->append(static_cast<const char*>(content), real_size);
    return real_size;
}

}

namespace notification {

TelegramApiClient::TelegramApiClient(const Config& config) : config_(config) {
    if (!config_.bot_token.empty()) {
        base_url_ = "https://api.telegram.org/bot" + config_.bot_token;
    }
}

bool TelegramApiClient::sendMessage(const std::string& text) {
    if (!config_.enabled || config_.bot_token.empty() || config_.channel.empty()) {
        return false;
    }

    const std::string url = base_url_ + "/sendMessage";
    const nlohmann::json body{
        {"chat_id", config_.channel},
        {"text", text}
    };
    const std::string payload = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        util::logWarn("Telegram API: curl handle initialization failed");
        return false;
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.size());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config_.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config_.request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendPayload);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const auto result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        util::logWarn(std::string("Telegram API send failed: curl ") +
                      curl_easy_strerror(result) + " url=" +
                      util::hideUrlForLog(url));
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        util::logWarn("Telegram API send failed: http_code=" +
                      std::to_string(http_code) + " url=" +
                      util::hideUrlForLog(url));
        return false;
    }

    try {
        const auto document = nlohmann::json::parse(response);
        return document.value("ok", false);
    } catch (const std::exception& error) {
        util::logWarn(std::string("Telegram API response parsing failed: ") +
                      error.what() + " payload=" + std::to_string(response.size()) +
                      " bytes");
        return false;
    }
}

}  // namespace notification

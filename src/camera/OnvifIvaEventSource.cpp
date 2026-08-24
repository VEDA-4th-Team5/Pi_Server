#include "camera/OnvifIvaEventSource.hpp"

#include "util/Logger.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace camera {
namespace {

constexpr std::string_view kCreateAction =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/"
    "CreatePullPointSubscriptionRequest";
constexpr std::string_view kPullAction =
    "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
    "PullMessagesRequest";
constexpr std::string_view kRenewAction =
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";
constexpr std::string_view kUnsubscribeAction =
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
    "UnsubscribeRequest";

struct HttpResponse {
    CURLcode curlCode{CURLE_OK};
    long statusCode{};
    std::string body;
    std::string error;

    [[nodiscard]] bool ok() const {
        return curlCode == CURLE_OK && statusCode >= 200 && statusCode < 300;
    }
};

std::once_flag curlInitOnce;
CURLcode curlInitResult{CURLE_FAILED_INIT};

bool ensureCurlInitialized() {
    std::call_once(curlInitOnce, [] {
        curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    return curlInitResult == CURLE_OK;
}

std::string xmlEscape(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string xmlUnescape(std::string value) {
    const std::array<std::pair<std::string_view, std::string_view>, 5> entities{{
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"},
    }};
    for (const auto& [encoded, decoded] : entities) {
        std::size_t position{};
        while ((position = value.find(encoded, position)) != std::string::npos) {
            value.replace(position, encoded.size(), decoded);
            position += decoded.size();
        }
    }
    return value;
}

std::string utcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (gmtime_r(&now, &utc) == nullptr)
        throw std::runtime_error("could not obtain UTC time");
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << ".000Z";
    return output.str();
}

std::string base64(const unsigned char* data, const std::size_t size) {
    std::string encoded(4U * ((size + 2U) / 3U), '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), data,
        static_cast<int>(size));
    if (length < 0) throw std::runtime_error("base64 encoding failed");
    encoded.resize(static_cast<std::size_t>(length));
    return encoded;
}

std::array<unsigned char, 20> sha1(
    const std::array<unsigned char, 16>& nonce,
    const std::string_view created,
    const std::string_view password) {
    using DigestContext =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), nonce.data(), nonce.size()) != 1 ||
        EVP_DigestUpdate(context.get(), created.data(), created.size()) != 1 ||
        EVP_DigestUpdate(context.get(), password.data(), password.size()) != 1) {
        throw std::runtime_error("WS-Security SHA-1 initialization failed");
    }
    std::array<unsigned char, 20> digest{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 ||
        length != digest.size()) {
        throw std::runtime_error("WS-Security SHA-1 calculation failed");
    }
    return digest;
}

std::string uuidUrn() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("secure UUID generation failed");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << "urn:uuid:" << std::hex << std::setfill('0');
    for (std::size_t index{}; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) output << '-';
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::string makeSoapEnvelope(const std::string& endpoint,
                             const std::string_view action,
                             const std::string_view body,
                             const OnvifIvaEventSource::Config& config) {
    std::array<unsigned char, 16> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
        throw std::runtime_error("WS-Security nonce generation failed");
    const std::string created = utcNow();
    const auto digest = sha1(nonce, created, config.password);

    std::ostringstream soap;
    soap
        << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<s:Envelope"
        << " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        << " xmlns:a=\"http://www.w3.org/2005/08/addressing\""
        << " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
        << " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
        << " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        << " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        << "<s:Header>"
        << "<a:Action s:mustUnderstand=\"1\">" << xmlEscape(action)
        << "</a:Action><a:MessageID>" << uuidUrn() << "</a:MessageID>"
        << "<a:ReplyTo><a:Address>http://www.w3.org/2005/08/addressing/"
           "anonymous</a:Address></a:ReplyTo>"
        << "<a:To s:mustUnderstand=\"1\">" << xmlEscape(endpoint)
        << "</a:To><wsse:Security s:mustUnderstand=\"1\">"
        << "<wsse:UsernameToken><wsse:Username>"
        << xmlEscape(config.username) << "</wsse:Username>"
        << "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        << base64(digest.data(), digest.size()) << "</wsse:Password>"
        << "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/"
           "01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
        << base64(nonce.data(), nonce.size()) << "</wsse:Nonce>"
        << "<wsu:Created>" << created << "</wsu:Created>"
        << "</wsse:UsernameToken></wsse:Security></s:Header>"
        << "<s:Body>" << body << "</s:Body></s:Envelope>";
    return soap.str();
}

std::size_t writeBody(char* data, const std::size_t size,
                      const std::size_t count, void* userData) {
    const std::size_t bytes = size * count;
    static_cast<std::string*>(userData)->append(data, bytes);
    return bytes;
}

int transferProgress(void* userData, curl_off_t, curl_off_t,
                     curl_off_t, curl_off_t) {
    const auto* stopped = static_cast<const std::atomic_bool*>(userData);
    return stopped != nullptr && stopped->load() ? 1 : 0;
}

HttpResponse postSoap(const std::string& endpoint,
                      const std::string_view action,
                      const std::string_view body,
                      const OnvifIvaEventSource::Config& config,
                      const long timeoutSeconds,
                      const std::atomic_bool* stopRequested) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.curlCode = CURLE_FAILED_INIT;
        response.error = "curl initialization failed";
        return response;
    }
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
        "Content-Type: application/soap+xml; charset=utf-8");
    headers = curl_slist_append(headers, "Expect:");

    try {
        const std::string envelope =
            makeSoapEnvelope(endpoint, action, body, config);
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, envelope.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(envelope.size()));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "veda-onvif-iva-source/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        if (stopRequested != nullptr) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stopRequested);
        }
        response.curlCode = curl_easy_perform(curl);
        if (response.curlCode != CURLE_OK)
            response.error = curl_easy_strerror(response.curlCode);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    } catch (const std::exception& error) {
        response.curlCode = CURLE_FAILED_INIT;
        response.error = error.what();
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

bool hasSoapFault(const std::string& xml) {
    static const std::regex pattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Fault\b)", std::regex::icase);
    return std::regex_search(xml, pattern);
}

std::string soapFaultReason(const std::string& xml) {
    static const std::regex pattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Text\b[^>]*>([^<]*)</)",
        std::regex::icase);
    std::smatch match;
    return std::regex_search(xml, match, pattern)
        ? xmlUnescape(match[1].str()) : "SOAP fault";
}

bool validSoapResponse(const HttpResponse& response,
                       const std::string_view operation,
                       const bool stopping) {
    if (stopping && response.curlCode == CURLE_ABORTED_BY_CALLBACK) return false;
    if (!response.ok()) {
        std::string message(operation);
        message += " failed: ";
        message += !response.error.empty()
            ? response.error : "HTTP " + std::to_string(response.statusCode);
        if (hasSoapFault(response.body))
            message += " (" + soapFaultReason(response.body) + ')';
        util::logWarn("ONVIF IVA " + message);
        return false;
    }
    if (hasSoapFault(response.body)) {
        util::logWarn("ONVIF IVA " + std::string(operation) +
                      " SOAP fault: " + soapFaultReason(response.body));
        return false;
    }
    return true;
}

std::optional<std::string> subscriptionAddress(const std::string& xml) {
    static const std::regex pattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Address\b[^>]*>([^<]+)</)",
        std::regex::icase);
    for (std::sregex_iterator iterator(xml.begin(), xml.end(), pattern), end;
         iterator != end; ++iterator) {
        std::string address = xmlUnescape((*iterator)[1].str());
        if (address.find("session=") != std::string::npos) return address;
    }
    return std::nullopt;
}

std::optional<std::string> attribute(const std::string& tag,
                                     const std::string& name) {
    const std::regex pattern(
        "(?:^|[[:space:]])" + name +
            R"([[:space:]]*=[[:space:]]*["']([^"']*)["'])",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(tag, match, pattern)) return std::nullopt;
    return xmlUnescape(match[1].str());
}

std::optional<std::string> createSubscription(
    const OnvifIvaEventSource::Config& config,
    const std::atomic_bool& stopRequested) {
    const auto response = postSoap(
        config.endpoint, kCreateAction,
        "<tev:CreatePullPointSubscription>"
        "<wsnt:InitialTerminationTime>PT2M</wsnt:InitialTerminationTime>"
        "</tev:CreatePullPointSubscription>",
        config, 10L, &stopRequested);
    if (!validSoapResponse(response, "subscription", stopRequested.load()))
        return std::nullopt;
    const auto address = subscriptionAddress(response.body);
    if (!address)
        util::logWarn("ONVIF IVA subscription response has no session address");
    return address;
}

bool renewSubscription(const std::string& address,
                       const OnvifIvaEventSource::Config& config,
                       const std::atomic_bool& stopRequested) {
    const auto response = postSoap(
        address, kRenewAction,
        "<wsnt:Renew><wsnt:TerminationTime>PT2M</wsnt:TerminationTime>"
        "</wsnt:Renew>",
        config, 10L, &stopRequested);
    return validSoapResponse(response, "renew", stopRequested.load());
}

void unsubscribe(const std::string& address,
                 const OnvifIvaEventSource::Config& config) {
    const auto response = postSoap(address, kUnsubscribeAction,
                                   "<wsnt:Unsubscribe/>", config, 5L, nullptr);
    if (!response.ok() && response.curlCode != CURLE_ABORTED_BY_CALLBACK)
        util::logWarn("ONVIF IVA unsubscribe failed");
}

}  // namespace

OnvifIvaEventSource::OnvifIvaEventSource(Config config, EventHandler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

OnvifIvaEventSource::~OnvifIvaEventSource() { stop(); }

bool OnvifIvaEventSource::start() {
    std::lock_guard lock(lifecycleMutex_);
    if (started_) return true;
    if (config_.endpoint.empty() || config_.username.empty() ||
        config_.password.empty() || !handler_) {
        util::logError(
            "ONVIF IVA source requires endpoint, camera credentials and handler");
        return false;
    }
    if (!ensureCurlInitialized()) {
        util::logError("ONVIF IVA source curl global initialization failed");
        return false;
    }
    stopRequested_.store(false);
    try {
        worker_ = std::thread(&OnvifIvaEventSource::run, this);
    } catch (const std::exception& error) {
        util::logError("ONVIF IVA source thread start failed: " +
                       std::string(error.what()));
        return false;
    }
    started_ = true;
    return true;
}

void OnvifIvaEventSource::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(lifecycleMutex_);
        if (!started_) return;
        stopRequested_.store(true);
        worker = std::move(worker_);
        started_ = false;
    }
    if (worker.joinable()) worker.join();
}

bool OnvifIvaEventSource::started() const noexcept {
    std::lock_guard lock(lifecycleMutex_);
    return started_;
}

std::string OnvifIvaEventSource::deriveEventEndpoint(
    const std::string& cameraBaseUrl) {
    if (cameraBaseUrl.find("/onvif/event_service") != std::string::npos)
        return cameraBaseUrl;
    const std::size_t scheme = cameraBaseUrl.find("://");
    if (scheme == std::string::npos)
        throw std::invalid_argument("camera base URL has no scheme");
    const std::size_t path = cameraBaseUrl.find('/', scheme + 3U);
    const std::string origin = path == std::string::npos
        ? cameraBaseUrl : cameraBaseUrl.substr(0, path);
    return origin + "/onvif/event_service";
}

std::vector<OnvifIvaEvent> OnvifIvaEventSource::parseEvents(
    const std::string& xml) {
    static const std::regex notificationPattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?NotificationMessage\b[\s\S]*?</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?NotificationMessage[[:space:]]*>)",
        std::regex::icase);
    static const std::regex messagePattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?Message\b[^>]*>)",
        std::regex::icase);
    static const std::regex itemPattern(
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?SimpleItem\b[^>]*>)",
        std::regex::icase);

    std::vector<OnvifIvaEvent> events;
    for (std::sregex_iterator notification(xml.begin(), xml.end(),
                                           notificationPattern), end;
         notification != end; ++notification) {
        const std::string block = notification->str();
        if (block.find("IvaArea") == std::string::npos) continue;
        OnvifIvaEvent event;
        for (std::sregex_iterator message(block.begin(), block.end(),
                                          messagePattern);
             message != end; ++message) {
            const std::string tag = message->str();
            const auto utcTime = attribute(tag, "UtcTime");
            const auto operation = attribute(tag, "PropertyOperation");
            if (!utcTime && !operation) continue;
            event.utcTime = utcTime.value_or("");
            event.operation = operation.value_or("");
            break;
        }
        for (std::sregex_iterator item(block.begin(), block.end(), itemPattern);
             item != end; ++item) {
            const std::string tag = item->str();
            const auto name = attribute(tag, "Name");
            const auto value = attribute(tag, "Value");
            if (!name || !value) continue;
            if (*name == "VideoSourceToken") event.videoSourceToken = *value;
            else if (*name == "RuleName") event.ruleName = *value;
            else if (*name == "State") event.state = *value;
            else if (*name == "ObjectId") event.objectId = *value;
            else if (*name == "Action") event.action = *value;
        }
        if (!event.videoSourceToken.empty()) events.push_back(std::move(event));
    }
    return events;
}

void OnvifIvaEventSource::run() noexcept {
    while (!stopRequested_.load()) {
        try {
            const auto address = createSubscription(config_, stopRequested_);
            if (!address) {
                if (!stopRequested_.load())
                    std::this_thread::sleep_for(config_.reconnectDelay);
                continue;
            }
            util::logLine("ONVIF_IVA", "subscription connected");
            auto renewAt = std::chrono::steady_clock::now() +
                           config_.renewInterval;
            bool reconnect{};
            while (!stopRequested_.load() && !reconnect) {
                if (std::chrono::steady_clock::now() >= renewAt) {
                    if (!renewSubscription(*address, config_, stopRequested_)) {
                        reconnect = !stopRequested_.load();
                        break;
                    }
                    renewAt = std::chrono::steady_clock::now() +
                              config_.renewInterval;
                }
                const long timeout = std::max<long>(
                    2L, config_.pullTimeout.count() + 5L);
                const auto response = postSoap(
                    *address, kPullAction,
                    "<tev:PullMessages><tev:Timeout>PT" +
                        std::to_string(config_.pullTimeout.count()) +
                        "S</tev:Timeout><tev:MessageLimit>100</tev:MessageLimit>"
                        "</tev:PullMessages>",
                    config_, timeout, &stopRequested_);
                if (stopRequested_.load()) break;
                if (!validSoapResponse(response, "pull", false)) {
                    reconnect = true;
                    break;
                }
                for (const auto& event : parseEvents(response.body)) {
                    if (!config_.acceptedVideoSourceTokens.empty() &&
                        !config_.acceptedVideoSourceTokens.contains(
                            event.videoSourceToken)) {
                        continue;
                    }
                    if (!config_.deliverInitialized && event.action.empty() &&
                        event.objectId.empty()) {
                        continue;
                    }
                    try {
                        if (!handler_(event)) {
                            util::logWarn(
                                "ONVIF IVA event rejected by server: token=" +
                                event.videoSourceToken + " rule=" +
                                event.ruleName + " action=" + event.action);
                        }
                    } catch (const std::exception& error) {
                        util::logError("ONVIF IVA handler exception: " +
                                       std::string(error.what()));
                    } catch (...) {
                        util::logError("ONVIF IVA handler unknown exception");
                    }
                }
            }
            unsubscribe(*address, config_);
            if (reconnect && !stopRequested_.load()) {
                util::logWarn("ONVIF IVA reconnecting after transport failure");
                std::this_thread::sleep_for(config_.reconnectDelay);
            }
        } catch (const std::exception& error) {
            util::logError("ONVIF IVA worker error: " +
                           std::string(error.what()));
            if (!stopRequested_.load())
                std::this_thread::sleep_for(config_.reconnectDelay);
        } catch (...) {
            util::logError("ONVIF IVA worker unknown error");
            if (!stopRequested_.load())
                std::this_thread::sleep_for(config_.reconnectDelay);
        }
    }
    util::logLine("ONVIF_IVA", "source stopped");
}

}  // namespace camera

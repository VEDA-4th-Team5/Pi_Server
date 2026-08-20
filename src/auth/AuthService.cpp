#include "auth/AuthService.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <limits>
#include <stdexcept>

namespace auth {
namespace {

constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kMaximumRateEntries = 4096;
constexpr std::size_t kMinimumPasswordLength = 4;

std::int64_t systemEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

}  // namespace

AuthService::AuthService(database::EventDatabase& database,
                         AuthConfig config, Clock clock)
    : database_(database), config_(config),
      clock_(clock ? std::move(clock) : Clock(systemEpochSeconds)) {
    if (config_.session_ttl_seconds < 28800 ||
        config_.session_ttl_seconds > 43200 ||
        config_.login_window_seconds <= 0 ||
        config_.login_max_failures <= 0 ||
        config_.login_cooldown_seconds <= 0) {
        throw std::invalid_argument("invalid app authentication configuration");
    }
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");

    // 존재하지 않는 계정도 Argon2id 검증 시간을 사용해 계정 존재 여부 노출을 줄인다.
    dummy_password_hash_ = hashPassword("not-a-real-user-password");
}

std::string AuthService::trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin)))
        ++begin;
    while (end != begin &&
           std::isspace(static_cast<unsigned char>(*(end - 1))))
        --end;
    return {begin, end};
}

std::optional<std::string> AuthService::normalizeAccountId(
    std::string_view account_id) {
    std::string normalized = trim(account_id);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (normalized.size() < 3 || normalized.size() > 64)
        return std::nullopt;
    if (!std::isalnum(static_cast<unsigned char>(normalized.front())))
        return std::nullopt;
    for (const unsigned char character : normalized) {
        if (!(character >= 'a' && character <= 'z') &&
            !(character >= '0' && character <= '9') &&
            character != '.' && character != '_' && character != '-') {
            return std::nullopt;
        }
    }
    return normalized;
}

bool AuthService::validPassword(std::string_view password) noexcept {
    return password.size() >= kMinimumPasswordLength &&
           password.size() <= 1024;
}

std::string AuthService::hashPassword(std::string_view password) {
    std::array<char, crypto_pwhash_STRBYTES> output{};
    if (crypto_pwhash_str(output.data(), password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("password hashing failed");
    }
    return output.data();
}

bool AuthService::verifyPassword(
    std::string_view password,
    const std::string& password_hash) noexcept {
    return crypto_pwhash_str_verify(password_hash.c_str(), password.data(),
                                    password.size()) == 0;
}

std::string AuthService::generateToken() {
    std::array<unsigned char, kTokenBytes> raw{};
    randombytes_buf(raw.data(), raw.size());
    std::array<char, sodium_base64_ENCODED_LEN(
        kTokenBytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING)> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), raw.data(), raw.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_memzero(raw.data(), raw.size());
    return encoded.data();
}

std::optional<std::vector<unsigned char>> AuthService::tokenDigest(
    std::string_view bearer_token) noexcept {
    constexpr std::size_t encoded_length = sodium_base64_ENCODED_LEN(
        kTokenBytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING) - 1;
    if (bearer_token.size() != encoded_length) return std::nullopt;

    std::array<unsigned char, kTokenBytes> raw{};
    std::size_t decoded_length{};
    const char* end{};
    if (sodium_base642bin(raw.data(), raw.size(), bearer_token.data(),
                          bearer_token.size(), nullptr, &decoded_length, &end,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
        decoded_length != raw.size() ||
        end != bearer_token.data() + bearer_token.size()) {
        sodium_memzero(raw.data(), raw.size());
        return std::nullopt;
    }

    std::vector<unsigned char> digest(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(digest.data(), raw.data(), raw.size());
    sodium_memzero(raw.data(), raw.size());
    return digest;
}

std::optional<std::string_view> AuthService::extractBearer(
    std::string_view authorization_header) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    if (authorization_header.size() <= prefix.size() ||
        authorization_header.size() > 128 ||
        authorization_header.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    const auto token = authorization_header.substr(prefix.size());
    if (token.find_first_of(" \t\r\n") != std::string_view::npos)
        return std::nullopt;
    return token;
}

std::string AuthService::formatUtc(std::int64_t epoch_seconds) {
    const std::time_t value = static_cast<std::time_t>(epoch_seconds);
    std::tm utc{};
    if (gmtime_r(&value, &utc) == nullptr)
        throw std::runtime_error("UTC timestamp conversion failed");
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ",
                      &utc) == 0) {
        throw std::runtime_error("UTC timestamp formatting failed");
    }
    return buffer.data();
}

void AuthService::pruneFailures(RateState& state, std::int64_t now) const {
    const std::int64_t cutoff = now - config_.login_window_seconds;
    while (!state.failures.empty() && state.failures.front() <= cutoff)
        state.failures.pop_front();
    if (state.blocked_until <= now) state.blocked_until = 0;
}

void AuthService::boundRateTable(std::int64_t now) {
    for (auto iterator = rate_states_.begin(); iterator != rate_states_.end();) {
        pruneFailures(iterator->second, now);
        if (iterator->second.failures.empty() &&
            iterator->second.blocked_until == 0) {
            iterator = rate_states_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    while (rate_states_.size() >= kMaximumRateEntries) {
        auto oldest = std::min_element(rate_states_.begin(), rate_states_.end(),
            [](const auto& left, const auto& right) {
                return left.second.last_seen < right.second.last_seen;
            });
        if (oldest == rate_states_.end()) break;
        rate_states_.erase(oldest);
    }
}

std::optional<int> AuthService::rateLimitRetryAfter(
    const std::string& key, std::int64_t now) {
    std::lock_guard lock(rate_mutex_);
    auto iterator = rate_states_.find(key);
    if (iterator == rate_states_.end()) return std::nullopt;
    pruneFailures(iterator->second, now);
    iterator->second.last_seen = now;
    if (iterator->second.blocked_until > now) {
        const auto remaining = iterator->second.blocked_until - now;
        return static_cast<int>(std::min<std::int64_t>(
            remaining, std::numeric_limits<int>::max()));
    }
    return std::nullopt;
}

void AuthService::recordFailure(const std::string& key, std::int64_t now) {
    std::lock_guard lock(rate_mutex_);
    if (rate_states_.find(key) == rate_states_.end()) boundRateTable(now);
    auto& state = rate_states_[key];
    pruneFailures(state, now);
    state.failures.push_back(now);
    state.last_seen = now;
    if (static_cast<int>(state.failures.size()) >= config_.login_max_failures)
        state.blocked_until = now + config_.login_cooldown_seconds;
}

void AuthService::clearFailures(const std::string& key) {
    std::lock_guard lock(rate_mutex_);
    rate_states_.erase(key);
}

LoginResult AuthService::login(const std::string& account_id,
                               const std::string& password,
                               const std::string& source_ip) {
    if (!database_.runtimeSchemaReady()) {
        LoginResult result;
        result.status = LoginStatus::Unavailable;
        return result;
    }
    const auto normalized = normalizeAccountId(account_id);
    if (!normalized || !validPassword(password)) {
        LoginResult result;
        result.status = LoginStatus::InvalidRequest;
        return result;
    }

    const std::int64_t now = clock_();
    const std::string rate_key =
        (source_ip.empty() ? std::string("unknown") : source_ip) + '\n' +
        *normalized;
    if (const auto retry_after = rateLimitRetryAfter(rate_key, now)) {
        LoginResult result;
        result.status = LoginStatus::RateLimited;
        result.retry_after_seconds = *retry_after;
        return result;
    }

    try {
        const auto user = database_.findAppUser(*normalized);
        const std::string& candidate_hash =
            user ? user->password_hash : dummy_password_hash_;
        const bool password_matches = verifyPassword(password, candidate_hash);
        if (!user || !user->enabled || !password_matches) {
            recordFailure(rate_key, now);
            LoginResult result;
            result.status = LoginStatus::InvalidCredentials;
            return result;
        }

        const std::int64_t expires_at = now + config_.session_ttl_seconds;
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::string token = generateToken();
            const auto digest = tokenDigest(token);
            if (digest && database_.createAppSession(
                    user->user_id, *digest, now, expires_at)) {
                clearFailures(rate_key);
                LoginResult result;
                result.status = LoginStatus::Success;
                result.access_token = std::move(token);
                result.expires_at = formatUtc(expires_at);
                result.user = {user->user_id, user->account_id,
                               user->display_name, expires_at};
                return result;
            }
        }
    } catch (...) {
        LoginResult result;
        result.status = LoginStatus::Unavailable;
        return result;
    }
    LoginResult result;
    result.status = LoginStatus::Unavailable;
    return result;
}

std::optional<AuthenticatedUser> AuthService::authenticateBearer(
    std::string_view authorization_header) const {
    const auto bearer = extractBearer(authorization_header);
    if (!bearer) return std::nullopt;
    const auto digest = tokenDigest(*bearer);
    if (!digest) return std::nullopt;
    try {
        const auto principal = database_.findActiveAppSession(*digest, clock_());
        if (!principal) return std::nullopt;
        return AuthenticatedUser{principal->user_id, principal->account_id,
                                 principal->display_name,
                                 principal->expires_at_utc};
    } catch (...) {
        return std::nullopt;
    }
}

bool AuthService::logoutBearer(std::string_view authorization_header) {
    const auto bearer = extractBearer(authorization_header);
    if (!bearer) return false;
    const auto digest = tokenDigest(*bearer);
    if (!digest) return false;
    try {
        return database_.revokeAppSession(*digest, clock_());
    } catch (...) {
        return false;
    }
}

bool AuthService::addUser(const std::string& account_id,
                          const std::string& password,
                          const std::string& display_name,
                          std::int64_t* user_id,
                          std::string* error) {
    const auto normalized = normalizeAccountId(account_id);
    if (!normalized) {
        setError(error, "account ID format is invalid");
        return false;
    }
    if (!validPassword(password)) {
        setError(error, "password must contain at least 4 characters");
        return false;
    }
    const std::string normalized_display = trim(display_name);
    if (normalized_display.size() > 128) {
        setError(error, "display name is too long");
        return false;
    }
    try {
        const std::string password_hash = hashPassword(password);
        if (!database_.createAppUser(*normalized, password_hash,
                                     normalized_display, clock_(), user_id)) {
            setError(error, "account already exists or database write failed");
            return false;
        }
        return true;
    } catch (...) {
        setError(error, "password hashing failed");
        return false;
    }
}

std::vector<database::AppUserRecord> AuthService::listUsers() const {
    return database_.listAppUsers();
}

bool AuthService::setUserEnabled(const std::string& account_id,
                                 bool enabled,
                                 std::string* error) {
    const auto normalized = normalizeAccountId(account_id);
    if (!normalized) {
        setError(error, "account ID format is invalid");
        return false;
    }
    if (!database_.setAppUserEnabled(*normalized, enabled, clock_())) {
        setError(error, "account was not found or database write failed");
        return false;
    }
    return true;
}

bool AuthService::resetPassword(const std::string& account_id,
                                const std::string& password,
                                std::string* error) {
    const auto normalized = normalizeAccountId(account_id);
    if (!normalized) {
        setError(error, "account ID format is invalid");
        return false;
    }
    if (!validPassword(password)) {
        setError(error, "password must contain at least 4 characters");
        return false;
    }
    try {
        const std::string password_hash = hashPassword(password);
        if (!database_.resetAppUserPassword(*normalized, password_hash,
                                            clock_())) {
            setError(error, "account was not found or database write failed");
            return false;
        }
        return true;
    } catch (...) {
        setError(error, "password hashing failed");
        return false;
    }
}

}  // namespace auth

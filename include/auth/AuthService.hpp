#pragma once

#include "database/EventDatabase.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace auth {

struct AuthConfig {
    int session_ttl_seconds{36000};
    int login_window_seconds{300};
    int login_max_failures{5};
    int login_cooldown_seconds{60};
};

struct AuthenticatedUser {
    std::int64_t id{-1};
    std::string account_id;
    std::string display_name;
    std::int64_t expires_at_utc{};
};

enum class LoginStatus {
    Success,
    InvalidRequest,
    InvalidCredentials,
    RateLimited,
    Unavailable
};

struct LoginResult {
    LoginStatus status{LoginStatus::Unavailable};
    std::string access_token;
    std::string expires_at;
    AuthenticatedUser user;
    int retry_after_seconds{};
};

/**
 * @brief Qt 앱 계정, Argon2id 비밀번호, Bearer 세션을 관리한다.
 *
 * 원문 비밀번호와 access token은 메서드 실행 중에만 메모리에 존재한다.
 * DB에는 Argon2id PHC 문자열과 SHA-256 token digest만 저장한다.
 */
class AuthService {
public:
    using Clock = std::function<std::int64_t()>;

    AuthService(database::EventDatabase& database,
                AuthConfig config = {}, Clock clock = {});

    LoginResult login(const std::string& account_id,
                      const std::string& password,
                      const std::string& source_ip);
    std::optional<AuthenticatedUser> authenticateBearer(
        std::string_view authorization_header) const;
    bool logoutBearer(std::string_view authorization_header);

    bool addUser(const std::string& account_id,
                 const std::string& password,
                 const std::string& display_name,
                 std::int64_t* user_id,
                 std::string* error);
    std::vector<database::AppUserRecord> listUsers() const;
    bool setUserEnabled(const std::string& account_id, bool enabled,
                        std::string* error);
    bool resetPassword(const std::string& account_id,
                       const std::string& password,
                       std::string* error);

    static std::optional<std::string> normalizeAccountId(
        std::string_view account_id);
    static bool validPassword(std::string_view password) noexcept;
    static std::string formatUtc(std::int64_t epoch_seconds);

private:
    struct RateState {
        std::deque<std::int64_t> failures;
        std::int64_t blocked_until{};
        std::int64_t last_seen{};
    };

    static std::string trim(std::string_view value);
    static std::string hashPassword(std::string_view password);
    static bool verifyPassword(std::string_view password,
                               const std::string& password_hash) noexcept;
    static std::string generateToken();
    static std::optional<std::vector<unsigned char>> tokenDigest(
        std::string_view bearer_token) noexcept;
    static std::optional<std::string_view> extractBearer(
        std::string_view authorization_header) noexcept;

    std::optional<int> rateLimitRetryAfter(const std::string& key,
                                           std::int64_t now);
    void recordFailure(const std::string& key, std::int64_t now);
    void clearFailures(const std::string& key);
    void pruneFailures(RateState& state, std::int64_t now) const;
    void boundRateTable(std::int64_t now);

    database::EventDatabase& database_;
    AuthConfig config_;
    Clock clock_;
    std::string dummy_password_hash_;
    mutable std::mutex rate_mutex_;
    std::unordered_map<std::string, RateState> rate_states_;
};

}  // namespace auth

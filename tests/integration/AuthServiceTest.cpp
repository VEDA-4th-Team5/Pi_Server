#include "auth/AuthService.hpp"
#include "database/EventDatabase.hpp"
#include "database/db_manager.h"

#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool execute(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::string scalarText(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement{};
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
        return {};
    std::string result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(statement, 0);
        if (value != nullptr)
            result = reinterpret_cast<const char*>(value);
    }
    sqlite3_finalize(statement);
    return result;
}

std::int64_t scalarInt(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement{};
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
        return -1;
    std::int64_t result{-1};
    if (sqlite3_step(statement) == SQLITE_ROW)
        result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pi-server-auth-test-" + std::to_string(getpid()));
    const fs::path db_path = root / "parking.db";
    fs::create_directories(root);

    sqlite3* bootstrap{};
    if (sqlite3_open(db_path.c_str(), &bootstrap) != SQLITE_OK) return 1;
    const bool bootstrap_ok = execute(bootstrap,
        "CREATE TABLE legacy_data(id INTEGER PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO legacy_data(value) VALUES('preserve-me');");
    sqlite3_close(bootstrap);
    if (!bootstrap_ok) return 1;

    database::EventDatabase database;
    if (!database.open(db_path.string())) return 1;
    database.migrateRuntimeSchema();
    database.migrateRuntimeSchema();

    bool success = true;
    sqlite3* native = db_native_handle();
    success &= expect(native != nullptr, "native SQLite handle");
    success &= expect(scalarInt(native, "PRAGMA foreign_keys;") == 1,
                      "foreign key enforcement enabled");
    success &= expect(scalarText(native,
        "SELECT value FROM legacy_data WHERE id=1;") == "preserve-me",
        "migration preserved existing data");
    success &= expect(scalarInt(native,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('app_users','app_sessions');") == 2,
        "authentication tables migrated");

    std::int64_t now = 1'776'931'200;
    try {
        auth::AuthConfig invalid_config;
        invalid_config.session_ttl_seconds = 60;
        auth::AuthService invalid_service(
            database, invalid_config, [&] { return now; });
        success &= expect(false, "invalid authentication TTL rejected");
    } catch (const std::invalid_argument&) {
        success &= expect(true, "invalid authentication TTL rejected");
    }
    auth::AuthService auth_service(database, {}, [&] { return now; });
    std::string error;
    std::int64_t user_id{};
    const std::string first_password = "pass";
    success &= expect(auth_service.addUser(
        " Operator ", first_password, "Parking Operator", &user_id, &error),
        "create normalized app user");
    success &= expect(user_id > 0, "positive user ID");

    const auto stored_user = database.findAppUser("operator");
    success &= expect(stored_user && stored_user->enabled &&
        stored_user->password_hash.rfind("$argon2id$", 0) == 0 &&
        stored_user->password_hash != first_password &&
        stored_user->password_hash.find(first_password) == std::string::npos,
        "Argon2id PHC hash stored without plaintext password");
    const auto listed_users = auth_service.listUsers();
    success &= expect(listed_users.size() == 1 &&
        listed_users.front().password_hash.empty(),
        "user list excludes password hash");

    success &= expect(auth_service.login(
        "bad account!", first_password, "10.0.0.1").status ==
        auth::LoginStatus::InvalidRequest, "invalid account request");
    success &= expect(auth_service.login(
        "operator", "abc", "10.0.0.1").status ==
        auth::LoginStatus::InvalidRequest, "short password request");
    success &= expect(auth_service.login(
        "missing", first_password, "10.0.0.1").status ==
        auth::LoginStatus::InvalidCredentials, "missing account is generic 401");
    success &= expect(auth_service.login(
        "operator", "nope", "10.0.0.2").status ==
        auth::LoginStatus::InvalidCredentials, "wrong password is generic 401");

    const auto first_login = auth_service.login(
        "OPERATOR", first_password, "10.0.0.3");
    const auto second_login = auth_service.login(
        "operator", first_password, "10.0.0.3");
    success &= expect(first_login.status == auth::LoginStatus::Success &&
        second_login.status == auth::LoginStatus::Success &&
        !first_login.access_token.empty() &&
        first_login.access_token != second_login.access_token &&
        first_login.expires_at == "2026-04-23T18:00:00Z",
        "new absolute-TTL token on every login");
    success &= expect(scalarInt(native,
        "SELECT COUNT(*) FROM app_sessions WHERE length(token_hash)=32 "
        "AND typeof(token_hash)='blob';") == 2,
        "only 32-byte token digests stored");
    success &= expect(scalarInt(native,
        "SELECT COUNT(*) FROM app_sessions WHERE CAST(token_hash AS TEXT) "
        "LIKE '%pass%';") == 0,
        "session table contains no credential plaintext");

    const std::string first_bearer = "Bearer " + first_login.access_token;
    const std::string second_bearer = "Bearer " + second_login.access_token;
    success &= expect(auth_service.authenticateBearer(first_bearer).has_value(),
                      "valid Bearer token");
    success &= expect(!auth_service.authenticateBearer("").has_value() &&
        !auth_service.authenticateBearer("bearer invalid").has_value() &&
        !auth_service.authenticateBearer("Bearer abc").has_value(),
        "missing and malformed Bearer tokens rejected");

    auth::AuthService restarted_service(database, {}, [&] { return now; });
    success &= expect(restarted_service.authenticateBearer(second_bearer).has_value(),
                      "session survives service restart");
    success &= expect(restarted_service.logoutBearer(first_bearer) &&
        !restarted_service.authenticateBearer(first_bearer).has_value() &&
        restarted_service.authenticateBearer(second_bearer).has_value(),
        "logout revokes only the current session");

    success &= expect(restarted_service.setUserEnabled(
        "operator", false, &error), "disable user");
    success &= expect(!restarted_service.authenticateBearer(second_bearer).has_value() &&
        restarted_service.login("operator", first_password, "10.0.0.4").status ==
            auth::LoginStatus::InvalidCredentials,
        "disabled user and all sessions rejected");
    success &= expect(restarted_service.setUserEnabled(
        "operator", true, &error), "enable user");

    const auto before_reset = restarted_service.login(
        "operator", first_password, "10.0.0.5");
    const std::string new_password = "replacement-password-456";
    success &= expect(before_reset.status == auth::LoginStatus::Success &&
        restarted_service.resetPassword(
            "operator", new_password, &error) &&
        !restarted_service.authenticateBearer(
            "Bearer " + before_reset.access_token).has_value() &&
        restarted_service.login("operator", first_password, "10.0.0.6").status ==
            auth::LoginStatus::InvalidCredentials,
        "password reset revokes sessions and old password");

    const auto expiring = restarted_service.login(
        "operator", new_password, "10.0.0.7");
    success &= expect(expiring.status == auth::LoginStatus::Success,
                      "new password login");
    now += 36001;
    success &= expect(!restarted_service.authenticateBearer(
        "Bearer " + expiring.access_token).has_value(), "expired token rejected");

    for (int attempt = 0; attempt < 5; ++attempt) {
        success &= expect(restarted_service.login(
            "operator", "another-wrong-password", "10.0.0.8").status ==
            auth::LoginStatus::InvalidCredentials, "rate-limit failure admitted");
    }
    const auto limited = restarted_service.login(
        "operator", new_password, "10.0.0.8");
    success &= expect(limited.status == auth::LoginStatus::RateLimited &&
        limited.retry_after_seconds == 60, "five failures trigger cooldown");
    now += 60;
    success &= expect(restarted_service.login(
        "operator", new_password, "10.0.0.8").status ==
        auth::LoginStatus::Success, "successful login clears failure history");

    database.close();
    fs::remove_all(root);
    if (success) std::cout << "Authentication service test passed\n";
    return success ? 0 : 1;
}

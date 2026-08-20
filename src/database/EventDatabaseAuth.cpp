#include "database/EventDatabase.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace database {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error("authentication database statement failed");
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bindInt64(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
            throw std::runtime_error("authentication integer bind failed");
    }

    void bindText(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(),
                              static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error("authentication text bind failed");
        }
    }

    void bindBlob(int index, const std::vector<unsigned char>& value) {
        if (sqlite3_bind_blob(statement_, index, value.data(),
                              static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error("authentication blob bind failed");
        }
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string{}
        : std::string(reinterpret_cast<const char*>(value));
}

void requireDone(sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE)
        throw std::runtime_error("authentication database mutation failed");
}

void begin(sqlite3* database) {
    if (sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error("authentication transaction begin failed");
    }
}

void commit(sqlite3* database) {
    if (sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error("authentication transaction commit failed");
    }
}

void rollback(sqlite3* database) noexcept {
    sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
}

AppUserRecord readUser(sqlite3_stmt* statement) {
    AppUserRecord user;
    user.user_id = sqlite3_column_int64(statement, 0);
    user.account_id = columnText(statement, 1);
    user.password_hash = columnText(statement, 2);
    user.display_name = columnText(statement, 3);
    user.enabled = sqlite3_column_int(statement, 4) == 1;
    user.created_at_utc = sqlite3_column_int64(statement, 5);
    user.updated_at_utc = sqlite3_column_int64(statement, 6);
    return user;
}

}  // namespace

std::optional<AppUserRecord> EventDatabase::findAppUser(
    const std::string& account_id) const {
    if (account_id.empty()) return std::nullopt;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;

    Statement statement(db_,
        "SELECT user_id,account_id,password_hash,COALESCE(display_name,''),"
        "enabled,created_at_utc,updated_at_utc FROM app_users "
        "WHERE account_id=? LIMIT 1;");
    statement.bindText(1, account_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return readUser(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error("authentication user lookup failed");
}

bool EventDatabase::createAppUser(const std::string& account_id,
                                  const std::string& password_hash,
                                  const std::string& display_name,
                                  std::int64_t now_utc,
                                  std::int64_t* user_id) {
    if (account_id.empty() || password_hash.empty() || user_id == nullptr)
        return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    try {
        begin(db_);
        Statement statement(db_,
            "INSERT INTO app_users(account_id,password_hash,display_name,"
            "enabled,created_at_utc,updated_at_utc) VALUES(?,?,?,1,?,?);");
        statement.bindText(1, account_id);
        statement.bindText(2, password_hash);
        statement.bindText(3, display_name);
        statement.bindInt64(4, now_utc);
        statement.bindInt64(5, now_utc);
        requireDone(statement.get());
        *user_id = sqlite3_last_insert_rowid(db_);
        commit(db_);
        return true;
    } catch (...) {
        rollback(db_);
        return false;
    }
}

std::vector<AppUserRecord> EventDatabase::listAppUsers() const {
    std::lock_guard lock(db_mutex_);
    std::vector<AppUserRecord> users;
    if (!opened_ || db_ == nullptr) return users;

    Statement statement(db_,
        "SELECT user_id,account_id,'' AS password_hash,"
        "COALESCE(display_name,''),enabled,created_at_utc,updated_at_utc "
        "FROM app_users ORDER BY user_id;");
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) return users;
        if (result != SQLITE_ROW)
            throw std::runtime_error("authentication user list failed");
        users.push_back(readUser(statement.get()));
    }
}

bool EventDatabase::setAppUserEnabled(const std::string& account_id,
                                      bool enabled,
                                      std::int64_t now_utc) {
    if (account_id.empty()) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    try {
        begin(db_);
        Statement update(db_,
            "UPDATE app_users SET enabled=?,updated_at_utc=? WHERE account_id=?;");
        update.bindInt64(1, enabled ? 1 : 0);
        update.bindInt64(2, now_utc);
        update.bindText(3, account_id);
        requireDone(update.get());
        if (sqlite3_changes(db_) != 1) {
            rollback(db_);
            return false;
        }
        if (!enabled) {
            Statement revoke(db_,
                "UPDATE app_sessions SET revoked_at_utc=? "
                "WHERE user_id=(SELECT user_id FROM app_users WHERE account_id=?) "
                "AND revoked_at_utc IS NULL;");
            revoke.bindInt64(1, now_utc);
            revoke.bindText(2, account_id);
            requireDone(revoke.get());
        }
        commit(db_);
        return true;
    } catch (...) {
        rollback(db_);
        return false;
    }
}

bool EventDatabase::resetAppUserPassword(
    const std::string& account_id,
    const std::string& password_hash,
    std::int64_t now_utc) {
    if (account_id.empty() || password_hash.empty()) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    try {
        begin(db_);
        Statement update(db_,
            "UPDATE app_users SET password_hash=?,updated_at_utc=? "
            "WHERE account_id=?;");
        update.bindText(1, password_hash);
        update.bindInt64(2, now_utc);
        update.bindText(3, account_id);
        requireDone(update.get());
        if (sqlite3_changes(db_) != 1) {
            rollback(db_);
            return false;
        }
        Statement revoke(db_,
            "UPDATE app_sessions SET revoked_at_utc=? "
            "WHERE user_id=(SELECT user_id FROM app_users WHERE account_id=?) "
            "AND revoked_at_utc IS NULL;");
        revoke.bindInt64(1, now_utc);
        revoke.bindText(2, account_id);
        requireDone(revoke.get());
        commit(db_);
        return true;
    } catch (...) {
        rollback(db_);
        return false;
    }
}

bool EventDatabase::createAppSession(
    std::int64_t user_id,
    const std::vector<unsigned char>& token_hash,
    std::int64_t created_at_utc,
    std::int64_t expires_at_utc) {
    if (user_id <= 0 || token_hash.size() != 32 ||
        expires_at_utc <= created_at_utc) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    try {
        begin(db_);
        Statement cleanup(db_,
            "DELETE FROM app_sessions WHERE expires_at_utc<=?;");
        cleanup.bindInt64(1, created_at_utc);
        requireDone(cleanup.get());
        Statement insert(db_,
            "INSERT INTO app_sessions(user_id,token_hash,created_at_utc,"
            "expires_at_utc,revoked_at_utc) VALUES(?,?,?,?,NULL);");
        insert.bindInt64(1, user_id);
        insert.bindBlob(2, token_hash);
        insert.bindInt64(3, created_at_utc);
        insert.bindInt64(4, expires_at_utc);
        requireDone(insert.get());
        commit(db_);
        return true;
    } catch (...) {
        rollback(db_);
        return false;
    }
}

std::optional<AppSessionPrincipal> EventDatabase::findActiveAppSession(
    const std::vector<unsigned char>& token_hash,
    std::int64_t now_utc) const {
    if (token_hash.size() != 32) return std::nullopt;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return std::nullopt;

    Statement statement(db_,
        "SELECT u.user_id,u.account_id,COALESCE(u.display_name,''),"
        "s.expires_at_utc FROM app_sessions s JOIN app_users u "
        "ON u.user_id=s.user_id WHERE s.token_hash=? "
        "AND s.revoked_at_utc IS NULL AND s.expires_at_utc>? "
        "AND u.enabled=1 LIMIT 1;");
    statement.bindBlob(1, token_hash);
    statement.bindInt64(2, now_utc);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW)
        throw std::runtime_error("authentication session lookup failed");

    AppSessionPrincipal principal;
    principal.user_id = sqlite3_column_int64(statement.get(), 0);
    principal.account_id = columnText(statement.get(), 1);
    principal.display_name = columnText(statement.get(), 2);
    principal.expires_at_utc = sqlite3_column_int64(statement.get(), 3);
    return principal;
}

bool EventDatabase::revokeAppSession(
    const std::vector<unsigned char>& token_hash,
    std::int64_t now_utc) {
    if (token_hash.size() != 32) return false;
    std::lock_guard lock(db_mutex_);
    if (!opened_ || db_ == nullptr) return false;

    try {
        begin(db_);
        Statement statement(db_,
            "UPDATE app_sessions SET revoked_at_utc=? WHERE token_hash=? "
            "AND revoked_at_utc IS NULL AND expires_at_utc>?;");
        statement.bindInt64(1, now_utc);
        statement.bindBlob(2, token_hash);
        statement.bindInt64(3, now_utc);
        requireDone(statement.get());
        const bool changed = sqlite3_changes(db_) == 1;
        commit(db_);
        return changed;
    } catch (...) {
        rollback(db_);
        return false;
    }
}

}  // namespace database

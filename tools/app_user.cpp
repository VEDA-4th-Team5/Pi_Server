#include "auth/AuthService.hpp"
#include "database/EventDatabase.hpp"

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

class Tty {
public:
    Tty() : stream_(std::fopen("/dev/tty", "r+")) {
        if (stream_ == nullptr || tcgetattr(fileno(stream_), &original_) != 0)
            throw std::runtime_error("interactive TTY is required");
    }

    ~Tty() {
        if (stream_ != nullptr) {
            tcsetattr(fileno(stream_), TCSAFLUSH, &original_);
            std::fclose(stream_);
        }
    }

    std::string readPassword(const char* prompt) {
        termios hidden = original_;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        std::fputs(prompt, stream_);
        std::fflush(stream_);
        if (tcsetattr(fileno(stream_), TCSAFLUSH, &hidden) != 0)
            throw std::runtime_error("cannot disable terminal echo");

        char buffer[1026]{};
        const bool read = std::fgets(buffer, sizeof(buffer), stream_) != nullptr;
        tcsetattr(fileno(stream_), TCSAFLUSH, &original_);
        std::fputc('\n', stream_);
        std::fflush(stream_);
        if (!read) {
            sodium_memzero(buffer, sizeof(buffer));
            throw std::runtime_error("password input failed");
        }
        buffer[std::strcspn(buffer, "\r\n")] = '\0';
        std::string password(buffer);
        sodium_memzero(buffer, sizeof(buffer));
        return password;
    }

private:
    FILE* stream_{};
    termios original_{};
};

void usage() {
    std::cerr
        << "Usage:\n"
        << "  app-user [--db PATH] add <account-id> [--display-name NAME]\n"
        << "  app-user [--db PATH] list\n"
        << "  app-user [--db PATH] disable <account-id>\n"
        << "  app-user [--db PATH] enable <account-id>\n"
        << "  app-user [--db PATH] reset-password <account-id>\n";
}

std::string readConfirmedPassword() {
    Tty tty;
    std::string first = tty.readPassword("Password: ");
    std::string second = tty.readPassword("Confirm password: ");
    if (first != second) {
        sodium_memzero(first.data(), first.size());
        sodium_memzero(second.data(), second.size());
        throw std::runtime_error("password confirmation does not match");
    }
    sodium_memzero(second.data(), second.size());
    return first;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = "data/db/parking.db";
    int index = 1;
    if (index < argc && std::string(argv[index]) == "--db") {
        if (++index >= argc) {
            usage();
            return 2;
        }
        db_path = argv[index++];
    }
    if (index >= argc) {
        usage();
        return 2;
    }
    const std::string command = argv[index++];

    database::EventDatabase database;
    if (!database.open(db_path)) {
        std::cerr << "Failed to open authentication database.\n";
        return 1;
    }
    try {
        database.migrateRuntimeSchema();
        auth::AuthService auth_service(database);

        if (command == "list") {
            if (index != argc) {
                usage();
                return 2;
            }
            std::cout << "ID\tACCOUNT\tDISPLAY NAME\tENABLED\tCREATED\tUPDATED\n";
            for (const auto& user : auth_service.listUsers()) {
                std::cout << user.user_id << '\t' << user.account_id << '\t'
                          << user.display_name << '\t'
                          << (user.enabled ? "yes" : "no") << '\t'
                          << auth::AuthService::formatUtc(user.created_at_utc)
                          << '\t'
                          << auth::AuthService::formatUtc(user.updated_at_utc)
                          << '\n';
            }
            return 0;
        }

        if (index >= argc) {
            usage();
            return 2;
        }
        const std::string account_id = argv[index++];
        std::string error;

        if (command == "add") {
            std::string display_name = account_id;
            if (index < argc && std::string(argv[index]) == "--display-name") {
                if (++index >= argc) {
                    usage();
                    return 2;
                }
                display_name = argv[index++];
            }
            if (index != argc) {
                usage();
                return 2;
            }
            std::string password = readConfirmedPassword();
            std::int64_t user_id{};
            const bool created = auth_service.addUser(
                account_id, password, display_name, &user_id, &error);
            sodium_memzero(password.data(), password.size());
            if (!created) throw std::runtime_error(error);
            std::cout << "Created app user ID " << user_id << ".\n";
            return 0;
        }

        if (index != argc) {
            usage();
            return 2;
        }
        if (command == "enable" || command == "disable") {
            const bool enabled = command == "enable";
            if (!auth_service.setUserEnabled(account_id, enabled, &error))
                throw std::runtime_error(error);
            std::cout << (enabled ? "Enabled" : "Disabled")
                      << " app user.\n";
            return 0;
        }
        if (command == "reset-password") {
            std::string password = readConfirmedPassword();
            const bool reset = auth_service.resetPassword(
                account_id, password, &error);
            sodium_memzero(password.data(), password.size());
            if (!reset) throw std::runtime_error(error);
            std::cout << "Password reset and active sessions revoked.\n";
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "app-user failed: " << error.what() << '\n';
        return 1;
    }
}

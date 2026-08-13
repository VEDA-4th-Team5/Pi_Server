#include "database/EventDatabase.hpp"
#include "event/FireAlarmService.hpp"
#include "event/FireDeliveryCoordinator.hpp"
#include "mqtt/MqttEndpoint.hpp"
#include "mqtt/MqttTransport.hpp"

#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kChannelId{"ch01"};
constexpr std::string_view kSensorId{"F1"};
constexpr std::string_view kRetainedTopic{"parking/fire/ch01"};
constexpr std::string_view kLifecycleTopic{"parking/v1/events/ch01"};

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string systemError(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("pi-fire-broker-restart-" + std::to_string(::getpid()) + '-' +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
        require(std::filesystem::create_directory(path_),
                "failed to create Fire broker test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int reserveLoopbackPort() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) throw std::runtime_error(systemError("socket"));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        const auto error = systemError("bind ephemeral broker port");
        ::close(socket_fd);
        throw std::runtime_error(error);
    }
    socklen_t length = sizeof(address);
    if (::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address),
                      &length) != 0) {
        const auto error = systemError("getsockname");
        ::close(socket_fd);
        throw std::runtime_error(error);
    }
    const int port = ntohs(address.sin_port);
    ::close(socket_fd);
    return port;
}

bool waitForProcess(pid_t process_id, const std::chrono::milliseconds timeout,
                    int& status) {
#ifdef SYS_pidfd_open
    const int process_fd = static_cast<int>(
        ::syscall(SYS_pidfd_open, process_id, 0));
    if (process_fd >= 0) {
        pollfd descriptor{process_fd, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        ::close(process_fd);
        if (result <= 0) return false;
        return ::waitpid(process_id, &status, 0) == process_id;
    }
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(process_id, &status, WNOHANG);
        if (result == process_id) return true;
        if (result < 0) return false;
        (void)::poll(nullptr, 0, 10);
    }
    return false;
}

bool loopbackPortAcceptsConnections(const int port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_fd < 0) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    int result = ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address),
                           sizeof(address));
    if (result != 0 && errno == EINPROGRESS) {
        pollfd descriptor{socket_fd, POLLOUT, 0};
        result = ::poll(&descriptor, 1, 100);
        if (result > 0) {
            int error{};
            socklen_t length = sizeof(error);
            result = ::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error,
                                  &length);
            if (result == 0) result = error == 0 ? 0 : -1;
        } else {
            result = -1;
        }
    }
    ::close(socket_fd);
    return result == 0;
}

class BrokerProcess {
public:
    BrokerProcess(const std::filesystem::path& directory, const int port)
        : port_(port),
          config_path_(directory / "mosquitto.conf"),
          log_path_(directory / "mosquitto.log") {
        std::ofstream config(config_path_);
        require(static_cast<bool>(config), "failed to create mosquitto config");
        config << "listener " << port_ << " 127.0.0.1\n"
               << "allow_anonymous true\n"
               << "persistence false\n"
               << "log_dest stdout\n"
               << "connection_messages true\n";
        config.close();

        process_id_ = ::fork();
        if (process_id_ < 0) throw std::runtime_error(systemError("fork broker"));
        if (process_id_ == 0) {
            const int log_fd = ::open(log_path_.c_str(),
                                      O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (log_fd >= 0) {
                (void)::dup2(log_fd, STDOUT_FILENO);
                (void)::dup2(log_fd, STDERR_FILENO);
                ::close(log_fd);
            }
            ::execlp("mosquitto", "mosquitto", "-c",
                     config_path_.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            const pid_t exited = ::waitpid(process_id_, &status, WNOHANG);
            if (exited == process_id_) {
                process_id_ = -1;
                throw std::runtime_error("mosquitto exited during startup; log: " +
                                         readLog());
            }
            if (loopbackPortAcceptsConnections(port_)) return;
            (void)::poll(nullptr, 0, 20);
        }
        stop();
        throw std::runtime_error("mosquitto socket readiness timeout; log: " +
                                 readLog());
    }

    ~BrokerProcess() { stop(); }

    int port() const noexcept { return port_; }

    void stop() noexcept {
        if (process_id_ <= 0) return;
        (void)::kill(process_id_, SIGTERM);
        int status{};
        if (!waitForProcess(process_id_, 2s, status)) {
            (void)::kill(process_id_, SIGKILL);
            (void)::waitpid(process_id_, &status, 0);
        }
        process_id_ = -1;
    }

private:
    std::string readLog() const {
        std::ifstream stream(log_path_);
        return {std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
    }

    int port_{};
    pid_t process_id_{-1};
    std::filesystem::path config_path_;
    std::filesystem::path log_path_;
};

bool sendAll(const int socket_fd, const std::uint8_t* data,
             const std::size_t size) {
    std::size_t sent{};
    while (sent < size) {
        const ssize_t result = ::send(socket_fd, data + sent, size - sent,
                                      MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::optional<std::size_t> mqttPacketSize(
    const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < 2) return std::nullopt;
    std::size_t remaining{};
    std::size_t multiplier{1};
    for (std::size_t index = 1; index <= 4; ++index) {
        if (index >= buffer.size()) return std::nullopt;
        const std::uint8_t encoded = buffer[index];
        remaining += static_cast<std::size_t>(encoded & 0x7fU) * multiplier;
        if ((encoded & 0x80U) == 0) {
            const std::size_t total = index + 1 + remaining;
            if (total > 1024U * 1024U) {
                throw std::runtime_error("MQTT packet exceeded test proxy limit");
            }
            return buffer.size() >= total ? std::optional<std::size_t>(total)
                                          : std::nullopt;
        }
        multiplier *= 128U;
    }
    throw std::runtime_error("malformed MQTT remaining length");
}

bool packetContains(const std::vector<std::uint8_t>& packet,
                    const std::string_view marker) {
    return std::search(packet.begin(), packet.end(), marker.begin(), marker.end())
        != packet.end();
}

class BlockingMqttProxy {
public:
    explicit BlockingMqttProxy(const int upstream_port)
        : upstream_port_(upstream_port) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error(systemError("proxy socket"));
        int reuse = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0 || ::listen(listen_fd_, 1) != 0) {
            const auto error = systemError("proxy bind/listen");
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(error);
        }
        socklen_t length = sizeof(address);
        require(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                              &length) == 0,
                systemError("proxy getsockname"));
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    ~BlockingMqttProxy() { stop(); }

    int port() const noexcept { return port_; }

    void waitUntilClearBlocked(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] {
                return clear_blocked_ || !error_.empty();
            })) {
            throw std::runtime_error(
                "proxy did not intercept retained CLEAR before timeout");
        }
        if (!error_.empty()) throw std::runtime_error(error_);
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            if (listen_fd_ >= 0) (void)::shutdown(listen_fd_, SHUT_RDWR);
            if (client_fd_ >= 0) (void)::shutdown(client_fd_, SHUT_RDWR);
            if (upstream_fd_ >= 0) (void)::shutdown(upstream_fd_, SHUT_RDWR);
            condition_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (client_fd_ >= 0) ::close(client_fd_);
        if (upstream_fd_ >= 0) ::close(upstream_fd_);
        listen_fd_ = -1;
        client_fd_ = -1;
        upstream_fd_ = -1;
    }

private:
    bool stopping() const {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    void fail(std::string error) noexcept {
        std::lock_guard lock(mutex_);
        if (!stopping_ && error_.empty()) error_ = std::move(error);
        condition_.notify_all();
    }

    int connectUpstream() {
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) throw std::runtime_error(systemError("upstream socket"));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(upstream_port_));
        if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) != 0) {
            const auto error = systemError("proxy connect upstream");
            ::close(socket_fd);
            throw std::runtime_error(error);
        }
        return socket_fd;
    }

    bool forwardClientPackets(std::vector<std::uint8_t>& buffer) {
        for (;;) {
            const auto total = mqttPacketSize(buffer);
            if (!total) return true;
            std::vector<std::uint8_t> packet(
                buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*total));
            buffer.erase(buffer.begin(),
                         buffer.begin() + static_cast<std::ptrdiff_t>(*total));
            const bool publish = (packet.front() >> 4U) == 3U;
            const bool retained = (packet.front() & 0x01U) != 0;
            if (publish && retained &&
                packetContains(packet, "\"event_type\":\"FIRE_CLEARED\"") &&
                packetContains(packet, "\"fire_revision\":3")) {
                std::unique_lock lock(mutex_);
                clear_blocked_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this] { return stopping_; });
                return false;
            }
            if (!sendAll(upstream_fd_, packet.data(), packet.size())) {
                throw std::runtime_error("proxy failed to forward client packet");
            }
        }
    }

    void run() noexcept {
        try {
            pollfd listener{listen_fd_, POLLIN, 0};
            while (!stopping()) {
                const int ready = ::poll(&listener, 1, 250);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0) throw std::runtime_error(systemError("proxy poll accept"));
                if (ready == 0) continue;
                const int accepted = ::accept(listen_fd_, nullptr, nullptr);
                if (accepted < 0) {
                    if (stopping()) return;
                    throw std::runtime_error(systemError("proxy accept"));
                }
                {
                    std::lock_guard lock(mutex_);
                    client_fd_ = accepted;
                }
                break;
            }
            if (stopping()) return;

            const int connected = connectUpstream();
            {
                std::lock_guard lock(mutex_);
                upstream_fd_ = connected;
            }
            std::vector<std::uint8_t> client_buffer;
            std::vector<std::uint8_t> read_buffer(8192);
            while (!stopping()) {
                pollfd descriptors[2]{{client_fd_, POLLIN, 0},
                                      {upstream_fd_, POLLIN, 0}};
                const int ready = ::poll(descriptors, 2, 250);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0) throw std::runtime_error(systemError("proxy poll"));
                if (ready == 0) continue;
                if ((descriptors[0].revents & POLLIN) != 0) {
                    const ssize_t size = ::recv(client_fd_, read_buffer.data(),
                                                read_buffer.size(), 0);
                    if (size <= 0) {
                        if (stopping()) return;
                        throw std::runtime_error("proxy client disconnected");
                    }
                    client_buffer.insert(
                        client_buffer.end(), read_buffer.begin(),
                        read_buffer.begin() + static_cast<std::ptrdiff_t>(size));
                    if (!forwardClientPackets(client_buffer)) return;
                }
                if ((descriptors[1].revents & POLLIN) != 0) {
                    const ssize_t size = ::recv(upstream_fd_, read_buffer.data(),
                                                read_buffer.size(), 0);
                    if (size <= 0) {
                        if (stopping()) return;
                        throw std::runtime_error("proxy broker disconnected");
                    }
                    if (!sendAll(client_fd_, read_buffer.data(),
                                 static_cast<std::size_t>(size))) {
                        throw std::runtime_error(
                            "proxy failed to forward broker packet");
                    }
                }
                if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
                    (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    if (stopping()) return;
                    throw std::runtime_error("proxy socket closed unexpectedly");
                }
            }
        } catch (const std::exception& error) {
            fail(std::string("MQTT blocking proxy failed: ") + error.what());
        }
        std::lock_guard lock(mutex_);
        if (client_fd_ >= 0) ::close(client_fd_);
        if (upstream_fd_ >= 0) ::close(upstream_fd_);
        client_fd_ = -1;
        upstream_fd_ = -1;
    }

    int upstream_port_{};
    int port_{};
    int listen_fd_{-1};
    int client_fd_{-1};
    int upstream_fd_{-1};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool clear_blocked_{};
    bool stopping_{};
    std::string error_;
};

class MosquittoLibrary {
public:
    MosquittoLibrary() {
        require(::mosquitto_lib_init() == MOSQ_ERR_SUCCESS,
                "mosquitto_lib_init failed");
    }
    ~MosquittoLibrary() { ::mosquitto_lib_cleanup(); }
};

struct ObservedMessage {
    std::string payload;
    bool retained{};
};

class RetainedObserver {
public:
    RetainedObserver(const int port, std::string client_id,
                     std::string topic = std::string(kRetainedTopic))
        : topic_(std::move(topic)) {
        handle_ = ::mosquitto_new(client_id.c_str(), true, this);
        require(handle_ != nullptr, "mosquitto_new subscriber failed");
        ::mosquitto_connect_callback_set(handle_, &RetainedObserver::onConnect);
        ::mosquitto_subscribe_callback_set(handle_, &RetainedObserver::onSubscribe);
        ::mosquitto_message_callback_set(handle_, &RetainedObserver::onMessage);
        const int connected = ::mosquitto_connect(handle_, "127.0.0.1", port, 10);
        require(connected == MOSQ_ERR_SUCCESS,
                "subscriber connect failed: " + std::to_string(connected));
        const int loop_started = ::mosquitto_loop_start(handle_);
        require(loop_started == MOSQ_ERR_SUCCESS,
                "subscriber loop_start failed: " + std::to_string(loop_started));
        loop_started_ = true;
        waitSubscribed(5s);
    }

    ~RetainedObserver() { stop(); }

    ObservedMessage waitForState(const std::string& state,
                                 const std::uint64_t revision,
                                 const bool require_retained,
                                 const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto matches = [&](const ObservedMessage& message) {
            if (require_retained && !message.retained) return false;
            try {
                const auto json = nlohmann::json::parse(message.payload);
                return json.value("alarm_state", std::string{}) == state &&
                       json.value("fire_revision", std::uint64_t{}) == revision;
            } catch (...) {
                return false;
            }
        };
        const auto find_match = [&]() {
            return std::find_if(messages_.begin(), messages_.end(), matches);
        };
        if (!condition_.wait_for(lock, timeout, [&] {
                return !error_.empty() || find_match() != messages_.end();
            })) {
            throw std::runtime_error("subscriber timeout waiting for " + state +
                                     " revision " + std::to_string(revision));
        }
        if (!error_.empty()) throw std::runtime_error(error_);
        return *find_match();
    }

    ObservedMessage waitForLifecycle(
        const std::string& event_id,
        const std::uint64_t revision,
        const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto matches = [&](const ObservedMessage& message) {
            try {
                const auto json = nlohmann::json::parse(message.payload);
                return json.value("event_id", std::string{}) == event_id &&
                    json.value("delivery_id", std::string{}) ==
                        "fire-lifecycle:" + event_id &&
                    json.value("fire_revision", std::uint64_t{}) == revision;
            } catch (...) {
                return false;
            }
        };
        const auto find_match = [&]() {
            return std::find_if(messages_.begin(), messages_.end(), matches);
        };
        if (!condition_.wait_for(lock, timeout, [&] {
                return !error_.empty() || find_match() != messages_.end();
            })) {
            throw std::runtime_error(
                "subscriber timeout waiting for lifecycle " + event_id);
        }
        if (!error_.empty()) throw std::runtime_error(error_);
        return *find_match();
    }

    void stop() noexcept {
        if (!handle_) return;
        (void)::mosquitto_disconnect(handle_);
        if (loop_started_) (void)::mosquitto_loop_stop(handle_, true);
        ::mosquitto_destroy(handle_);
        handle_ = nullptr;
        loop_started_ = false;
    }

private:
    void waitSubscribed(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] {
                return subscribed_ || !error_.empty();
            })) {
            throw std::runtime_error("subscriber SUBACK timeout");
        }
        if (!error_.empty()) throw std::runtime_error(error_);
    }

    void setError(std::string error) noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (error_.empty()) error_ = std::move(error);
            condition_.notify_all();
        } catch (...) {
        }
    }

    static void onConnect(mosquitto* handle, void* context,
                          const int result) noexcept {
        auto* self = static_cast<RetainedObserver*>(context);
        if (!self) return;
        if (result != 0) {
            self->setError("subscriber CONNACK failed: " +
                           std::to_string(result));
            return;
        }
        int message_id{};
        const int subscribed = ::mosquitto_subscribe(
            handle, &message_id, self->topic_.c_str(), 1);
        if (subscribed != MOSQ_ERR_SUCCESS) {
            self->setError("subscriber subscribe failed: " +
                           std::to_string(subscribed));
        }
    }

    static void onSubscribe(mosquitto*, void* context, int, int qos_count,
                            const int* granted_qos) noexcept {
        auto* self = static_cast<RetainedObserver*>(context);
        if (!self) return;
        try {
            std::lock_guard lock(self->mutex_);
            if (qos_count != 1 || !granted_qos || granted_qos[0] == 0x80) {
                self->error_ = "subscriber SUBACK rejected";
            } else {
                self->subscribed_ = true;
            }
            self->condition_.notify_all();
        } catch (...) {
        }
    }

    static void onMessage(mosquitto*, void* context,
                          const mosquitto_message* message) noexcept {
        auto* self = static_cast<RetainedObserver*>(context);
        if (!self || !message) return;
        try {
            ObservedMessage observed;
            observed.retained = message->retain;
            if (message->payload && message->payloadlen > 0) {
                observed.payload.assign(
                    static_cast<const char*>(message->payload),
                    static_cast<std::size_t>(message->payloadlen));
            }
            std::lock_guard lock(self->mutex_);
            self->messages_.push_back(std::move(observed));
            self->condition_.notify_all();
        } catch (...) {
            self->setError("subscriber failed to retain message copy");
        }
    }

    mosquitto* handle_{};
    std::string topic_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<ObservedMessage> messages_;
    std::string error_;
    bool loop_started_{};
    bool subscribed_{};
};

class ControlChannel {
public:
    explicit ControlChannel(const int descriptor = -1) : descriptor_(descriptor) {}
    ~ControlChannel() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;
    ControlChannel(ControlChannel&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)),
          buffer_(std::move(other.buffer_)) {}
    ControlChannel& operator=(ControlChannel&& other) noexcept {
        if (this == &other) return *this;
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        buffer_ = std::move(other.buffer_);
        return *this;
    }

    void sendLine(const std::string& line) {
        const std::string framed = line + '\n';
        require(sendAll(descriptor_,
                        reinterpret_cast<const std::uint8_t*>(framed.data()),
                        framed.size()),
                "control channel send failed");
    }

    std::string receiveLine(const std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                return line;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= 0ms) {
                throw std::runtime_error("control channel receive timeout");
            }
            pollfd descriptor{descriptor_, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1,
                                     static_cast<int>(remaining.count()));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) {
                throw std::runtime_error("control channel receive timeout");
            }
            char chunk[512];
            const ssize_t size = ::recv(descriptor_, chunk, sizeof(chunk), 0);
            if (size <= 0) {
                throw std::runtime_error(
                    "child control channel closed before expected barrier");
            }
            buffer_.append(chunk, static_cast<std::size_t>(size));
        }
    }

    int descriptor() const noexcept { return descriptor_; }

private:
    int descriptor_{-1};
    std::string buffer_;
};

class SpawnedChild {
public:
    SpawnedChild(pid_t process_id, ControlChannel control)
        : process_id_(process_id), control_(std::move(control)) {}
    ~SpawnedChild() { killNow(); }
    SpawnedChild(const SpawnedChild&) = delete;
    SpawnedChild& operator=(const SpawnedChild&) = delete;

    ControlChannel& control() noexcept { return control_; }

    void killNow() noexcept {
        if (process_id_ <= 0) return;
        (void)::kill(process_id_, SIGKILL);
        int status{};
        (void)::waitpid(process_id_, &status, 0);
        process_id_ = -1;
    }

    int wait(const std::chrono::milliseconds timeout) {
        require(process_id_ > 0, "child process is not running");
        int status{};
        require(waitForProcess(process_id_, timeout, status),
                "child process exit timeout");
        process_id_ = -1;
        return status;
    }

private:
    pid_t process_id_{-1};
    ControlChannel control_;
};

std::unique_ptr<SpawnedChild> spawnSelf(
    const std::filesystem::path& executable,
    const std::string& mode,
    const std::filesystem::path& database_path,
    const int broker_port) {
    int sockets[2]{};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            systemError("socketpair"));

    std::vector<std::string> arguments{
        executable.string(), mode, database_path.string(),
        std::to_string(broker_port), std::to_string(sockets[1])};
    std::vector<char*> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (auto& argument : arguments) raw_arguments.push_back(argument.data());
    raw_arguments.push_back(nullptr);

    const pid_t process_id = ::fork();
    if (process_id < 0) {
        const auto error = systemError("fork test child");
        ::close(sockets[0]);
        ::close(sockets[1]);
        throw std::runtime_error(error);
    }
    if (process_id == 0) {
        ::close(sockets[0]);
        ::execv(raw_arguments[0], raw_arguments.data());
        ::_exit(127);
    }
    ::close(sockets[1]);
    return std::make_unique<SpawnedChild>(
        process_id, ControlChannel(sockets[0]));
}

class ResultCollector {
public:
    void add(const event::FireCommandResult& result) {
        std::lock_guard lock(mutex_);
        results_[result.ticket] = result;
        condition_.notify_all();
    }

    event::FireCommandResult wait(const std::uint64_t ticket,
                                  const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        require(condition_.wait_for(lock, timeout, [&] {
                    return results_.contains(ticket);
                }),
                "Fire command result timeout");
        return results_.at(ticket);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::map<std::uint64_t, event::FireCommandResult> results_;
};

event::FireSignal fireSignal(const bool detected,
                             const std::uint64_t sequence) {
    event::FireSignal signal;
    signal.sensorId = std::string(kSensorId);
    signal.detected = detected;
    signal.occurredAt = std::chrono::system_clock::now();
    signal.sourceSequence = sequence;
    signal.sourceTransport = "uart";
    signal.rawPayload = std::string("FIRE:F1:") +
        (detected ? "DETECTED:" : "CLEARED:") + std::to_string(sequence);
    return signal;
}

struct ChildRuntime {
    explicit ChildRuntime(const std::filesystem::path& database_path,
                          const int broker_port)
        : database(database_path),
          endpoint(mqtt::makeMosquittoTransport()) {
        database.migrateRuntimeSchema();
        event::FireAlarmService::Config service_config;
        service_config.cameraId = "cam01";
        event::FireDeliveryCoordinator::Config delivery_config;
        delivery_config.channelIds = {std::string(kChannelId)};
        delivery_config.localFailureRetryDelay = 25ms;

        coordinator = std::make_unique<event::FireDeliveryCoordinator>(
            database, delivery_config,
            [this](const event::FireOutboxRecord& delivery,
                   mqtt::MqttPublishCorrelation correlation,
                   const mqtt::MqttConnectionEpoch expected_epoch) {
                return endpoint.publishTracked(
                    delivery.topic, delivery.payloadJson, delivery.retain,
                    std::move(correlation), expected_epoch);
            });
        service = std::make_unique<event::FireAlarmService>(
            database, service_config,
            std::vector<event::FireChannelBinding>{
                {std::string(kSensorId), std::string(kChannelId),
                 std::string(kRetainedTopic)}},
            [this](const event::FireCommandResult& result) {
                results.add(result);
                if (result.status == event::FireCommandStatus::DurablyCommitted ||
                    result.status == event::FireCommandStatus::Idempotent) {
                    coordinator->notifyOutboxChanged(result.channelId);
                }
            },
            [this](const std::string& channel, const std::uint64_t revision) {
                return coordinator->isRevisionSynchronized(channel, revision);
            },
            [this](const std::string& channel) {
                return coordinator->beginDomainMutation(channel);
            },
            [this](const std::string& channel) {
                coordinator->completeDomainMutation(channel);
            });
        require(service->initialize(), "Fire service initialize failed: " +
                                       service->configurationError());
        require(endpoint.bindApplicationHandler(
                    [](const std::string&, const std::string&) {}),
                "MQTT application handler bind failed");
        mqtt::IMqttTransport::ObserverCallbacks observers;
        observers.onFact = [this](const mqtt::MqttTransportFact& fact) {
            (void)coordinator->enqueueTransportFact(fact);
        };
        require(endpoint.bindTransportObservers(std::move(observers)),
                "MQTT transport observer bind failed");
        options.clientId = "fire-restart-" + std::to_string(::getpid());
        options.host = "127.0.0.1";
        options.port = broker_port;
        options.keepAliveSeconds = 5;
    }

    ~ChildRuntime() {
        // The endpoint owns callbacks into the coordinator. Preserve the same
        // shutdown order on both success and exception unwind so a diagnostic
        // failure in the harness cannot manufacture a callback UAF.
        try {
            if (service) (void)service->stop(5s);
        } catch (...) {
        }
        endpoint.closeIngress();
        (void)endpoint.quiesceIngress(5s);
        (void)endpoint.stop(5s);
        try {
            if (coordinator) (void)coordinator->stop();
        } catch (...) {
        }
    }

    void startDomainAndCoordinator() {
        require(coordinator->start(), "Fire delivery coordinator start failed");
        require(service->start(), "Fire service start failed");
    }

    void startTransport() {
        require(endpoint.start(
                    options,
                    {{"parking/v1/fire/ack/+", 1},
                     {"camera/events/#", 0}}),
                "MQTT endpoint start/SUBACK barrier failed");
    }

    void stopCleanly() {
        require(service->stop(5s), "Fire service stop failed");
        endpoint.closeIngress();
        require(endpoint.quiesceIngress(5s), "MQTT ingress quiesce failed");
        require(endpoint.stop(5s), "MQTT endpoint stop failed");
        require(coordinator->stop(), "Fire delivery coordinator stop failed");
    }

    database::EventDatabase database;
    ResultCollector results;
    mqtt::MqttEndpoint endpoint;
    mqtt::MqttConnectionOptions options;
    std::unique_ptr<event::FireDeliveryCoordinator> coordinator;
    std::unique_ptr<event::FireAlarmService> service;
};

int runPhaseOne(const std::filesystem::path& database_path,
                const int proxy_port, const int control_descriptor) {
    ControlChannel control(control_descriptor);
    std::unique_ptr<ChildRuntime> runtime;
    try {
        runtime = std::make_unique<ChildRuntime>(database_path, proxy_port);
        runtime->startDomainAndCoordinator();
        runtime->startTransport();
        require(runtime->coordinator->waitUntilReady(10s),
                "bootstrap retained state did not receive PUBACK");

        const auto open = runtime->service->submitSignal(fireSignal(true, 1));
        const auto open_result = runtime->results.wait(open.ticket, 5s);
        require(open_result.status == event::FireCommandStatus::DurablyCommitted &&
                    open_result.fireRevision == 2,
                "OPEN was not durably committed as revision 2: " +
                    open_result.error);
        require(runtime->coordinator->waitUntilReady(10s) &&
                    runtime->coordinator->isRevisionSynchronized(
                        std::string(kChannelId), 2),
                "OPEN retained state did not receive exact PUBACK");
        control.sendLine("OPEN_SYNCED");

        const auto clear = runtime->service->submitSignal(fireSignal(false, 2));
        const auto clear_result = runtime->results.wait(clear.ticket, 5s);
        require(clear_result.status == event::FireCommandStatus::DurablyCommitted &&
                    clear_result.fireRevision == 3,
                "CLEAR was not durably committed as revision 3: " +
                    clear_result.error);
        control.sendLine("CLEAR_COMMITTED");

        (void)control.receiveLine(24h);
        return 2;
    } catch (const std::exception& error) {
        try {
            control.sendLine(std::string("ERROR ") + error.what());
        } catch (...) {
        }
        return 1;
    }
}

int runRecovery(const std::filesystem::path& database_path,
                const int broker_port, const int control_descriptor) {
    ControlChannel control(control_descriptor);
    std::unique_ptr<ChildRuntime> runtime;
    try {
        runtime = std::make_unique<ChildRuntime>(database_path, broker_port);
        runtime->startDomainAndCoordinator();
        control.sendLine("OUTBOX_RESET_PENDING");
        require(control.receiveLine(10s) == "CONTINUE",
                "recovery continuation barrier mismatch");

        // No submitSignal() call is made in this process. Recovery is driven
        // solely by the durable revision-3 outbox row restored at startup.
        runtime->startTransport();
        require(runtime->coordinator->waitUntilReady(10s) &&
                    runtime->coordinator->isRevisionSynchronized(
                        std::string(kChannelId), 3),
                "recovered CLEAR retained state did not receive exact PUBACK");
        require(runtime->coordinator->drainDeliveries(10s),
                "recovered Fire delivery outbox did not drain");
        control.sendLine("RESOLVED_SYNCED");
        runtime->stopCleanly();
        runtime.reset();
        return 0;
    } catch (const std::exception& error) {
        try {
            control.sendLine(std::string("ERROR ") + error.what());
        } catch (...) {
        }
        return 1;
    }
}

void assertDurableClearUnacknowledged(const std::filesystem::path& path) {
    database::EventDatabase database(path);
    const auto state = database.getFireAlarmState(std::string(kChannelId));
    require(state && state->fireRevision == 3 &&
                state->desiredLifecycle == event::FireAlarmLifecycle::Resolved &&
                state->activeAlarmId.empty(),
            "SQLite did not preserve authoritative revision-3 RESOLVED state");
    const auto delivery = database.getFireDelivery("fire-state:ch01");
    require(delivery && delivery->fireRevision == 3 &&
                delivery->deliveryState != event::FireDeliveryState::Acknowledged &&
                (!delivery->acknowledgedRevision ||
                 *delivery->acknowledgedRevision != 3) &&
                delivery->payloadJson.find("\"event_type\":\"FIRE_CLEARED\"")
                    != std::string::npos,
            "SQLite did not preserve unacknowledged retained CLEAR intent");
}

void assertRetainedDeliveryPending(const std::filesystem::path& path) {
    database::EventDatabase database(path);
    const auto delivery = database.getFireDelivery("fire-state:ch01");
    require(delivery && delivery->fireRevision == 3 &&
                delivery->deliveryState == event::FireDeliveryState::Pending,
            "coordinator restart did not reset revision-3 CLEAR to PENDING");
}

void assertRetainedDeliveryAcknowledged(const std::filesystem::path& path) {
    database::EventDatabase database(path);
    const auto delivery = database.getFireDelivery("fire-state:ch01");
    require(delivery && delivery->fireRevision == 3 &&
                delivery->deliveryState == event::FireDeliveryState::Acknowledged &&
                delivery->acknowledgedRevision &&
                *delivery->acknowledgedRevision == 3,
            "recovered revision-3 CLEAR was not durably acknowledged");
    const auto lifecycle = database.listFireLifecycleDeliveries();
    const auto clear = std::find_if(
        lifecycle.begin(), lifecycle.end(), [](const auto& row) {
            return row.eventId == "fire-event:ch01:3";
        });
    require(clear != lifecycle.end() &&
                clear->deliveryKey ==
                    "fire-lifecycle:fire-event:ch01:3" &&
                clear->fireRevision == 3 &&
                clear->deliveryState ==
                    event::FireDeliveryState::Acknowledged,
            "recovered CLEAR lifecycle event did not preserve stable ID/PUBACK");
}

std::filesystem::path currentExecutable() {
    std::vector<char> buffer(4096);
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(),
                                    buffer.size() - 1);
    require(size > 0, systemError("readlink /proc/self/exe"));
    buffer[static_cast<std::size_t>(size)] = '\0';
    return std::filesystem::path(buffer.data());
}

void runAcceptanceTest() {
    TemporaryDirectory temporary;
    const auto database_path = temporary.path() / "fire.db";
    BrokerProcess broker(temporary.path(), reserveLoopbackPort());
    MosquittoLibrary mqtt_library;

    RetainedObserver initial_observer(
        broker.port(), "fire-observer-initial-" + std::to_string(::getpid()));
    BlockingMqttProxy proxy(broker.port());
    auto phase_one = spawnSelf(currentExecutable(), "--phase-one",
                               database_path, proxy.port());

    const std::string open_barrier = phase_one->control().receiveLine(15s);
    require(open_barrier == "OPEN_SYNCED",
            "phase-one OPEN synchronization barrier failed: " + open_barrier);
    (void)initial_observer.waitForState("OPEN", 2, false, 5s);
    const std::string clear_barrier = phase_one->control().receiveLine(10s);
    require(clear_barrier == "CLEAR_COMMITTED",
            "phase-one CLEAR commit barrier failed: " + clear_barrier);
    proxy.waitUntilClearBlocked(10s);

    phase_one->killNow();
    proxy.stop();
    initial_observer.stop();
    assertDurableClearUnacknowledged(database_path);

    RetainedObserver recovery_observer(
        broker.port(), "fire-observer-recovery-" + std::to_string(::getpid()));
    RetainedObserver lifecycle_observer(
        broker.port(), "fire-lifecycle-recovery-" +
            std::to_string(::getpid()),
        std::string(kLifecycleTopic));
    (void)recovery_observer.waitForState("OPEN", 2, true, 5s);

    auto recovery = spawnSelf(currentExecutable(), "--recovery",
                              database_path, broker.port());
    const std::string pending_barrier = recovery->control().receiveLine(10s);
    require(pending_barrier == "OUTBOX_RESET_PENDING",
            "recovery PENDING barrier failed: " + pending_barrier);
    assertRetainedDeliveryPending(database_path);
    recovery->control().sendLine("CONTINUE");

    (void)recovery_observer.waitForState("RESOLVED", 3, false, 10s);
    (void)lifecycle_observer.waitForLifecycle(
        "fire-event:ch01:3", 3, 10s);
    const std::string resolved_barrier = recovery->control().receiveLine(10s);
    require(resolved_barrier == "RESOLVED_SYNCED",
            "recovery synchronization barrier failed: " + resolved_barrier);
    const int recovery_status = recovery->wait(10s);
    require(WIFEXITED(recovery_status) && WEXITSTATUS(recovery_status) == 0,
            "recovery child exited unsuccessfully");
    recovery_observer.stop();
    lifecycle_observer.stop();

    RetainedObserver final_observer(
        broker.port(), "fire-observer-final-" + std::to_string(::getpid()));
    (void)final_observer.waitForState("RESOLVED", 3, true, 5s);
    final_observer.stop();
    assertRetainedDeliveryAcknowledged(database_path);
}

}  // namespace

int main(int argc, char** argv) {
    (void)::signal(SIGPIPE, SIG_IGN);
    if (argc == 5 && std::string_view(argv[1]) == "--phase-one") {
        return runPhaseOne(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
    }
    if (argc == 5 && std::string_view(argv[1]) == "--recovery") {
        return runRecovery(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
    }
    try {
        runAcceptanceTest();
    } catch (const std::exception& error) {
        std::cerr << "FireBrokerRestartProcessTest failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "FireBrokerRestartProcessTest passed\n";
    return 0;
}

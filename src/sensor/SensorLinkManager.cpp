#include "sensor/SensorLinkManager.hpp"

#include "util/Logger.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace sensor {
namespace {

constexpr int kPollTimeoutMs = 500;
constexpr std::size_t kMaxLineLength = 512;

speed_t toSpeed(int baudRate) {
    switch (baudRate) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        default:
            return B0;
    }
}

}  // namespace

SensorLinkManager::SensorLinkManager(
    SensorLinkConfig config,
    std::atomic<bool>& running)
    : config_(std::move(config)),
      running_(running) {
}

SensorLinkManager::~SensorLinkManager() {
    stop();
}

void SensorLinkManager::setParkingHandler(ParkingHandler handler) {
    parking_handler_ = std::move(handler);
}

void SensorLinkManager::setFireHandler(FireHandler handler) {
    fire_handler_ = std::move(handler);
}

bool SensorLinkManager::start() {
    if (config_.devicePath.empty()) {
        util::logInfo("sensor link disabled: no device path configured");
        return false;
    }

    if (toSpeed(config_.baudRate) == B0) {
        util::logError(
            "unsupported sensor UART baud rate: " +
            std::to_string(config_.baudRate));
        return false;
    }

    started_.store(true);
    worker_ = std::thread(&SensorLinkManager::run, this);

    util::logInfo(
        "sensor link started: " + config_.devicePath + " @ " +
        std::to_string(config_.baudRate));
    return true;
}

void SensorLinkManager::stop() {
    started_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

int SensorLinkManager::openDevice() const {
    // O_NONBLOCK 으로 열어야 FIFO 가 writer 를 기다리며 블로킹되지 않는다.
    const int fd = ::open(
        config_.devicePath.c_str(),
        O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    // FIFO 로 테스트할 때는 termios 설정 대상이 아니므로 건너뛴다.
    if (::isatty(fd) == 0) {
        return fd;
    }

    termios options{};
    if (::tcgetattr(fd, &options) != 0) {
        util::logError(
            std::string("tcgetattr failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }

    ::cfmakeraw(&options);
    const speed_t speed = toSpeed(config_.baudRate);
    ::cfsetispeed(&options, speed);
    ::cfsetospeed(&options, speed);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);   // 1 stop bit
    options.c_cflag &= ~static_cast<tcflag_t>(PARENB);   // no parity
    options.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);  // no flow control
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &options) != 0) {
        util::logError(
            std::string("tcsetattr failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }

    ::tcflush(fd, TCIFLUSH);
    return fd;
}

void SensorLinkManager::dispatchLine(const std::string& line) const {
    if (line.empty()) {
        return;
    }

    const auto receivedAt = std::chrono::system_clock::now();
    std::string error;

    if (SensorProtocolParser::isFireLine(line)) {
        auto message = parser_.parseFire(line, receivedAt, &error);
        if (!message) {
            util::logWarn("sensor line rejected: " + error + " | " + line);
            return;
        }
        message->transport = "uart";
        message->raw = line;
        if (fire_handler_) {
            fire_handler_(*message);
        }
        return;
    }

    auto message = parser_.parse(line, receivedAt, &error);
    if (!message) {
        util::logWarn("sensor line rejected: " + error + " | " + line);
        return;
    }
    message->transport = "uart";
    if (parking_handler_) {
        parking_handler_(*message);
    }
}

void SensorLinkManager::run() {
    std::string pending;

    while (running_.load() && started_.load()) {
        const int fd = openDevice();
        if (fd < 0) {
            util::logWarn(
                "sensor link open failed (" + config_.devicePath + "): " +
                std::strerror(errno));
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.reopenDelayMs));
            continue;
        }

        util::logInfo("sensor link connected: " + config_.devicePath);
        pending.clear();

        while (running_.load() && started_.load()) {
            pollfd descriptor{};
            descriptor.fd = fd;
            descriptor.events = POLLIN;

            const int ready = ::poll(&descriptor, 1, kPollTimeoutMs);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                util::logWarn(
                    std::string("sensor link poll failed: ") +
                    std::strerror(errno));
                break;
            }
            if (ready == 0) {
                continue;  // 타임아웃은 종료 플래그를 다시 확인할 기회다.
            }

            std::array<char, 256> buffer{};
            const ssize_t read_bytes =
                ::read(fd, buffer.data(), buffer.size());
            if (read_bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR) {
                    continue;
                }
                util::logWarn(
                    std::string("sensor link read failed: ") +
                    std::strerror(errno));
                break;
            }
            if (read_bytes == 0) {
                // FIFO writer 가 닫혔거나 UART 가 끊겼다. 재연결한다.
                break;
            }

            for (ssize_t i = 0; i < read_bytes; ++i) {
                const char ch = buffer[static_cast<std::size_t>(i)];
                if (ch == '\n' || ch == '\r') {
                    dispatchLine(pending);
                    pending.clear();
                    continue;
                }

                if (pending.size() >= kMaxLineLength) {
                    // 개행 없는 쓰레기 입력이 무한히 쌓이는 것을 막는다.
                    util::logWarn("sensor line too long, discarded");
                    pending.clear();
                    continue;
                }
                pending.push_back(ch);
            }
        }

        ::close(fd);

        if (running_.load() && started_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.reopenDelayMs));
        }
    }

    util::logInfo("sensor link stopped");
}

}  // namespace sensor

#include "device/UartDriver.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace device {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

std::string systemError(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

bool baudConstant(const int baud_rate, speed_t* speed) {
    if (speed == nullptr) return false;
    switch (baud_rate) {
    case 9600: *speed = B9600; return true;
    case 19200: *speed = B19200; return true;
    case 38400: *speed = B38400; return true;
    case 57600: *speed = B57600; return true;
    case 115200: *speed = B115200; return true;
    case 230400: *speed = B230400; return true;
#ifdef B460800
    case 460800: *speed = B460800; return true;
#endif
#ifdef B921600
    case 921600: *speed = B921600; return true;
#endif
    default: return false;
    }
}

}  // namespace

UartDriver::UartDriver(Config config) : config_(std::move(config)) {}

UartDriver::~UartDriver() { disconnect(); }

bool UartDriver::connect(std::string* error) {
    std::unique_lock descriptor_lock(descriptor_mutex_);
    if (descriptor_ >= 0) return true;
    if (config_.device_path.empty()) {
        setError(error, "UART device path is empty");
        return false;
    }

    speed_t speed{};
    if (!baudConstant(config_.baud_rate, &speed)) {
        setError(error, "unsupported UART baud rate: " +
                        std::to_string(config_.baud_rate));
        return false;
    }

    const int descriptor = ::open(config_.device_path.c_str(),
                                  O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        setError(error, systemError("cannot open UART " + config_.device_path));
        return false;
    }

    termios attributes{};
    if (tcgetattr(descriptor, &attributes) != 0) {
        setError(error, systemError("tcgetattr failed"));
        ::close(descriptor);
        return false;
    }
    cfmakeraw(&attributes);
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_cflag &= static_cast<tcflag_t>(~(CSTOPB | PARENB | CRTSCTS));
    attributes.c_cflag &= static_cast<tcflag_t>(~CSIZE);
    attributes.c_cflag |= CS8;
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 0;
    if (cfsetispeed(&attributes, speed) != 0 ||
        cfsetospeed(&attributes, speed) != 0 ||
        tcsetattr(descriptor, TCSANOW, &attributes) != 0) {
        setError(error, systemError("UART termios configuration failed"));
        ::close(descriptor);
        return false;
    }
    tcflush(descriptor, TCIOFLUSH);
    descriptor_ = descriptor;
    return true;
}

void UartDriver::disconnect() {
    std::unique_lock descriptor_lock(descriptor_mutex_);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

bool UartDriver::connected() const {
    std::shared_lock descriptor_lock(descriptor_mutex_);
    return descriptor_ >= 0;
}

int UartDriver::readSome(const std::span<std::uint8_t> output,
                         std::string* error) {
    std::shared_lock descriptor_lock(descriptor_mutex_);
    if (descriptor_ < 0) {
        setError(error, "UART is not connected");
        return -1;
    }
    if (output.empty()) return 0;

    pollfd descriptor{descriptor_, POLLIN, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, config_.read_timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result == 0) return 0;
    if (result < 0) {
        setError(error, systemError("UART poll failed"));
        return -1;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        setError(error, "UART disconnected or reported poll error");
        return -1;
    }
    if ((descriptor.revents & POLLIN) == 0) return 0;

    ssize_t count;
    do {
        count = ::read(descriptor_, output.data(), output.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (count < 0) {
        setError(error, systemError("UART read failed"));
        return -1;
    }
    return static_cast<int>(count);
}

bool UartDriver::writeAll(const std::span<const std::uint8_t> data,
                          std::string* error) {
    std::lock_guard lock(write_mutex_);
    std::shared_lock descriptor_lock(descriptor_mutex_);
    if (descriptor_ < 0) {
        setError(error, "UART is not connected");
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        pollfd descriptor{descriptor_, POLLOUT, 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, 1000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (descriptor.revents & POLLOUT) == 0) {
            setError(error, result == 0 ? "UART write timeout" :
                                        systemError("UART write poll failed"));
            return false;
        }
        ssize_t count;
        do {
            count = ::write(descriptor_, data.data() + offset,
                            data.size() - offset);
        } while (count < 0 && errno == EINTR);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) {
            setError(error, systemError("UART write failed"));
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace device

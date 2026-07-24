#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>

namespace device {

/** @brief POSIX termios 기반 UART byte transport다. */
class UartDriver {
public:
    struct Config {
        std::string device_path;
        int baud_rate{115200};
        int read_timeout_ms{250};
    };

    explicit UartDriver(Config config);
    ~UartDriver();

    UartDriver(const UartDriver&) = delete;
    UartDriver& operator=(const UartDriver&) = delete;

    /** @brief UART 장치를 non-blocking raw 8N1 모드로 연다. */
    bool connect(std::string* error = nullptr);
    /** @brief 열려 있는 descriptor를 닫는다. */
    void disconnect();
    [[nodiscard]] bool connected() const;

    /**
     * @brief poll 후 수신 가능한 byte를 읽는다.
     * @return 양수는 byte 수, 0은 timeout, -1은 오류다.
     */
    int readSome(std::span<std::uint8_t> output,
                 std::string* error = nullptr);

    /** @brief 전체 byte가 전송될 때까지 partial write를 처리한다. */
    bool writeAll(std::span<const std::uint8_t> data,
                  std::string* error = nullptr);

    [[nodiscard]] const Config& config() const { return config_; }

private:
    Config config_;
    int descriptor_{-1};
    mutable std::shared_mutex descriptor_mutex_;
    mutable std::mutex write_mutex_;
};

}  // namespace device

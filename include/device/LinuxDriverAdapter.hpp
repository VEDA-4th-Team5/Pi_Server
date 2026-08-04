#pragma once

#include <cstdint>
#include <string>

namespace device {

// /dev/parking_alert의 상태를 C++ 도메인 코드에 노출하는 값 객체다.
struct ParkingAlertState {
    std::uint32_t activeMask{0};
    std::uint32_t lastOperation{0};
    std::uint32_t lastSlot{0};
    std::uint64_t generation{0};
    std::uint64_t lastEventId{0};
};

// Linux 문자 디바이스의 open/ioctl/close를 캡슐화한다.
class LinuxDriverAdapter {
public:
    explicit LinuxDriverAdapter(std::string devicePath = "/dev/parking_alert");
    ~LinuxDriverAdapter();

    LinuxDriverAdapter(const LinuxDriverAdapter&) = delete;
    LinuxDriverAdapter& operator=(const LinuxDriverAdapter&) = delete;
    LinuxDriverAdapter(LinuxDriverAdapter&& other) noexcept;
    LinuxDriverAdapter& operator=(LinuxDriverAdapter&& other) noexcept;

    void openDevice();
    void closeDevice() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::string& devicePath() const noexcept;

    void setSlot(std::uint32_t slotIndex, std::uint64_t eventId = 0);
    void clearSlot(std::uint32_t slotIndex, std::uint64_t eventId = 0);
    void clearAll();
    [[nodiscard]] ParkingAlertState state() const;

private:
    void updateSlot(unsigned long request, std::uint16_t operation,
                    std::uint32_t slotIndex, std::uint64_t eventId);
    void requireOpen() const;

    std::string devicePath_;
    int fd_{-1};
};

}  // namespace device

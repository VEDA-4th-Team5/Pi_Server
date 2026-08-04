#include "device/LinuxDriverAdapter.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#include "parking_alert.h"

namespace device {
namespace {

std::runtime_error systemError(const std::string& operation) {
    return std::runtime_error(operation + ": " + std::strerror(errno));
}

}  // namespace

LinuxDriverAdapter::LinuxDriverAdapter(std::string devicePath)
    : devicePath_(std::move(devicePath)) {}

LinuxDriverAdapter::~LinuxDriverAdapter() {
    closeDevice();
}

LinuxDriverAdapter::LinuxDriverAdapter(LinuxDriverAdapter&& other) noexcept
    : devicePath_(std::move(other.devicePath_)), fd_(other.fd_) {
    other.fd_ = -1;
}

LinuxDriverAdapter& LinuxDriverAdapter::operator=(LinuxDriverAdapter&& other) noexcept {
    if (this != &other) {
        closeDevice();
        devicePath_ = std::move(other.devicePath_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void LinuxDriverAdapter::openDevice() {
    if (isOpen()) {
        return;
    }
    fd_ = ::open(devicePath_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw systemError("open " + devicePath_);
    }
}

void LinuxDriverAdapter::closeDevice() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LinuxDriverAdapter::isOpen() const noexcept {
    return fd_ >= 0;
}

const std::string& LinuxDriverAdapter::devicePath() const noexcept {
    return devicePath_;
}

void LinuxDriverAdapter::requireOpen() const {
    if (!isOpen()) {
        throw std::logic_error("parking alert device is not open");
    }
}

void LinuxDriverAdapter::updateSlot(unsigned long request, std::uint16_t operation,
                                    std::uint32_t slotIndex, std::uint64_t eventId) {
    requireOpen();
    if (slotIndex >= PARKING_ALERT_MAX_SLOTS) {
        throw std::out_of_range("parking alert slot index must be between 0 and 31");
    }

    parking_alert_command command{};
    command.version = PARKING_ALERT_API_VERSION;
    command.operation = operation;
    command.slot_index = slotIndex;
    command.event_id = eventId;
    if (::ioctl(fd_, request, &command) < 0) {
        throw systemError("parking alert ioctl");
    }
}

void LinuxDriverAdapter::setSlot(std::uint32_t slotIndex, std::uint64_t eventId) {
    updateSlot(PARKING_ALERT_IOC_SET_SLOT, PARKING_ALERT_OP_SET_SLOT,
               slotIndex, eventId);
}

void LinuxDriverAdapter::clearSlot(std::uint32_t slotIndex, std::uint64_t eventId) {
    updateSlot(PARKING_ALERT_IOC_CLEAR_SLOT, PARKING_ALERT_OP_CLEAR_SLOT,
               slotIndex, eventId);
}

void LinuxDriverAdapter::clearAll() {
    requireOpen();
    if (::ioctl(fd_, PARKING_ALERT_IOC_CLEAR_ALL) < 0) {
        throw systemError("parking alert clear-all ioctl");
    }
}

ParkingAlertState LinuxDriverAdapter::state() const {
    requireOpen();
    parking_alert_state raw{};
    if (::ioctl(fd_, PARKING_ALERT_IOC_GET_STATE, &raw) < 0) {
        throw systemError("parking alert get-state ioctl");
    }
    if (raw.version != PARKING_ALERT_API_VERSION) {
        throw std::runtime_error("unsupported parking alert API version");
    }

    return ParkingAlertState{
        .activeMask = raw.active_mask,
        .lastOperation = raw.last_operation,
        .lastSlot = raw.last_slot,
        .generation = raw.generation,
        .lastEventId = raw.last_event_id,
    };
}

}  // namespace device

#pragma once

#include "parking/ParkingSensorEvent.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace parking {

// Rejects duplicate or delayed packets when the transport supplies a
// monotonically increasing sequence number. Events without a sequence are
// accepted so the same domain pipeline can be used during early tests.
class ParkingSensorSequenceGuard {
public:
    [[nodiscard]] bool accept(
        const ParkingSensorEvent& event,
        std::string* reason = nullptr);

    void reset(const std::string& sensorId);
    void clear();

private:
    std::unordered_map<std::string, std::uint64_t> lastBySensor_;
};

}  // namespace parking

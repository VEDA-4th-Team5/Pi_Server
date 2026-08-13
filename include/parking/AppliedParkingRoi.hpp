#pragma once

#include "snapshot/NormalizedRoi.hpp"

#include <cstdint>
#include <string>

namespace parking {

/** ROI value actually selected when a capture/dispatch attempt starts. */
struct AppliedParkingRoi {
    std::string slotId;
    snapshot::NormalizedRoi value{};
    std::uint64_t revision{};
};

}  // namespace parking

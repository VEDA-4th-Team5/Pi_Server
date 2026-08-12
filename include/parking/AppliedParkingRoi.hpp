#pragma once

#include "snapshot/NormalizedRoi.hpp"

#include <cstdint>
#include <string>

namespace parking {

/** Revisioned ROI configuration returned to runtime callers. */
struct AppliedParkingRoi {
    std::string slotId;
    snapshot::NormalizedRoi value{};
    std::uint64_t revision{};
};

}  // namespace parking

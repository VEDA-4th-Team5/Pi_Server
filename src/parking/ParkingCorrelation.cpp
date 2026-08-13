#include "parking/ParkingCorrelation.hpp"

namespace parking {

const char* toString(const BestShotEvidenceKind kind) noexcept {
    switch (kind) {
    case BestShotEvidenceKind::Vehicle:
        return "BESTSHOT_VEHICLE";
    case BestShotEvidenceKind::Plate:
        return "BESTSHOT_PLATE";
    }
    return "BESTSHOT_UNKNOWN";
}

}  // namespace parking

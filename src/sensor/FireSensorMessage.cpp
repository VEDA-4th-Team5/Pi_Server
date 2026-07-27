#include "sensor/FireSensorMessage.hpp"

namespace sensor {

const char* toString(FireSensorState state) noexcept {
    switch (state) {
        case FireSensorState::Detected:
            return "DETECTED";
        case FireSensorState::Cleared:
            return "CLEARED";
    }
    return "CLEARED";
}

}  // namespace sensor

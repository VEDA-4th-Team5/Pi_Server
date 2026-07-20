#pragma once

#include <string>
#include <vector>

namespace parking {

struct SlotObservationBinding {
    std::string cameraId;
    std::string videoSourceToken;
    std::string ruleName;
    bool enabled{true};
    int priority{0};
};

struct ParkingSlotConfig {
    std::string slotId;
    bool enabled{false};
    std::string zoneType;
    std::string sensorId;
    std::vector<SlotObservationBinding> cameraBindings;
};

class ParkingSlotConfigLoader {
public:
    [[nodiscard]] static std::vector<ParkingSlotConfig> loadFromFile(
        const std::string& path);

    [[nodiscard]] static std::vector<ParkingSlotConfig> parse(
        const std::string& jsonText);
};

}  // namespace parking

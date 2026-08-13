#pragma once

#include <optional>
#include <string_view>

namespace sensor {

// The wire format version is explicit at the parser boundary.  A boot ID is
// metadata of BootEpochV2, not an implicit version discriminator.
enum class SensorProtocolVersion {
    LegacyV1,
    BootEpochV2
};

[[nodiscard]] inline constexpr const char* toString(
    const SensorProtocolVersion version) noexcept {
    switch (version) {
    case SensorProtocolVersion::LegacyV1:
        return "LEGACY_V1";
    case SensorProtocolVersion::BootEpochV2:
        return "BOOT_EPOCH_V2";
    }
    return "LEGACY_V1";
}

[[nodiscard]] inline std::optional<SensorProtocolVersion>
sensorProtocolVersionFromString(const std::string_view value) noexcept {
    if (value == "LEGACY_V1") return SensorProtocolVersion::LegacyV1;
    if (value == "BOOT_EPOCH_V2")
        return SensorProtocolVersion::BootEpochV2;
    return std::nullopt;
}

}  // namespace sensor

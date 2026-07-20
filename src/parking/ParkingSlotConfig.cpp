#include "parking/ParkingSlotConfig.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace parking {
namespace {

using Json = nlohmann::json;

std::string readTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open parking slot config: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string requiredString(
    const Json& object,
    const char* key,
    const std::string& context) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        throw std::runtime_error(
            context + ": field '" + key + "' must be a string");
    }

    const auto value = it->get<std::string>();
    if (value.empty()) {
        throw std::runtime_error(
            context + ": field '" + key + "' must not be empty");
    }

    return value;
}

std::string optionalString(
    const Json& object,
    const char* key,
    const std::string& fallback,
    const std::string& context) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_string()) {
        throw std::runtime_error(
            context + ": field '" + key + "' must be a string");
    }
    return it->get<std::string>();
}

bool optionalBool(
    const Json& object,
    const char* key,
    bool fallback,
    const std::string& context) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        throw std::runtime_error(
            context + ": field '" + key + "' must be a boolean");
    }
    return it->get<bool>();
}

int optionalInt(
    const Json& object,
    const char* key,
    int fallback,
    const std::string& context) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_number_integer()) {
        throw std::runtime_error(
            context + ": field '" + key + "' must be an integer");
    }
    return it->get<int>();
}

SlotObservationBinding parseBinding(
    const Json& object,
    const std::string& context) {
    if (!object.is_object()) {
        throw std::runtime_error(context + " must be an object");
    }

    SlotObservationBinding binding;
    binding.cameraId = requiredString(object, "camera_id", context);
    binding.videoSourceToken =
        requiredString(object, "video_source_token", context);
    binding.ruleName = requiredString(object, "rule_name", context);
    binding.enabled = optionalBool(object, "enabled", true, context);
    binding.priority = optionalInt(object, "priority", 0, context);
    return binding;
}

ParkingSlotConfig parseSlot(const Json& object, std::size_t index) {
    const std::string context = "slots[" + std::to_string(index) + "]";
    if (!object.is_object()) {
        throw std::runtime_error(context + " must be an object");
    }

    ParkingSlotConfig config;
    config.slotId = requiredString(object, "slot_id", context);
    config.enabled = optionalBool(object, "enabled", false, context);
    config.zoneType =
        optionalString(object, "zone_type", "unassigned", context);
    config.sensorId =
        optionalString(object, "sensor_id", "", context);

    const auto bindingsIt = object.find("camera_bindings");
    if (bindingsIt != object.end() && !bindingsIt->is_null()) {
        if (!bindingsIt->is_array()) {
            throw std::runtime_error(
                context + ": field 'camera_bindings' must be an array");
        }

        std::unordered_set<std::string> bindingKeys;
        for (std::size_t bindingIndex = 0;
             bindingIndex < bindingsIt->size();
             ++bindingIndex) {
            const std::string bindingContext =
                context + ".camera_bindings[" +
                std::to_string(bindingIndex) + "]";

            auto binding =
                parseBinding((*bindingsIt)[bindingIndex], bindingContext);

            const std::string key =
                binding.cameraId + "\n" +
                binding.videoSourceToken + "\n" +
                binding.ruleName;

            if (!bindingKeys.insert(key).second) {
                throw std::runtime_error(
                    bindingContext +
                    ": duplicate camera/token/rule binding");
            }

            config.cameraBindings.push_back(std::move(binding));
        }
    }

    if (config.enabled && config.sensorId.empty()) {
        throw std::runtime_error(
            context + ": enabled slot requires non-empty sensor_id");
    }

    return config;
}

}  // namespace

std::vector<ParkingSlotConfig> ParkingSlotConfigLoader::loadFromFile(
    const std::string& path) {
    return parse(readTextFile(path));
}

std::vector<ParkingSlotConfig> ParkingSlotConfigLoader::parse(
    const std::string& jsonText) {
    if (jsonText.empty()) {
        throw std::invalid_argument("parking slot config is empty");
    }

    const Json root = Json::parse(jsonText);
    if (!root.is_object()) {
        throw std::runtime_error(
            "parking slot config root must be an object");
    }

    const auto slotsIt = root.find("slots");
    if (slotsIt == root.end() || !slotsIt->is_array()) {
        throw std::runtime_error(
            "parking slot config field 'slots' must be an array");
    }
    if (slotsIt->empty()) {
        throw std::runtime_error(
            "parking slot config must contain at least one slot");
    }

    std::vector<ParkingSlotConfig> configs;
    configs.reserve(slotsIt->size());

    std::unordered_set<std::string> slotIds;
    std::unordered_set<std::string> enabledSensorIds;

    for (std::size_t index = 0; index < slotsIt->size(); ++index) {
        auto config = parseSlot((*slotsIt)[index], index);

        if (!slotIds.insert(config.slotId).second) {
            throw std::runtime_error(
                "duplicate slot_id: " + config.slotId);
        }

        if (config.enabled &&
            !enabledSensorIds.insert(config.sensorId).second) {
            throw std::runtime_error(
                "duplicate sensor_id among enabled slots: " +
                config.sensorId);
        }

        configs.push_back(std::move(config));
    }

    return configs;
}

}  // namespace parking

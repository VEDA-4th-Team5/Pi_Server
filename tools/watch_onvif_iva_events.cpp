#include "camera/OnvifIvaEventSource.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

std::atomic_bool running{true};

void stopHandler(int) { running.store(false); }

std::string environmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::set<int> parseChannels(const std::string& value) {
    std::set<int> channels;
    std::istringstream input(value);
    std::string part;
    while (std::getline(input, part, ',')) {
        const int channel = std::stoi(part);
        if (channel < 1 || channel > 999)
            throw std::invalid_argument("channel must be between 1 and 999");
        channels.insert(channel);
    }
    if (channels.empty()) throw std::invalid_argument("no channels selected");
    return channels;
}

std::string channelList(const std::set<int>& channels) {
    std::ostringstream output;
    bool first = true;
    for (const int channel : channels) {
        if (!first) output << ',';
        output << "CH" << channel;
        first = false;
    }
    return output.str();
}

void usage(const char* program) {
    std::cout
        << "Usage: " << program
        << " [--url ONVIF_EVENT_URL] [--channels 1,3] [--show-initialized]\n"
        << "Credentials: CAMERA_API_USERNAME and CAMERA_API_PASSWORD\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        std::string endpoint = environmentValue("CAMERA_ONVIF_EVENT_URL");
        std::set<int> channels{1, 3};
        bool showInitialized{};
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") {
                usage(argv[0]);
                return 0;
            }
            if (argument == "--show-initialized") {
                showInitialized = true;
                continue;
            }
            if (index + 1 >= argc)
                throw std::invalid_argument("missing value for " + argument);
            const std::string value = argv[++index];
            if (argument == "--url") endpoint = value;
            else if (argument == "--channels") channels = parseChannels(value);
            else throw std::invalid_argument("unknown option: " + argument);
        }

        if (endpoint.empty()) {
            const std::string base = environmentValue("CAMERA_OPEN_API_BASE");
            if (base.empty())
                throw std::invalid_argument(
                    "CAMERA_ONVIF_EVENT_URL or CAMERA_OPEN_API_BASE is required");
            endpoint = camera::OnvifIvaEventSource::deriveEventEndpoint(base);
        }

        camera::OnvifIvaEventSource::Config config;
        config.endpoint = endpoint;
        config.username = environmentValue("CAMERA_API_USERNAME");
        config.password = environmentValue("CAMERA_API_PASSWORD");
        config.deliverInitialized = showInitialized;
        for (const int channel : channels) {
            config.acceptedVideoSourceTokens.insert(
                "vs-" + std::to_string(channel - 1));
        }

        camera::OnvifIvaEventSource source(
            std::move(config),
            [](const camera::OnvifIvaEvent& event) {
                const auto separator = event.videoSourceToken.find_last_of('-');
                const int channel = separator == std::string::npos
                    ? 0
                    : std::stoi(event.videoSourceToken.substr(separator + 1)) + 1;
                std::cout
                    << "[ONVIF_IVA] utc="
                    << (event.utcTime.empty() ? "-" : event.utcTime)
                    << " channel=CH" << channel
                    << " token=" << event.videoSourceToken
                    << " rule=" << (event.ruleName.empty() ? "-" : event.ruleName)
                    << " object_id="
                    << (event.objectId.empty() ? "-" : event.objectId)
                    << " action=" << (event.action.empty() ? "-" : event.action)
                    << " state=" << (event.state.empty() ? "-" : event.state)
                    << " operation="
                    << (event.operation.empty() ? "-" : event.operation)
                    << std::endl;
                return true;
            });

        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        std::cout << "Watching camera ONVIF IVA events for "
                  << channelList(channels) << ". Ctrl+C to stop.\n";
        if (!source.start()) return 3;
        while (running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        source.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "watch-onvif-iva-events: " << error.what() << '\n';
        return 2;
    }
}

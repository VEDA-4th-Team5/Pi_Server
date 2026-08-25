#include "camera/CameraSnapshotApiClient.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void sendJson(httplib::Response& response, const nlohmann::json& body,
              const int status = 200) {
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

}  // namespace

int main() {
    httplib::Server server;
    std::atomic<int> generateCalls{};
    std::atomic<int> startServerCalls{};

    server.Post("/opensdk/test/startserver",
        [&startServerCalls](const httplib::Request& request,
                            httplib::Response& response) {
            const auto body = nlohmann::json::parse(request.body);
            require(body.at("port") == 18080, "startserver port mismatch");
            startServerCalls.fetch_add(1);
            sendJson(response, {{"success", true}, {"http_status", 202}}, 202);
        });
    server.Get("/opensdk/test/channels",
        [](const httplib::Request&, httplib::Response& response) {
            sendJson(response, {
                {"success", true}, {"channel_count", 2},
                {"default_channel", 0},
                {"channels", nlohmann::json::array({
                    {{"id", 0}, {"label", "CH 1"}},
                    {{"id", 1}, {"label", "CH 2"}}})}});
        });
    server.Get("/opensdk/test/filters",
        [](const httplib::Request&, httplib::Response& response) {
            sendJson(response, {
                {"success", true},
                {"filters", nlohmann::json::array({
                    {{"id", "fast_bilateral"}},
                    {{"id", "stretch_1_99"}}})}});
        });
    server.Post("/opensdk/test/images/generate",
        [&generateCalls](const httplib::Request& request,
                         httplib::Response& response) {
            const auto body = nlohmann::json::parse(request.body);
            require(body.at("channel") == 0, "generate channel mismatch");
            require(body.at("outputs").size() == 2,
                    "original/enhanced outputs missing");
            const int call = generateCalls.fetch_add(1);
            if (call == 0) {
                sendJson(response, {
                    {"success", false}, {"error_code", "PROCESSING_BUSY"},
                    {"message", "busy"}}, 503);
                return;
            }
            if (call == 1) {
                sendJson(response, {
                    {"success", false},
                    {"error_code", "IMAGE_SERVER_NOT_STARTED"},
                    {"message", "image server is stopped"}}, 409);
                return;
            }
            sendJson(response, {
                {"success", true}, {"run_id", "img-test-1"}, {"channel", 0},
                {"detected_environment", "extreme_low_light"},
                {"auto_filter", "fast_bilateral"},
                {"results", nlohmann::json::array({
                    {{"id", "original"}, {"jpeg_bytes", 4},
                     {"image_path", "/images/result/jpg?run_id=img-test-1&result=original"}},
                    {{"id", "enhanced"}, {"jpeg_bytes", 5},
                     {"image_path", "/images/result/jpg?run_id=img-test-1&result=enhanced"}}})}});
        });
    server.Get("/images/result/jpg",
        [](const httplib::Request& request, httplib::Response& response) {
            require(request.get_param_value("run_id") == "img-test-1",
                    "run_id mismatch");
            const std::string result = request.get_param_value("result");
            const std::string bytes = result == "original"
                ? std::string{"\xFF\xD8\xFF\xD9", 4}
                : std::string{"\xFF\xD8\xFF\x01\xD9", 5};
            response.set_content(bytes, "image/jpeg");
        });

    const int port = server.bind_to_any_port("127.0.0.1");
    require(port > 0, "mock HTTP server bind failed");
    std::thread serverThread([&server] { server.listen_after_bind(); });

    try {
        camera::CameraSnapshotApiConfig config;
        config.openApiBase = "http://127.0.0.1:" + std::to_string(port) +
                             "/opensdk/test";
        config.imageBase = "http://127.0.0.1:" + std::to_string(port);
        config.imageServerPort = 18080;
        config.connectTimeoutMs = 500;
        config.requestTimeoutMs = 1000;
        config.jpegTimeoutMs = 1000;
        config.maxRetries = 2;
        config.retryDelayMs = 5;
        camera::CameraSnapshotApiClient client(config);

        require(client.initialize(), "client initialization failed: " +
                client.lastError());
        require(client.channels().size() == 2 && client.filters().size() == 2,
                "discovery results mismatch");

        camera::CameraGeneratedImages images;
        require(client.generate(0, images), "generate failed: " +
                client.lastError());
        require(generateCalls.load() == 3,
                "retryable generate failures were not retried");
        require(startServerCalls.load() == 2,
                "stopped image server was not restarted before retry");
        require(images.runId == "img-test-1" && images.channel == 0,
                "run metadata mismatch");
        require(images.autoFilter == "fast_bilateral" &&
                images.detectedEnvironment == "extreme_low_light",
                "CV metadata mismatch");
        require(images.originalJpeg.size() == 4 &&
                images.enhancedJpeg.size() == 5,
                "JPEG downloads mismatch");
        require(!client.generate(3, images) &&
                client.lastError().find("not advertised") != std::string::npos,
                "invalid channel was not rejected locally");

        server.stop();
        serverThread.join();
        std::cout << "[PASS] camera snapshot API discovery/generate/"
                     "server-restart/retry/JPEG\n";
        return EXIT_SUCCESS;
    } catch (...) {
        server.stop();
        if (serverThread.joinable()) serverThread.join();
        throw;
    }
}

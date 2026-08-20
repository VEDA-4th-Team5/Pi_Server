#pragma once

#include "app/CallbackLeaseGate.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace database { class EventDatabase; }
namespace auth { class AuthService; }
namespace httplib { class Server; }
namespace settings { class OverstayThresholdService; }
namespace settings { class ParkingRoiSettingsService; }

namespace http {

enum class HttpServerState { Constructed, Running, Quiescing, Stopped };

struct ServerConfig {
    std::string listen_address{"0.0.0.0"};
    int port{8080};
    std::string tls_certificate_path;
    std::string tls_private_key_path;
    std::string data_root{"data"};
    std::size_t max_image_bytes{10U * 1024U * 1024U};
    bool require_tls{true};
};

class ParkingHttpServer {
public:
    ParkingHttpServer(database::EventDatabase& database,
                      auth::AuthService& auth_service, ServerConfig config,
                      settings::OverstayThresholdService* overstay_settings = nullptr,
                      settings::ParkingRoiSettingsService* roi_settings = nullptr);
    ~ParkingHttpServer();
    ParkingHttpServer(const ParkingHttpServer&) = delete;
    ParkingHttpServer& operator=(const ParkingHttpServer&) = delete;
    bool start();
    void closeIngress() noexcept;
    [[nodiscard]] bool stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;
    bool usesTls() const;
    [[nodiscard]] HttpServerState state() const noexcept;

private:
    void closeIngressLocked() noexcept;
    void stopListenerLocked() noexcept;
    void registerRoutes();
    database::EventDatabase& database_;
    auth::AuthService& auth_service_;
    settings::OverstayThresholdService* overstay_settings_{};
    settings::ParkingRoiSettingsService* roi_settings_{};
    ServerConfig config_;
    std::unique_ptr<httplib::Server> server_;
    std::thread worker_;
    app::CallbackLeaseGate request_gate_;
    mutable std::mutex lifecycle_mutex_;
    std::mutex stop_mutex_;
    HttpServerState state_{HttpServerState::Constructed};
    bool uses_tls_{false};
};

}

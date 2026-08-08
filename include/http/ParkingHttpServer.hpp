#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <thread>

namespace database { class EventDatabase; }
namespace httplib { class Server; }
namespace settings { class OverstayThresholdService; }
namespace settings { class ParkingRoiSettingsService; }

namespace http {

struct ServerConfig {
    std::string listen_address{"0.0.0.0"};
    int port{8080};
    std::string tls_certificate_path;
    std::string tls_private_key_path;
    std::string data_root{"data"};
    std::size_t max_image_bytes{10U * 1024U * 1024U};
};

class ParkingHttpServer {
public:
    ParkingHttpServer(database::EventDatabase& database, ServerConfig config,
                      settings::OverstayThresholdService* overstay_settings = nullptr,
                      settings::ParkingRoiSettingsService* roi_settings = nullptr);
    ~ParkingHttpServer();
    ParkingHttpServer(const ParkingHttpServer&) = delete;
    ParkingHttpServer& operator=(const ParkingHttpServer&) = delete;
    bool start();
    void stop();
    bool usesTls() const;

private:
    void registerRoutes();
    database::EventDatabase& database_;
    settings::OverstayThresholdService* overstay_settings_{};
    settings::ParkingRoiSettingsService* roi_settings_{};
    ServerConfig config_;
    std::unique_ptr<httplib::Server> server_;
    std::thread worker_;
    bool uses_tls_{false};
};

}

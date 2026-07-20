#include "http/ParkingHttpServer.hpp"

#include "database/EventDatabase.hpp"
#include "util/Logger.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
void sendJson(httplib::Response& response, const json& body, int status = 200) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}
void sendError(httplib::Response& response, int status, const std::string& code,
               const std::string& message) {
    sendJson(response, {{"error", code}, {"message", message}}, status);
}
json optionalText(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}
json evStatus(int is_ev) {
    if (is_ev < 0) return nullptr;
    return is_ev == 1 ? json("EV") : json("NON_EV");
}
json slotJson(const database::ParkingSlotView& slot) {
    json value = {{"slot_id", slot.slot_id}, {"slot_type", slot.slot_type},
                  {"parking_status", slot.parking_status},
                  {"sensor_type", optionalText(slot.sensor_type)},
                  {"updated_at", optionalText(slot.updated_at)},
                  {"active_session", nullptr}};
    if (slot.session_id >= 0) {
        value["active_session"] = {{"session_id", slot.session_id},
            {"plate_number", optionalText(slot.plate_number)},
            {"ev_status", evStatus(slot.is_ev)},
            {"entry_time", optionalText(slot.entry_time)}};
    }
    return value;
}
json imageJson(const database::ImageView& image) {
    const std::string base = "/api/v1/images/" + std::to_string(image.image_id);
    return {{"image_id", image.image_id},
        {"session_id", image.session_id < 0 ? json(nullptr) : json(image.session_id)},
        {"original_url", image.original_path.empty() ? json(nullptr) : json(base + "/original")},
        {"enhanced_url", image.enhanced_path.empty() ? json(nullptr) : json(base + "/enhanced")},
        {"enhancement_type", optionalText(image.enhancement_type)},
        {"ocr_result", optionalText(image.ocr_result)},
        {"captured_at", optionalText(image.captured_at)}};
}
bool isInside(const fs::path& child, const fs::path& parent) {
    auto child_it = child.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end();
         ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) return false;
    }
    return true;
}
std::string mimeType(const fs::path& path) {
    const std::string extension = path.extension().string();
    if (extension == ".png" || extension == ".PNG") return "image/png";
    if (extension == ".webp" || extension == ".WEBP") return "image/webp";
    return "image/jpeg";
}
bool parsePositiveId(const std::string& value, int& output) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (parsed < 0 || consumed != value.size()) return false;
        output = parsed;
        return true;
    } catch (...) { return false; }
}
}

namespace http {
ParkingHttpServer::ParkingHttpServer(database::EventDatabase& database,
                                     ServerConfig config)
    : database_(database), config_(std::move(config)) {}
ParkingHttpServer::~ParkingHttpServer() { stop(); }

bool ParkingHttpServer::start() {
    if (server_) return true;
    const bool cert_set = !config_.tls_certificate_path.empty();
    const bool key_set = !config_.tls_private_key_path.empty();
    if (cert_set != key_set) {
        util::logError("HTTP API TLS certificate/key must be configured together");
        return false;
    }
    if (cert_set) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        auto tls_server = std::make_unique<httplib::SSLServer>(
            config_.tls_certificate_path.c_str(), config_.tls_private_key_path.c_str());
        if (!tls_server->is_valid()) {
            util::logError("HTTPS server certificate or private key is invalid");
            return false;
        }
        server_ = std::move(tls_server);
        uses_tls_ = true;
#else
        util::logError("HTTPS requested but cpp-httplib has no OpenSSL support");
        return false;
#endif
    } else {
        server_ = std::make_unique<httplib::Server>();
        uses_tls_ = false;
    }
    registerRoutes();
    if (!server_->bind_to_port(config_.listen_address, config_.port)) {
        util::logError("HTTP API bind failed: " + config_.listen_address + ":" +
                       std::to_string(config_.port));
        server_.reset();
        return false;
    }
    worker_ = std::thread([this] {
        if (!server_->listen_after_bind())
            util::logError("HTTP API listener stopped with an error");
    });
    util::logInfo(std::string(uses_tls_ ? "HTTPS" : "HTTP") +
                  " API listening on " + config_.listen_address + ":" +
                  std::to_string(config_.port));
    return true;
}
void ParkingHttpServer::stop() {
    if (server_) server_->stop();
    if (worker_.joinable()) worker_.join();
    server_.reset();
}
bool ParkingHttpServer::usesTls() const { return uses_tls_; }

void ParkingHttpServer::registerRoutes() {
    server_->Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"status", "ok"}, {"service", "pi-server"}});
    });
    server_->Get("/api/v1/parking-slots", [this](const httplib::Request&, httplib::Response& res) {
        std::vector<database::ParkingSlotView> slots;
        if (!database_.listParkingSlots(slots)) {
            sendError(res, 503, "DATABASE_UNAVAILABLE", "주차면을 조회할 수 없습니다."); return;
        }
        json items = json::array();
        for (const auto& slot : slots) items.push_back(slotJson(slot));
        sendJson(res, {{"items", items}, {"count", items.size()}});
    });
    server_->Get(R"(/api/v1/parking-slots/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        database::ParkingSlotView slot;
        if (!database_.getParkingSlot(req.matches[1], slot)) {
            sendError(res, 404, "SLOT_NOT_FOUND", "주차면을 찾을 수 없습니다."); return;
        }
        sendJson(res, slotJson(slot));
    });
    server_->Get("/api/v1/parking-sessions/active", [this](const httplib::Request&, httplib::Response& res) {
        std::vector<database::ParkingSlotView> slots;
        if (!database_.listParkingSlots(slots)) {
            sendError(res, 503, "DATABASE_UNAVAILABLE", "활성 세션을 조회할 수 없습니다."); return;
        }
        json items = json::array();
        for (const auto& slot : slots) if (slot.session_id >= 0) items.push_back(slotJson(slot));
        sendJson(res, {{"items", items}, {"count", items.size()}});
    });
    server_->Get(R"(/api/v1/parking-sessions/([0-9]+)/images)", [this](const httplib::Request& req, httplib::Response& res) {
        int session_id;
        if (!parsePositiveId(req.matches[1], session_id)) {
            sendError(res, 400, "INVALID_SESSION_ID", "session_id 형식이 잘못되었습니다."); return;
        }
        std::vector<database::ImageView> images;
        if (!database_.listSessionImages(session_id, images)) {
            sendError(res, 503, "DATABASE_UNAVAILABLE", "이미지를 조회할 수 없습니다."); return;
        }
        json items = json::array();
        for (const auto& image : images) items.push_back(imageJson(image));
        sendJson(res, {{"session_id", session_id}, {"items", items}, {"count", items.size()}});
    });
    server_->Get(R"(/api/v1/images/([0-9]+)/(original|enhanced))", [this](const httplib::Request& req, httplib::Response& res) {
        int image_id;
        if (!parsePositiveId(req.matches[1], image_id)) {
            sendError(res, 400, "INVALID_IMAGE_ID", "image_id 형식이 잘못되었습니다."); return;
        }
        database::ImageView image;
        if (!database_.getImage(image_id, image)) {
            sendError(res, 404, "IMAGE_NOT_FOUND", "이미지 기록을 찾을 수 없습니다."); return;
        }
        const std::string stored_path = req.matches[2] == "enhanced" ? image.enhanced_path : image.original_path;
        if (stored_path.empty()) {
            sendError(res, 404, "IMAGE_VARIANT_NOT_FOUND", "요청한 이미지 종류가 없습니다."); return;
        }
        std::error_code error;
        const fs::path root = fs::weakly_canonical(config_.data_root, error);
        fs::path candidate(stored_path);
        if (candidate.is_relative()) candidate = fs::current_path() / candidate;
        candidate = fs::weakly_canonical(candidate, error);
        if (error || !isInside(candidate, root) || !fs::is_regular_file(candidate)) {
            sendError(res, 404, "IMAGE_FILE_NOT_FOUND", "이미지 파일이 없거나 허용된 data 경로 밖입니다."); return;
        }
        const auto size = fs::file_size(candidate, error);
        if (error || size > config_.max_image_bytes) {
            sendError(res, 413, "IMAGE_TOO_LARGE", "이미지 크기 제한을 초과했습니다."); return;
        }
        std::ifstream input(candidate, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            sendError(res, 500, "IMAGE_READ_FAILED", "이미지 파일을 읽지 못했습니다."); return;
        }
        res.set_content(std::move(body), mimeType(candidate));
    });
    server_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404) sendError(res, 404, "ENDPOINT_NOT_FOUND", "API 경로를 찾을 수 없습니다.");
    });
}
}

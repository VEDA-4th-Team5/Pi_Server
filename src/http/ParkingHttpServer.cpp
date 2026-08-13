#include "http/ParkingHttpServer.hpp"

#include "database/EventDatabase.hpp"
#include "settings/OverstayThresholdService.hpp"
#include "settings/ParkingRoiSettingsService.hpp"
#include "util/Logger.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>
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
json roiJson(const snapshot::NormalizedRoi& roi);
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
    json value = {{"image_id", image.image_id},
        {"session_id", image.session_id < 0 ? json(nullptr) : json(image.session_id)},
        {"original_url", image.original_path.empty() ? json(nullptr) : json(base + "/original")},
        {"enhanced_url", image.enhanced_path.empty() ? json(nullptr) : json(base + "/enhanced")},
        {"enhancement_type", optionalText(image.enhancement_type)},
        {"evidence_reason", optionalText(image.evidence_reason)},
        {"ocr_result", optionalText(image.ocr_result)},
        {"captured_at", optionalText(image.captured_at)},
        {"roi", nullptr}, {"roi_revision", nullptr}};
    if (image.applied_roi && image.roi_revision > 0) {
        value["roi"] = roiJson(*image.applied_roi);
        value["roi_revision"] = image.roi_revision;
    }
    return value;
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
std::string normalizedSlotId(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}
json roiJson(const snapshot::NormalizedRoi& roi) {
    return {{"x", roi.x}, {"y", roi.y}, {"width", roi.width},
            {"height", roi.height}};
}
}

namespace http {
ParkingHttpServer::ParkingHttpServer(database::EventDatabase& database,
                                     ServerConfig config,
                                     settings::OverstayThresholdService* overstay_settings,
                                     settings::ParkingRoiSettingsService* roi_settings)
    : database_(database), overstay_settings_(overstay_settings),
      roi_settings_(roi_settings),
      config_(std::move(config)) {}
ParkingHttpServer::~ParkingHttpServer() {
    if (!stop()) std::terminate();
}

bool ParkingHttpServer::start() {
    std::lock_guard lifecycleLock(lifecycle_mutex_);
    if (state_ == HttpServerState::Running) return true;
    if (state_ != HttpServerState::Constructed) return false;
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
    if (!server_->bind_to_port(config_.listen_address.c_str(), config_.port)) {
        util::logError("HTTP API bind failed: " + config_.listen_address + ":" +
                       std::to_string(config_.port));
        server_.reset();
        return false;
    }
    try {
        state_ = HttpServerState::Running;
        worker_ = std::thread([this] {
            if (!server_->listen_after_bind()) {
                std::lock_guard lock(lifecycle_mutex_);
                if (state_ == HttpServerState::Running)
                    util::logError("HTTP API listener stopped with an error");
            }
        });
    } catch (const std::exception& error) {
        state_ = HttpServerState::Constructed;
        server_->stop();
        server_.reset();
        util::logError("HTTP API worker start failed: " +
                       std::string(error.what()));
        return false;
    }
    util::logInfo(std::string(uses_tls_ ? "HTTPS" : "HTTP") +
                  " API listening on " + config_.listen_address + ":" +
                  std::to_string(config_.port));
    return true;
}
void ParkingHttpServer::closeIngress() noexcept {
    std::lock_guard stopLock(stop_mutex_);
    closeIngressLocked();
}

void ParkingHttpServer::closeIngressLocked() noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_ == HttpServerState::Stopped) return;
        request_gate_.close();
        state_ = HttpServerState::Quiescing;
    }
}

void ParkingHttpServer::stopListenerLocked() noexcept {
    httplib::Server* server{};
    {
        std::lock_guard lock(lifecycle_mutex_);
        server = server_.get();
    }
    if (server) server->stop();
}

bool ParkingHttpServer::stop(
    const std::chrono::milliseconds timeout) noexcept {
    std::lock_guard stopLock(stop_mutex_);
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_ == HttpServerState::Stopped) return true;
    }
    closeIngressLocked();
    stopListenerLocked();
    if (!request_gate_.waitForDrainedFor(timeout)) {
        util::logError("HTTP request drain timed out: active=" +
                       std::to_string(request_gate_.activeCount()));
        return false;
    }
    try {
        if (worker_.joinable()) worker_.join();
    } catch (const std::exception& error) {
        util::logError("HTTP listener join failed: " +
                       std::string(error.what()));
        return false;
    }
    {
        std::lock_guard lock(lifecycle_mutex_);
        server_.reset();
        state_ = HttpServerState::Stopped;
    }
    return true;
}
bool ParkingHttpServer::usesTls() const {
    std::lock_guard lock(lifecycle_mutex_);
    return uses_tls_;
}

HttpServerState ParkingHttpServer::state() const noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    return state_;
}

void ParkingHttpServer::registerRoutes() {
    const auto guarded = [this](auto handler) {
        return [this, handler = std::move(handler)](
                   const httplib::Request& request,
                   httplib::Response& response) mutable {
            auto lease = request_gate_.tryAcquire();
            if (!lease) {
                sendError(response, 503, "SERVER_QUIESCING",
                          "서버가 종료 중이어서 요청을 처리할 수 없습니다.");
                return;
            }
            handler(request, response);
        };
    };

    server_->Get("/api/v1/health", guarded([](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"status", "ok"}, {"service", "pi-server"}});
    }));
    if (overstay_settings_ != nullptr) {
        const auto get_threshold = guarded([this](const httplib::Request&,
                                          httplib::Response& res) {
            const auto status = overstay_settings_->status();
            sendJson(res, {{"thresholdSeconds", status.thresholdSeconds},
                           {"effectiveSeconds", status.thresholdSeconds},
                           {"thresholdMinutes", status.thresholdSeconds / 60.0},
                           {"appliedRevision", status.appliedRevision},
                           {"runtimeApplied", status.runtimeApplied},
                           {"runtimeHealthy", status.runtimeHealthy},
                           {"applyPolicy", "ACTIVE_AND_NEW_SESSIONS"}});
        });
        const auto put_threshold = guarded([this](const httplib::Request& req,
                                          httplib::Response& res) {
            json body;
            try {
                body = json::parse(req.body);
            } catch (...) {
                util::logWarn("Overstay threshold PUT rejected: malformed JSON");
                sendJson(res, {{"success", false},
                               {"error", "request body must be valid JSON"}}, 400);
                return;
            }
            if (!body.is_object() || !body.contains("thresholdSeconds") ||
                !body["thresholdSeconds"].is_number_integer()) {
                util::logWarn("Overstay threshold PUT rejected: integer required");
                sendJson(res, {{"success", false},
                               {"error", "thresholdSeconds must be an integer"}}, 400);
                return;
            }
            std::int64_t raw{};
            try {
                raw = body["thresholdSeconds"].get<std::int64_t>();
            } catch (...) {
                util::logWarn("Overstay threshold PUT rejected: integer overflow");
                sendJson(res, {{"success", false},
                               {"error", "thresholdSeconds must be an integer"}}, 400);
                return;
            }
            if (raw < settings::OverstayThresholdService::kMinimumSeconds ||
                raw > settings::OverstayThresholdService::kMaximumSeconds) {
                util::logWarn("Overstay threshold PUT rejected: out of range");
                sendJson(res, {{"success", false},
                               {"error", "thresholdSeconds must be between 60 and 86400"}}, 400);
                return;
            }
            const auto result = overstay_settings_->update(static_cast<int>(raw));
            if (!result.success) {
                sendJson(res, {{"success", false},
                               {"requestedSeconds", raw},
                               {"effectiveSeconds", result.thresholdSeconds},
                               {"thresholdSeconds", result.thresholdSeconds},
                               {"appliedRevision", result.appliedRevision},
                               {"runtimeApplied", result.runtimeApplied},
                               {"runtimeHealthy", result.runtimeHealthy},
                               {"applyPolicy", "ACTIVE_AND_NEW_SESSIONS"},
                               {"error", result.error}}, 503);
                return;
            }
            sendJson(res, {{"success", true},
                           {"requestedSeconds", raw},
                           {"effectiveSeconds", result.thresholdSeconds},
                           {"thresholdSeconds", result.thresholdSeconds},
                           {"thresholdMinutes", result.thresholdSeconds / 60.0},
                           {"appliedRevision", result.appliedRevision},
                           {"runtimeApplied", result.runtimeApplied},
                           {"runtimeHealthy", result.runtimeHealthy},
                           {"applyPolicy", "ACTIVE_AND_NEW_SESSIONS"}});
        });
        server_->Get("/api/v1/settings/overstay-threshold", get_threshold);
        server_->Put("/api/v1/settings/overstay-threshold", put_threshold);
        // 초기 요청서 경로도 유지해 Qt 배포 버전 간 호환성을 보장한다.
        server_->Get("/api/settings/overstay-threshold", get_threshold);
        server_->Put("/api/settings/overstay-threshold", put_threshold);
    }
    if (roi_settings_ != nullptr) {
        server_->Get("/api/v1/settings/parking-slots/roi",
            guarded([this](const httplib::Request&, httplib::Response& res) {
                json items = json::array();
                for (const auto& setting : roi_settings_->list()) {
                    items.push_back({{"slotId", setting.slotId},
                                     {"roi", roiJson(setting.roi)},
                                     {"revision", setting.revision}});
                }
                sendJson(res, {{"items", items}, {"count", items.size()}});
            }));
        server_->Get(
            R"(/api/v1/settings/parking-slots/([^/]+)/roi)",
            guarded([this](const httplib::Request& req, httplib::Response& res) {
                const std::string slot_id =
                    normalizedSlotId(req.matches[1].str());
                const auto roi = roi_settings_->resolveForUse(slot_id);
                if (!roi) {
                    sendError(res, 404, "SLOT_NOT_FOUND",
                              "ROI가 설정된 주차면을 찾을 수 없습니다.");
                    return;
                }
                sendJson(res, {{"slotId", slot_id},
                               {"roi", roiJson(roi->value)},
                               {"revision", roi->revision}});
            }));
        server_->Put(
            R"(/api/v1/settings/parking-slots/([^/]+)/roi)",
            guarded([this](const httplib::Request& req, httplib::Response& res) {
                json body;
                try {
                    body = json::parse(req.body);
                } catch (...) {
                    sendJson(res, {{"success", false},
                                   {"error", "request body must be valid JSON"}},
                             400);
                    return;
                }
                for (const char* field : {"x", "y", "width", "height"}) {
                    if (!body.is_object() || !body.contains(field) ||
                        !body[field].is_number()) {
                        sendJson(res, {{"success", false},
                                       {"error", std::string(field) +
                                           " must be a number"}}, 400);
                        return;
                    }
                }
                snapshot::NormalizedRoi roi{};
                try {
                    roi = {body["x"].get<double>(), body["y"].get<double>(),
                           body["width"].get<double>(),
                           body["height"].get<double>()};
                } catch (...) {
                    sendJson(res, {{"success", false},
                                   {"error", "ROI values are invalid"}}, 400);
                    return;
                }
                const std::string slot_id =
                    normalizedSlotId(req.matches[1].str());
                const auto result = roi_settings_->update(slot_id, roi);
                if (!result.slotFound) {
                    sendJson(res, {{"success", false},
                                   {"error", result.error}}, 404);
                    return;
                }
                if (!result.success) {
                    const int status = settings::ParkingRoiSettingsService::isValid(roi)
                        ? 500 : 400;
                    sendJson(res, {{"success", false},
                                   {"error", result.error}}, status);
                    return;
                }
                sendJson(res, {{"success", true}, {"slotId", slot_id},
                               {"appliedImmediately", true},
                               {"revision", result.revision},
                               {"roi", roiJson(result.roi)}});
            }));
    }
    server_->Get("/api/v1/parking-slots", guarded([this](const httplib::Request&, httplib::Response& res) {
        std::vector<database::ParkingSlotView> slots;
        if (!database_.listParkingSlots(slots)) {
            sendError(res, 503, "DATABASE_UNAVAILABLE", "주차면을 조회할 수 없습니다."); return;
        }
        json items = json::array();
        for (const auto& slot : slots) items.push_back(slotJson(slot));
        sendJson(res, {{"items", items}, {"count", items.size()}});
    }));
    server_->Get(R"(/api/v1/parking-slots/([^/]+))", guarded([this](const httplib::Request& req, httplib::Response& res) {
        database::ParkingSlotView slot;
        if (!database_.getParkingSlot(req.matches[1], slot)) {
            sendError(res, 404, "SLOT_NOT_FOUND", "주차면을 찾을 수 없습니다."); return;
        }
        sendJson(res, slotJson(slot));
    }));
    server_->Get("/api/v1/parking-sessions/active", guarded([this](const httplib::Request&, httplib::Response& res) {
        std::vector<database::ParkingSlotView> slots;
        if (!database_.listParkingSlots(slots)) {
            sendError(res, 503, "DATABASE_UNAVAILABLE", "활성 세션을 조회할 수 없습니다."); return;
        }
        json items = json::array();
        for (const auto& slot : slots) if (slot.session_id >= 0) items.push_back(slotJson(slot));
        sendJson(res, {{"items", items}, {"count", items.size()}});
    }));
    server_->Get(R"(/api/v1/parking-sessions/([0-9]+)/images)", guarded([this](const httplib::Request& req, httplib::Response& res) {
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
    }));
    server_->Get(R"(/api/v1/images/([0-9]+)/(original|enhanced))", guarded([this](const httplib::Request& req, httplib::Response& res) {
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
        const std::string mime_type = mimeType(candidate);
        res.set_content(std::move(body), mime_type.c_str());
    }));
    server_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404) sendError(res, 404, "ENDPOINT_NOT_FOUND", "API 경로를 찾을 수 없습니다.");
    });
}
}

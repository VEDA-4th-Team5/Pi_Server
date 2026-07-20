#include "bestshot/BestShotReceiver.hpp"

#include "util/Logger.hpp"
#include "util/TimeUtil.hpp"
#include "util/UrlMasker.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <filesystem>
#include <fstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// av_read_frame()이 네트워크에서 대기 중이어도 서버 종료 요청 시 빠져나오게 한다.
int interruptRead(void* opaque) {
    auto* running = static_cast<std::atomic<bool>*>(opaque);
    return running != nullptr && !running->load();
}

// 간단한 ONVIF XML 조각에서 시작/종료 문자열 사이 값을 꺼낸다.
// 완전한 범용 XML parser가 아니라 카메라 metadata 형식에 한정된 보조 함수다.
std::string between(const std::string& text, const std::string& begin,
                    const std::string& end) {
    std::size_t first = text.find(begin);
    if (first == std::string::npos) return "";
    first += begin.size();
    std::size_t last = text.find(end, first);
    return last == std::string::npos ? "" : text.substr(first, last - first);
}

std::string attribute(const std::string& text, const std::string& name) {
    return between(text, name + "=\"", "\"");
}

// curl 설정을 표준입력으로 전달할 때 따옴표와 역슬래시를 이스케이프한다.
std::string configEscape(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

struct CameraAddress {
    std::string user;
    std::string password;
    std::string host;
};

// RTSP URL의 인증정보와 host만 분리해 BestShot HTTPS 요청에 재사용한다.
CameraAddress parseRtspAddress(const std::string& url) {
    CameraAddress result;
    std::size_t scheme = url.find("://");
    std::size_t authority_start = scheme == std::string::npos ? 0 : scheme + 3;
    std::size_t path = url.find('/', authority_start);
    std::string authority = url.substr(authority_start, path - authority_start);
    std::size_t at = authority.rfind('@');
    std::string host_port = authority;
    if (at != std::string::npos) {
        std::string credentials = authority.substr(0, at);
        host_port = authority.substr(at + 1);
        std::size_t colon = credentials.find(':');
        result.user = credentials.substr(0, colon);
        if (colon != std::string::npos) result.password = credentials.substr(colon + 1);
    }
    if (!host_port.empty() && host_port.front() == '[') {
        std::size_t close = host_port.find(']');
        result.host = host_port.substr(0, close + 1);
    } else {
        result.host = host_port.substr(0, host_port.find(':'));
    }
    return result;
}

}

namespace bestshot {

BestShotReceiver::BestShotReceiver(
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    database::EventDatabase& database,
    parking::ParkingTriggerCoordinator& trigger_coordinator,
    ocr::OcrWorker& ocr_worker,
    std::atomic<bool>& running,
    std::string output_root)
    : channels_(channels), database_(database),
      trigger_coordinator_(trigger_coordinator), ocr_worker_(ocr_worker),
      running_(running),
      output_root_(std::move(output_root)) {
}

BestShotReceiver::~BestShotReceiver() { stop(); }

void BestShotReceiver::start() {
    // 영상 수신과 독립적으로 metadata를 읽어 채널 간 블로킹을 방지한다.
    for (const auto& channel : channels_)
        workers_.emplace_back(&BestShotReceiver::receiveLoop, this, channel);
}

void BestShotReceiver::stop() {
    // running_은 main에서 먼저 false가 되며 FFmpeg interrupt callback이 read를 깨운다.
    for (auto& worker : workers_)
        if (worker.joinable()) worker.join();
    workers_.clear();
}

std::string BestShotReceiver::slotIdForChannel(const std::string& channel_id) {
    if (channel_id == "ch01") return "EV01";
    if (channel_id == "ch02") return "EV02";
    if (channel_id == "ch03") return "EV03";
    if (channel_id == "ch04") return "EV04";
    return "";
}

void BestShotReceiver::receiveLoop(
    const std::shared_ptr<camera::CameraChannel>& channel) {
    std::string metadata_url = channel->rtsp_url;
    // 저해상도 영상 profile2를 쓰더라도 analytics metadata는 profile1에서 받는다.
    std::size_t profile = metadata_url.find("profile2/media.smp");
    if (profile != std::string::npos)
        metadata_url.replace(profile, std::string("profile2").size(), "profile1");

    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) return;
    format->interrupt_callback.callback = interruptRead;
    format->interrupt_callback.opaque = &running_;

    AVDictionary* options = nullptr;
    // CCTV 환경에서 손실을 줄이기 위해 TCP를 사용하고 무한 대기를 막는다.
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    if (avformat_open_input(&format, metadata_url.c_str(), nullptr, &options) < 0) {
        util::logError("BestShot metadata open failed: " +
                       util::hideUrlForLog(metadata_url));
        av_dict_free(&options);
        avformat_free_context(format);
        return;
    }
    av_dict_free(&options);
    avformat_find_stream_info(format, nullptr);

    int data_stream = -1;
    // 영상/음성이 아닌 ONVIF XML metadata가 담긴 data track을 찾는다.
    for (unsigned int i = 0; i < format->nb_streams; ++i) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA) {
            data_stream = static_cast<int>(i);
            break;
        }
    }
    if (data_stream < 0) {
        util::logError("BestShot metadata track missing: " + channel->channel_id);
        avformat_close_input(&format);
        return;
    }
    util::logInfo("BestShot metadata ready: " + channel->channel_id);

    std::string buffer;
    AVPacket* packet = av_packet_alloc();
    while (running_.load() && packet != nullptr) {
        int rc = av_read_frame(format, packet);
        if (rc < 0) break;
        if (packet->stream_index == data_stream && packet->data != nullptr) {
            // 한 XML 문서가 여러 AVPacket으로 나뉠 수 있으므로 닫는 태그까지 누적한다.
            buffer.append(reinterpret_cast<const char*>(packet->data), packet->size);
            const std::string closing = "</tt:MetadataStream>";
            std::size_t end;
            while ((end = buffer.find(closing)) != std::string::npos) {
                end += closing.size();
                processMetadata(channel->channel_id, buffer.substr(0, end), metadata_url);
                buffer.erase(0, end);
            }
            // 비정상 stream 때문에 닫는 태그를 못 찾을 때 메모리가 계속 늘어나는 것을 막는다.
            if (buffer.size() > 4U * 1024U * 1024U) buffer.clear();
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    util::logInfo("BestShot metadata stopped: " + channel->channel_id);
}

void BestShotReceiver::processMetadata(const std::string& channel_id,
                                       const std::string& xml,
                                       const std::string& rtsp_url) {
    std::size_t position = 0;
    // 한 MetadataStream 안에 여러 Object가 포함될 수 있어 모두 순회한다.
    while ((position = xml.find("<tt:Object ", position)) != std::string::npos) {
        std::size_t end = xml.find("</tt:Object>", position);
        if (end == std::string::npos) break;
        std::string object = xml.substr(position, end + 12 - position);
        position = end + 12;
        std::string image_ref = between(object, "<tt:ImageRef>", "</tt:ImageRef>");
        if (image_ref.empty()) continue;

        std::string object_id = attribute(object, "ObjectId");
        bool is_plate = object.find("LicensePlate") != std::string::npos ||
                        object.find(">License Plate<") != std::string::npos;
        bool is_vehicle = object.find(">Vehicle<") != std::string::npos;
        if (!is_vehicle && !is_plate) continue;

        util::logLine("BESTSHOT_META",
                      "received channel=" + channel_id +
                      " kind=" + (is_plate ? "plate" : "vehicle") +
                      " object=" + object_id);

        std::string slot_id;
        int active_session_id = -1;
        if (is_vehicle) {
            // 같은 채널에 활성 입차 세션이 있으면 반복 차량 BestShot을 새 입차로 만들지 않는다.
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto active = active_sessions_.find(channel_id);
                if (active != active_sessions_.end()) {
                    // 번호판이 오지 않은 비정상 세션도 다음 차량을 영구히 막지 않게 한다.
                    if (std::chrono::steady_clock::now() - active->second.created_at <
                        std::chrono::seconds(30)) {
                        util::logLine("BESTSHOT_META",
                                      "vehicle suppressed by active session channel=" +
                                      channel_id + " object=" + object_id +
                                      " session=" +
                                      std::to_string(active->second.session_id));
                        continue;
                    }
                    active_sessions_.erase(active);
                }
            }
            std::optional<std::string> matched_slot =
                trigger_coordinator_.claimSlotForVehicle(channel_id);
            // IVA가 있으면 해당 주차면을 사용하고, 없으면 카메라 BestShot 단독
            // 모드로 채널 기본 주차면에 저장한다.
            slot_id = matched_slot ? *matched_slot : slotIdForChannel(channel_id);
            if (slot_id.empty()) {
                util::logWarn("Vehicle BestShot has no slot mapping: channel=" + channel_id);
                continue;
            }
        } else {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto active = active_sessions_.find(channel_id);
            if (active == active_sessions_.end()) {
                // 이미지는 먼저 내려받아 잠시 보관하고 뒤따르는 Vehicle 세션에 붙인다.
                active_session_id = -1;
            } else {
                slot_id = active->second.slot_id;
                active_session_id = active->second.session_id;
            }
        }

        std::string key = channel_id + ":" + image_ref;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // 카메라가 같은 ImageRef를 반복 전송해도 파일과 DB 행은 한 번만 만든다.
            if (!processed_refs_.insert(key).second) continue;
        }

        std::string kind = is_plate ? "plate" : "vehicle";
        // IVA 없이 들어오는 카메라 BestShot도 기존 vehicle/plate 구조에 저장한다.
        fs::path directory = fs::path(output_root_) / kind;
        fs::create_directories(directory);
        fs::path destination = directory /
            (channel_id + "_" + kind + "_obj" + object_id + "_" +
             util::nowStringForFilename() + ".jpg");

        if (!downloadImage(rtsp_url, image_ref, destination.string())) {
            // 일시적인 권한/네트워크 오류였다면 같은 ImageRef 알림에서 다시 받을 수 있게 한다.
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                processed_refs_.erase(key);
            }
            util::logWarn("BestShot download failed: " + channel_id +
                          " object=" + object_id);
            continue;
        }

        if (is_vehicle) {
            // 차량 이미지가 먼저 도착하면 주차면을 점유시키고 새 세션을 생성한다.
            int session_id = -1;
            if (database_.createEntryWithBestShot(slot_id, destination.string(),
                                                  object_id, &session_id)) {
                std::optional<PendingPlate> pending_plate;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    active_sessions_[channel_id] = {
                        session_id, slot_id, std::chrono::steady_clock::now()};
                    auto pending = pending_plates_.find(channel_id);
                    if (pending != pending_plates_.end()) {
                        if (std::chrono::steady_clock::now() - pending->second.created_at <
                            std::chrono::seconds(15)) {
                            pending_plate = pending->second;
                        } else {
                            fs::remove(pending->second.image_path);
                        }
                        pending_plates_.erase(pending);
                    }
                }
                util::logLine("BESTSHOT", "vehicle saved channel=" + channel_id +
                              " slot=" + slot_id +
                              " session=" + std::to_string(session_id) +
                              " path=" + destination.string());
                if (pending_plate && database_.attachPlateBestShot(
                        session_id, pending_plate->image_path,
                        pending_plate->plate_text)) {
                    util::logLine("BESTSHOT", "pending plate attached channel=" +
                                  channel_id + " slot=" + slot_id + " session=" +
                                  std::to_string(session_id) + " path=" +
                                  pending_plate->image_path);
                    ocr_worker_.enqueue(session_id, slot_id,
                                        pending_plate->image_path);
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    active_sessions_.erase(channel_id);
                }
            } else {
                fs::remove(destination);
                std::lock_guard<std::mutex> lock(state_mutex_);
                processed_refs_.erase(key);
            }
        } else {
            // 번호판 이미지는 앞서 차량 BestShot이 만든 같은 채널의 세션에 연결한다.
            std::string plate_text = between(object, "<tt:PlateNumber>",
                                             "</tt:PlateNumber>");
            if (active_session_id < 0) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto old = pending_plates_.find(channel_id);
                if (old != pending_plates_.end()) fs::remove(old->second.image_path);
                pending_plates_[channel_id] = {
                    destination.string(), plate_text,
                    std::chrono::steady_clock::now()};
                util::logLine("BESTSHOT", "plate pending before vehicle channel=" +
                              channel_id + " path=" + destination.string());
            } else if (database_.attachPlateBestShot(
                    active_session_id, destination.string(), plate_text)) {
                util::logLine("BESTSHOT", "plate saved channel=" + channel_id +
                              " slot=" + slot_id +
                              " session=" + std::to_string(active_session_id) +
                              " path=" + destination.string());
                ocr_worker_.enqueue(active_session_id, slot_id,
                                    destination.string());
                // 한 입차에서 반복되는 plate ImageRef는 첫 성공본 이후 받지 않는다.
                std::lock_guard<std::mutex> lock(state_mutex_);
                active_sessions_.erase(channel_id);
            } else {
                fs::remove(destination);
                std::lock_guard<std::mutex> lock(state_mutex_);
                processed_refs_.erase(key);
            }
        }
    }
}

bool BestShotReceiver::downloadImage(const std::string& rtsp_url,
                                     const std::string& image_ref,
                                     const std::string& destination) {
    CameraAddress camera = parseRtspAddress(rtsp_url);
    if (camera.user.empty() || camera.host.empty() || image_ref.empty()) return false;
    std::string temporary = destination + ".part";
    int input_pipe[2];
    if (pipe(input_pipe) != 0) return false;
    pid_t child = fork();
    if (child == 0) {
        // 인증정보가 명령행 인수나 process list에 노출되지 않도록 stdin 설정으로 전달한다.
        dup2(input_pipe[0], STDIN_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        execlp("curl", "curl", "-k", "-sS", "--fail", "--digest",
               "--connect-timeout", "3", "--max-time", "8", "--config", "-",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    close(input_pipe[0]);
    std::string config =
        "user = \"" + configEscape(camera.user + ":" + camera.password) + "\"\n" +
        "url = \"https://" + configEscape(camera.host + image_ref) + "\"\n" +
        "output = \"" + configEscape(temporary) + "\"\n";
    ssize_t ignored = write(input_pipe[1], config.data(), config.size());
    (void)ignored;
    close(input_pipe[1]);
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fs::remove(temporary);
        return false;
    }
    std::ifstream file(temporary, std::ios::binary);
    // HTTP 오류 문서 등을 JPEG로 오인하지 않도록 SOI magic byte를 검사한다.
    unsigned char magic[3] = {};
    file.read(reinterpret_cast<char*>(magic), sizeof(magic));
    if (file.gcount() != 3 || magic[0] != 0xFF || magic[1] != 0xD8 || magic[2] != 0xFF) {
        file.close(); fs::remove(temporary); return false;
    }
    file.close();
    fs::rename(temporary, destination);
    return true;
}

}

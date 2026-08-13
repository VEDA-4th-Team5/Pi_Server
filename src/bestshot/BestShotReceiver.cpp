#include "bestshot/BestShotReceiver.hpp"

#include "util/Logger.hpp"
#include "util/UrlMasker.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

int interruptRead(void* opaque) {
    auto* running = static_cast<std::atomic<bool>*>(opaque);
    return running != nullptr && !running->load();
}

std::string between(const std::string& text, const std::string& begin,
                    const std::string& end) {
    std::size_t first = text.find(begin);
    if (first == std::string::npos) return {};
    first += begin.size();
    const std::size_t last = text.find(end, first);
    return last == std::string::npos ? std::string{}
                                     : text.substr(first, last - first);
}

std::string attribute(const std::string& text, const std::string& name) {
    return between(text, name + "=\"", "\"");
}

std::string configEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
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

class InFlightReservation {
public:
    InFlightReservation(std::mutex& mutex,
                        std::unordered_set<std::string>& identities,
                        std::string identity)
        : mutex_(mutex), identities_(identities), identity_(std::move(identity)) {
        std::lock_guard lock(mutex_);
        acquired_ = identities_.insert(identity_).second;
    }

    ~InFlightReservation() {
        if (!acquired_) return;
        std::lock_guard lock(mutex_);
        identities_.erase(identity_);
    }

    InFlightReservation(const InFlightReservation&) = delete;
    InFlightReservation& operator=(const InFlightReservation&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    std::mutex& mutex_;
    std::unordered_set<std::string>& identities_;
    std::string identity_;
    bool acquired_{};
};

CameraAddress parseRtspAddress(const std::string& url) {
    CameraAddress result;
    const std::size_t scheme = url.find("://");
    const std::size_t authority_start =
        scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t path = url.find('/', authority_start);
    const std::string authority = url.substr(
        authority_start, path == std::string::npos
                             ? std::string::npos
                             : path - authority_start);
    const std::size_t at = authority.rfind('@');
    std::string host_port = authority;
    if (at != std::string::npos) {
        const std::string credentials = authority.substr(0, at);
        host_port = authority.substr(at + 1);
        const std::size_t colon = credentials.find(':');
        result.user = credentials.substr(0, colon);
        if (colon != std::string::npos)
            result.password = credentials.substr(colon + 1);
    }
    if (!host_port.empty() && host_port.front() == '[') {
        const std::size_t close = host_port.find(']');
        if (close != std::string::npos)
            result.host = host_port.substr(0, close + 1);
    } else {
        result.host = host_port.substr(0, host_port.find(':'));
    }
    return result;
}

}  // namespace

namespace bestshot {

BestShotReceiver::BestShotReceiver(
    std::vector<std::shared_ptr<camera::CameraChannel>>& channels,
    parking::ParkingTriggerCoordinator& trigger_coordinator,
    ocr::OcrWorker& ocr_worker,
    std::atomic<bool>& running,
    std::string output_root,
    DownloadCallback downloader,
    OcrCallback ocr_callback,
    const std::size_t pending_capacity)
    : channels_(channels),
      trigger_coordinator_(trigger_coordinator),
      running_(running),
      output_root_(std::move(output_root)),
      downloader_(std::move(downloader)),
      ocr_callback_(std::move(ocr_callback)),
      pending_capacity_(std::max<std::size_t>(1, pending_capacity)) {
    if (!downloader_) {
        downloader_ = [this](const std::string& rtsp_url,
                             const std::string& image_ref,
                             const std::string& destination) {
            return downloadImage(rtsp_url, image_ref, destination);
        };
    }
    if (!ocr_callback_) {
        ocr_callback_ = [&ocr_worker](const int session_id,
                                     const std::string& slot_id,
                                     const std::string& image_path) {
            ocr_worker.enqueue(session_id, slot_id, image_path);
        };
    }
}

BestShotReceiver::~BestShotReceiver() { stop(); }

void BestShotReceiver::start() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (pending_worker_.joinable() || !workers_.empty()) return;

    pending_worker_stop_.store(false);
    pending_worker_ = std::thread(&BestShotReceiver::pendingLoop, this);
    for (const auto& channel : channels_) {
        if (channel)
            workers_.emplace_back(
                &BestShotReceiver::receiveLoop, this, channel);
    }
}

void BestShotReceiver::stop() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    // The application owns running_.  Its shutdown sequence clears it before
    // calling stop(), causing FFmpeg's interrupt callback to release reads.
    // Join metadata producers before the Pending reconciler so no producer
    // can enqueue an item after its only consumer has stopped.
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    pending_worker_stop_.store(true);
    pending_condition_.notify_all();
    if (pending_worker_.joinable()) pending_worker_.join();
}

void BestShotReceiver::receiveLoop(
    const std::shared_ptr<camera::CameraChannel>& channel) {
    std::string metadata_url = channel->rtsp_url;
    const std::size_t profile = metadata_url.find("profile2/media.smp");
    if (profile != std::string::npos) {
        metadata_url.replace(profile, std::string("profile2").size(),
                             "profile1");
    }

    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) return;
    format->interrupt_callback.callback = interruptRead;
    format->interrupt_callback.opaque = &running_;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    if (avformat_open_input(&format, metadata_url.c_str(), nullptr, &options) <
        0) {
        util::logError("BestShot metadata open failed: " +
                       util::hideUrlForLog(metadata_url));
        av_dict_free(&options);
        avformat_free_context(format);
        return;
    }
    av_dict_free(&options);
    avformat_find_stream_info(format, nullptr);

    int data_stream = -1;
    for (unsigned int i = 0; i < format->nb_streams; ++i) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA) {
            data_stream = static_cast<int>(i);
            break;
        }
    }
    if (data_stream < 0) {
        util::logError("BestShot metadata track missing: " +
                       channel->channel_id);
        avformat_close_input(&format);
        return;
    }
    util::logInfo("BestShot metadata ready: " + channel->channel_id);

    std::string buffer;
    AVPacket* packet = av_packet_alloc();
    while (running_.load() && packet != nullptr) {
        const int rc = av_read_frame(format, packet);
        if (rc < 0) break;
        if (packet->stream_index == data_stream && packet->data != nullptr) {
            buffer.append(reinterpret_cast<const char*>(packet->data),
                          packet->size);
            const std::string closing = "</tt:MetadataStream>";
            std::size_t end = 0;
            while ((end = buffer.find(closing)) != std::string::npos) {
                end += closing.size();
                (void)processMetadataDocument(
                    channel->camera_id, channel->channel_id,
                    buffer.substr(0, end), metadata_url);
                buffer.erase(0, end);
            }
            if (buffer.size() > 4U * 1024U * 1024U) buffer.clear();
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    util::logInfo("BestShot metadata stopped: " + channel->channel_id);
}

std::vector<BestShotProcessResult>
BestShotReceiver::processMetadataDocument(
    const std::string& camera_id,
    const std::string& channel_id,
    const std::string& xml,
    const std::string& rtsp_url) {
    std::vector<BestShotProcessResult> results;
    std::size_t position = 0;
    while ((position = xml.find("<tt:Object ", position)) !=
           std::string::npos) {
        const std::size_t end = xml.find("</tt:Object>", position);
        if (end == std::string::npos) break;
        const std::string object = xml.substr(position, end + 12 - position);
        position = end + 12;

        const bool is_plate =
            object.find("LicensePlate") != std::string::npos ||
            object.find(">License Plate<") != std::string::npos;
        const bool is_vehicle = object.find(">Vehicle<") != std::string::npos;
        if (!is_vehicle && !is_plate) continue;

        PendingMetadata metadata;
        metadata.cameraId = camera_id;
        metadata.channelId = channel_id;
        metadata.objectId = attribute(object, "ObjectId");
        metadata.imageRef = between(
            object, "<tt:ImageRef>", "</tt:ImageRef>");
        metadata.plateText = between(
            object, "<tt:PlateNumber>", "</tt:PlateNumber>");
        metadata.rtspUrl = rtsp_url;
        metadata.kind = is_plate ? parking::BestShotEvidenceKind::Plate
                                 : parking::BestShotEvidenceKind::Vehicle;
        metadata.createdAtEpochMs = trigger_coordinator_.nowEpochMs();
        metadata.evidenceIdentity = evidenceIdentity(metadata);
        metadata.exactIdentityKey = exactIdentityKey(metadata);

        if (metadata.cameraId.empty() || metadata.channelId.empty() ||
            metadata.objectId.empty() || metadata.imageRef.empty()) {
            BestShotProcessResult invalid;
            invalid.code = BestShotProcessCode::InvalidMetadata;
            invalid.kind = metadata.kind;
            invalid.evidenceIdentity = metadata.evidenceIdentity;
            invalid.cameraId = metadata.cameraId;
            invalid.channelId = metadata.channelId;
            invalid.objectId = metadata.objectId;
            invalid.imageRef = metadata.imageRef;
            invalid.message =
                "BestShot metadata lacks camera/channel/ObjectId/ImageRef";
            results.push_back(std::move(invalid));
            continue;
        }

        util::logLine(
            "BESTSHOT_META",
            "received camera=" + camera_id + " channel=" + channel_id +
                " kind=" + (is_plate ? "plate" : "vehicle") +
                " object=" + metadata.objectId +
                " evidence=" + metadata.evidenceIdentity);
        results.push_back(processOne(std::move(metadata), true));
    }
    return results;
}

BestShotProcessResult BestShotReceiver::processOne(
    PendingMetadata metadata,
    const bool may_requeue) {
    BestShotProcessResult result;
    result.kind = metadata.kind;
    result.evidenceIdentity = metadata.evidenceIdentity;
    result.cameraId = metadata.cameraId;
    result.channelId = metadata.channelId;
    result.objectId = metadata.objectId;
    result.imageRef = metadata.imageRef;

    parking::ParkingCorrelationMatch match;
    try {
        match = trigger_coordinator_.resolve(
            metadata.cameraId, metadata.channelId, metadata.objectId);
    } catch (const std::exception& error) {
        result.code = BestShotProcessCode::AttachRejected;
        result.attachCode = parking::BestShotAttachCode::RetryableFailure;
        result.message = error.what();
        if (may_requeue &&
            trigger_coordinator_.nowEpochMs() - metadata.createdAtEpochMs <
                trigger_coordinator_.correlationWindow().count()) {
            (void)enqueuePending(std::move(metadata));
        }
        return result;
    } catch (...) {
        result.code = BestShotProcessCode::AttachRejected;
        result.attachCode = parking::BestShotAttachCode::RetryableFailure;
        result.message = "unknown correlation lookup failure";
        if (may_requeue &&
            trigger_coordinator_.nowEpochMs() - metadata.createdAtEpochMs <
                trigger_coordinator_.correlationWindow().count()) {
            (void)enqueuePending(std::move(metadata));
        }
        return result;
    }
    result.candidateCorrelationIds = match.candidateCorrelationIds;
    switch (match.kind) {
    case parking::ParkingCorrelationMatchKind::Unmatched:
        result.code = BestShotProcessCode::Unmatched;
        result.message = "no live correlation candidate";
        util::logWarn("BestShot unmatched: evidence=" +
                      metadata.evidenceIdentity);
        return result;
    case parking::ParkingCorrelationMatchKind::Ambiguous:
        result.code = BestShotProcessCode::Ambiguous;
        result.message = "multiple live correlation candidates";
        util::logWarn(
            "BestShot quarantined as ambiguous: evidence=" +
            metadata.evidenceIdentity + " candidates=" +
            std::to_string(match.candidateCorrelationIds.size()));
        return result;
    case parking::ParkingCorrelationMatchKind::Pending: {
        const auto age = trigger_coordinator_.nowEpochMs() -
                         metadata.createdAtEpochMs;
        if (age >= trigger_coordinator_.correlationWindow().count()) {
            result.code = BestShotProcessCode::ExpiredPending;
            result.message = "pending correlation window expired";
            return result;
        }
        result.code = BestShotProcessCode::Pending;
        result.message = "correlation has not committed";
        if (may_requeue) (void)enqueuePending(std::move(metadata));
        return result;
    }
    case parking::ParkingCorrelationMatchKind::Unique:
        break;
    }

    if (!match.lease) {
        result.code = BestShotProcessCode::AttachRejected;
        result.attachCode = parking::BestShotAttachCode::Conflict;
        result.message = "unique match omitted its committed lease";
        return result;
    }
    const auto& lease = *match.lease;
    result.correlationId = lease.correlationId;
    result.sessionId = lease.sessionId;

    InFlightReservation in_flight(
        pending_mutex_, in_flight_identity_index_,
        std::to_string(lease.correlationId.size()) + ":" +
            lease.correlationId + "|" + metadata.exactIdentityKey);
    if (!in_flight.acquired()) {
        result.code = BestShotProcessCode::DuplicateInFlight;
        result.message = "same correlation evidence is already in flight";
        return result;
    }

    const std::string kind_name =
        metadata.kind == parking::BestShotEvidenceKind::Plate
            ? "plate"
            : "vehicle";
    const fs::path directory = fs::path(output_root_) / kind_name;
    const fs::path destination = directory /
        (safePathComponent(metadata.channelId) + "_" + kind_name +
         "_obj" + safePathComponent(metadata.objectId) + "_" +
         stableDigest(metadata.evidenceIdentity + "|" +
                      lease.correlationId) + ".jpg");
    result.imagePath = destination.string();

    std::error_code file_error;
    const bool destination_existed = fs::exists(destination, file_error) &&
                                     !file_error;
    if (file_error) {
        result.code = BestShotProcessCode::DownloadFailed;
        result.message = "BestShot destination could not be inspected: " +
                         file_error.message();
        return result;
    }
    try {
        fs::create_directories(directory);
        if (!downloader_(metadata.rtspUrl, metadata.imageRef,
                         destination.string())) {
            if (!destination_existed) {
                std::error_code ignored;
                fs::remove(destination, ignored);
            }
            result.code = BestShotProcessCode::DownloadFailed;
            result.message = "BestShot image download failed";
            return result;
        }
    } catch (const std::exception& error) {
        if (!destination_existed) {
            std::error_code ignored;
            fs::remove(destination, ignored);
        }
        result.code = BestShotProcessCode::DownloadFailed;
        result.message = error.what();
        return result;
    } catch (...) {
        if (!destination_existed) {
            std::error_code ignored;
            fs::remove(destination, ignored);
        }
        result.code = BestShotProcessCode::DownloadFailed;
        result.message = "unknown BestShot download failure";
        return result;
    }

    parking::BestShotAttachResult attached;
    try {
        attached = trigger_coordinator_.attachBestShotIfActive(
            lease, metadata.kind, metadata.imageRef, destination.string(),
            metadata.plateText);
    } catch (const std::exception& error) {
        attached = {parking::BestShotAttachCode::RetryableFailure, -1,
                    error.what()};
    } catch (...) {
        attached = {parking::BestShotAttachCode::RetryableFailure, -1,
                    "unknown correlation attach failure"};
    }

    // A transient/unknown SQLite outcome is reconciled once by the stable
    // evidence identity before artifact cleanup.  If the first transaction
    // committed but its response was lost, the exact retry returns
    // AlreadyAttached; if it rolled back, the retry may perform the one
    // authoritative insert.
    if (attached.code == parking::BestShotAttachCode::RetryableFailure) {
        try {
            attached = trigger_coordinator_.attachBestShotIfActive(
                lease, metadata.kind, metadata.imageRef,
                destination.string(), metadata.plateText);
        } catch (const std::exception& error) {
            attached = {parking::BestShotAttachCode::RetryableFailure, -1,
                        error.what()};
        } catch (...) {
            attached = {
                parking::BestShotAttachCode::RetryableFailure, -1,
                "unknown correlation attach reconciliation failure"};
        }
    }
    result.attachCode = attached.code;
    result.message = attached.message;

    if (!attached.accepted()) {
        // A lease can become Ended/Expired after resolution but before the
        // image download finishes.  Such an artifact has no authority and is
        // removed.  If an authoritative canonical row already existed, the
        // exact DB identity check above would have returned AlreadyAttached;
        // therefore every rejected path is non-authoritative even when the
        // deterministic file was left behind by an earlier crash.
        std::error_code cleanup_error;
        fs::remove(destination, cleanup_error);
        if (cleanup_error) {
            util::logError(
                "BestShot rejected artifact cleanup failed: path=" +
                destination.string() + " reason=" +
                cleanup_error.message());
            result.message +=
                "; artifact cleanup failed: " + cleanup_error.message();
        }
        result.code = BestShotProcessCode::AttachRejected;
        util::logWarn(
            "BestShot attach rejected after download: evidence=" +
            metadata.evidenceIdentity + " correlation=" +
            lease.correlationId + " reason=" + attached.message);
        return result;
    }

    if (attached.code == parking::BestShotAttachCode::AlreadyAttached) {
        result.code = BestShotProcessCode::AlreadyAttached;
        return result;
    }

    result.code = BestShotProcessCode::Attached;
    util::logLine(
        "BESTSHOT",
        "saved evidence=" + metadata.evidenceIdentity +
            " correlation=" + lease.correlationId +
            " slot=" + lease.slotId +
            " session=" + std::to_string(lease.sessionId) +
            " path=" + destination.string());

    if (metadata.kind == parking::BestShotEvidenceKind::Plate) {
        if (lease.sessionId > std::numeric_limits<int>::max()) {
            util::logError(
                "BestShot OCR session id exceeds worker range: " +
                std::to_string(lease.sessionId));
        } else {
            try {
                ocr_callback_(static_cast<int>(lease.sessionId),
                              lease.slotId, destination.string());
            } catch (const std::exception& error) {
                util::logError("BestShot OCR enqueue failed: " +
                               std::string(error.what()));
            } catch (...) {
                util::logError("BestShot OCR enqueue failed: unknown error");
            }
        }
    }
    return result;
}

bool BestShotReceiver::enqueuePending(PendingMetadata metadata) {
    std::lock_guard lock(pending_mutex_);
    if (pending_identity_index_.contains(metadata.exactIdentityKey))
        return false;
    if (pending_identity_index_.size() >= pending_capacity_) {
        // Normally the oldest queued item is evictable.  During reconciliation
        // every capacity slot may instead be reserved by an in-flight item;
        // reject the new item in that case rather than exceed the bound.
        if (pending_metadata_.empty()) {
            util::logWarn(
                "BestShot pending quarantine rejected at capacity: evidence=" +
                metadata.evidenceIdentity);
            return false;
        }
        pending_identity_index_.erase(
            pending_metadata_.front().exactIdentityKey);
        util::logWarn(
            "BestShot pending quarantine evicted oldest evidence=" +
            pending_metadata_.front().evidenceIdentity);
        pending_metadata_.pop_front();
    }
    pending_identity_index_.insert(metadata.exactIdentityKey);
    pending_metadata_.push_back(std::move(metadata));
    pending_condition_.notify_one();
    return true;
}

std::vector<BestShotProcessResult> BestShotReceiver::reconcilePending() {
    std::deque<PendingMetadata> pending;
    {
        std::lock_guard lock(pending_mutex_);
        pending.swap(pending_metadata_);
    }

    std::vector<BestShotProcessResult> results;
    results.reserve(pending.size());
    const auto now = trigger_coordinator_.nowEpochMs();
    for (auto& metadata : pending) {
        const std::string exact_identity = metadata.exactIdentityKey;
        if (now - metadata.createdAtEpochMs >=
            trigger_coordinator_.correlationWindow().count()) {
            BestShotProcessResult expired;
            expired.code = BestShotProcessCode::ExpiredPending;
            expired.kind = metadata.kind;
            expired.evidenceIdentity = metadata.evidenceIdentity;
            expired.cameraId = metadata.cameraId;
            expired.channelId = metadata.channelId;
            expired.objectId = metadata.objectId;
            expired.imageRef = metadata.imageRef;
            expired.message = "pending correlation window expired";
            {
                std::lock_guard lock(pending_mutex_);
                pending_identity_index_.erase(exact_identity);
            }
            results.push_back(std::move(expired));
            continue;
        }
        auto result = processOne(metadata, false);
        const bool retry =
            result.code == BestShotProcessCode::Pending ||
            result.code == BestShotProcessCode::DownloadFailed ||
            (result.code == BestShotProcessCode::AttachRejected &&
             result.attachCode ==
                 parking::BestShotAttachCode::RetryableFailure);
        {
            std::lock_guard lock(pending_mutex_);
            if (retry) {
                // Keep the original first-seen timestamp and the reserved
                // exact identity.  Redelivery cannot extend its expiry.
                pending_metadata_.push_back(std::move(metadata));
            } else {
                pending_identity_index_.erase(exact_identity);
            }
        }
        if (retry) pending_condition_.notify_one();
        results.push_back(std::move(result));
    }
    return results;
}

void BestShotReceiver::pendingLoop() {
    while (!pending_worker_stop_.load()) {
        std::unique_lock lock(pending_mutex_);
        if (pending_metadata_.empty()) {
            pending_condition_.wait(lock, [this] {
                return pending_worker_stop_.load() ||
                       !pending_metadata_.empty();
            });
        } else {
            pending_condition_.wait_for(
                lock, std::chrono::milliseconds(100), [this] {
                    return pending_worker_stop_.load();
                });
        }
        if (pending_worker_stop_.load()) return;
        const bool has_pending = !pending_metadata_.empty();
        lock.unlock();
        if (has_pending) (void)reconcilePending();
    }
}

std::string BestShotReceiver::evidenceIdentity(
    const PendingMetadata& metadata) {
    return "bestshot:" + stableDigest(exactIdentityKey(metadata));
}

std::string BestShotReceiver::exactIdentityKey(
    const PendingMetadata& metadata) {
    const auto append = [](std::string& destination,
                           const std::string& value) {
        destination += std::to_string(value.size());
        destination.push_back(':');
        destination += value;
        destination.push_back('|');
    };
    std::string key;
    key.reserve(metadata.cameraId.size() + metadata.channelId.size() +
                metadata.objectId.size() + metadata.imageRef.size() + 64);
    append(key, metadata.cameraId);
    append(key, metadata.channelId);
    append(key, metadata.objectId);
    append(key, parking::toString(metadata.kind));
    append(key, metadata.imageRef);
    return key;
}

std::string BestShotReceiver::safePathComponent(const std::string& value) {
    std::string safe;
    safe.reserve(std::min<std::size_t>(value.size(), 64));
    for (const unsigned char character : value) {
        if (safe.size() >= 64) break;
        if (std::isalnum(character) || character == '-' || character == '_')
            safe.push_back(static_cast<char>(character));
        else
            safe.push_back('_');
    }
    return safe.empty() ? "unknown" : safe;
}

std::string BestShotReceiver::stableDigest(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

bool BestShotReceiver::downloadImage(const std::string& rtsp_url,
                                     const std::string& image_ref,
                                     const std::string& destination) {
    const CameraAddress camera = parseRtspAddress(rtsp_url);
    if (camera.user.empty() || camera.host.empty() || image_ref.empty())
        return false;

    const std::string temporary = destination + ".part";
    int input_pipe[2];
    if (pipe(input_pipe) != 0) return false;
    const pid_t child = fork();
    if (child == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        execlp("curl", "curl", "-k", "-sS", "--fail", "--digest",
               "--connect-timeout", "3", "--max-time", "8", "--config",
               "-", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return false;
    }

    close(input_pipe[0]);
    const std::string config =
        "user = \"" +
        configEscape(camera.user + ":" + camera.password) + "\"\n" +
        "url = \"https://" + configEscape(camera.host + image_ref) +
        "\"\n" +
        "output = \"" + configEscape(temporary) + "\"\n";
    std::size_t offset = 0;
    while (offset < config.size()) {
        const ssize_t written = write(
            input_pipe[1], config.data() + offset, config.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    close(input_pipe[1]);

    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || offset != config.size()) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }

    std::ifstream file(temporary, std::ios::binary);
    unsigned char magic[3] = {};
    file.read(reinterpret_cast<char*>(magic), sizeof(magic));
    if (file.gcount() != 3 || magic[0] != 0xFF || magic[1] != 0xD8 ||
        magic[2] != 0xFF) {
        file.close();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    file.close();

    std::error_code rename_error;
    fs::rename(temporary, destination, rename_error);
    if (rename_error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

}  // namespace bestshot

#pragma once

#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "parking/AppliedParkingRoi.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace parking {

enum class EvidenceReason {
    OccupancyStart,
    Overstay
};

[[nodiscard]] const char* toString(EvidenceReason reason) noexcept;

struct EvidenceCaptureRequest {
    std::int64_t sessionId{-1};
    std::string slotId;
    std::shared_ptr<camera::CameraChannel> channel;
    snapshot::NormalizedRoi roi{};
    std::chrono::steady_clock::time_point startedAtMonotonic;
    int snapshotApiChannel{0};
    std::uint64_t roiRevision{};
};

struct EvidenceCaptureResult {
    std::int64_t sessionId{-1};
    std::string slotId;
    std::string channelId;
    EvidenceReason reason{EvidenceReason::OccupancyStart};
    std::string imagePath;
    std::string enhancedImagePath;
    snapshot::NormalizedRoi roi{};
    std::uint64_t roiRevision{};
    bool stored{};
    bool duplicate{};
    std::string message;
};

/** @brief 카메라 API 또는 최신 RTSP 프레임의 증거 저장을 전용 스레드에서 처리한다. */
class EvidenceCaptureWorker {
public:
    struct Config {
        std::chrono::milliseconds overstayDelay{std::chrono::hours{1}};
        std::size_t maxPendingJobs{128};
    };
    using Completion = std::function<void(const EvidenceCaptureResult&)>;
    using Capture = std::function<snapshot::StoredImagePair(
        const EvidenceCaptureRequest&, EvidenceReason)>;
    using RoiResolver = std::function<std::optional<AppliedParkingRoi>(
        const std::string& slot_id)>;

    EvidenceCaptureWorker(snapshot::SnapshotStorage& storage,
                          database::EventDatabase& database,
                          Config config,
                          Completion completion = {},
                          Capture capture = {},
                          RoiResolver roi_resolver = {});
    ~EvidenceCaptureWorker();

    EvidenceCaptureWorker(const EvidenceCaptureWorker&) = delete;
    EvidenceCaptureWorker& operator=(const EvidenceCaptureWorker&) = delete;

    bool start();
    void stop();

    /** @brief 시작 즉시 한 장과 T0+지연 한 장을 세션당 한 번 예약한다. */
    bool scheduleSession(EvidenceCaptureRequest request);
    /** @brief 재시작 시 DB에 없는 증거만 원래 T0 기준으로 다시 예약한다. */
    bool restoreSession(EvidenceCaptureRequest request);
    /** @brief 타이머가 먼저 만료되면 기존 초과 증거 작업을 즉시 실행 대상으로 만든다. */
    bool expediteOverstay(std::int64_t session_id);
    /** @brief 모든 활성 세션의 초과 증거 deadline을 같은 T0 기준으로 재계산한다. */
    std::size_t updateOverstayDelay(std::chrono::milliseconds delay);
    /** @brief VACANT 세션의 아직 실행되지 않은 작업을 취소한다. */
    void cancelSession(std::int64_t session_id);
    [[nodiscard]] std::size_t pendingCount() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Job {
        Clock::time_point deadline;
        std::uint64_t sequence{};
        std::uint64_t generation{};
        EvidenceCaptureRequest request;
        EvidenceReason reason{EvidenceReason::OccupancyStart};
        bool terminal{};
    };
    struct Later {
        bool operator()(const Job& left, const Job& right) const noexcept;
    };

    void run() noexcept;
    void process(Job job) noexcept;
    bool scheduleSessionImpl(EvidenceCaptureRequest request,
                             bool include_start,
                             bool include_overstay,
                             bool restored);
    [[nodiscard]] bool canceled(std::int64_t session_id) const;
    void emit(EvidenceCaptureResult result) noexcept;

    snapshot::SnapshotStorage& storage_;
    database::EventDatabase& database_;
    Config config_;
    Completion completion_;
    Capture capture_;
    RoiResolver roi_resolver_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::priority_queue<Job, std::vector<Job>, Later> jobs_;
    std::unordered_set<std::int64_t> scheduledSessions_;
    std::unordered_set<std::int64_t> canceledSessions_;
    bool running_{};
    bool stopping_{};
    std::uint64_t nextSequence_{};
    std::uint64_t overstayGeneration_{1};
    std::optional<std::int64_t> inFlightSession_;
    std::optional<EvidenceReason> inFlightReason_;
    std::thread worker_;
};

}  // namespace parking

#pragma once

#include "camera/CameraChannel.hpp"
#include "database/EventDatabase.hpp"
#include "snapshot/SnapshotStorage.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
};

struct EvidenceCaptureResult {
    std::int64_t sessionId{-1};
    std::string slotId;
    std::string channelId;
    EvidenceReason reason{EvidenceReason::OccupancyStart};
    std::string imagePath;
    bool stored{};
    bool duplicate{};
    std::string message;
};

/** @brief 최신 RTSP 프레임의 증거 저장과 예약을 전용 스레드에서 처리한다. */
class EvidenceCaptureWorker {
public:
    struct Config {
        std::chrono::milliseconds overstayDelay{std::chrono::hours{1}};
        std::size_t maxPendingJobs{128};
    };
    using Completion = std::function<void(const EvidenceCaptureResult&)>;

    EvidenceCaptureWorker(snapshot::SnapshotStorage& storage,
                          database::EventDatabase& database,
                          Config config,
                          Completion completion = {});
    ~EvidenceCaptureWorker();

    EvidenceCaptureWorker(const EvidenceCaptureWorker&) = delete;
    EvidenceCaptureWorker& operator=(const EvidenceCaptureWorker&) = delete;

    bool start();
    void stop();

    /** @brief 시작 즉시 한 장과 T0+지연 한 장을 세션당 한 번 예약한다. */
    bool scheduleSession(EvidenceCaptureRequest request);
    /** @brief VACANT 세션의 아직 실행되지 않은 작업을 취소한다. */
    void cancelSession(std::int64_t session_id);
    [[nodiscard]] std::size_t pendingCount() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Job {
        Clock::time_point deadline;
        std::uint64_t sequence{};
        EvidenceCaptureRequest request;
        EvidenceReason reason{EvidenceReason::OccupancyStart};
    };
    struct Later {
        bool operator()(const Job& left, const Job& right) const noexcept;
    };

    void run() noexcept;
    void process(Job job) noexcept;
    [[nodiscard]] bool canceled(std::int64_t session_id) const;
    void emit(EvidenceCaptureResult result) noexcept;

    snapshot::SnapshotStorage& storage_;
    database::EventDatabase& database_;
    Config config_;
    Completion completion_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::priority_queue<Job, std::vector<Job>, Later> jobs_;
    std::unordered_set<std::int64_t> scheduledSessions_;
    std::unordered_set<std::int64_t> canceledSessions_;
    bool running_{};
    bool stopping_{};
    std::uint64_t nextSequence_{};
    std::thread worker_;
};

}  // namespace parking

#pragma once

#include "database/SessionTransitionStore.hpp"
#include "parking/SlotTransitionTypes.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace parking {

enum class SlotSubmitCode {
    TransportQueued,
    Admitted,
    Existing,
    AlreadyTerminal,
    RejectedStale,
    Conflict,
    QueueFull,
    Closed,
    RetryableFailure
};

struct SlotSubmitResult {
    SlotSubmitCode code{SlotSubmitCode::RetryableFailure};
    std::string commandId;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == SlotSubmitCode::TransportQueued ||
               code == SlotSubmitCode::Admitted ||
               code == SlotSubmitCode::Existing ||
               code == SlotSubmitCode::AlreadyTerminal;
    }
};

enum class SlotActorCheckpoint {
    BeforeDeadlineLinearization,
    AfterDeadlineAdmission
};

class SlotTransitionActor {
public:
    using EffectSink =
        std::function<bool(const CommittedOccupancyTransition&)>;

    struct Config {
        std::size_t transportAdmissionCapacity{16};
        std::size_t durablePendingCapacity{100};
        std::chrono::milliseconds retryDelay{50};
        /**
         * 종결된 INBOX 행의 보존 기간. 0이면 정리를 하지 않는다.
         *
         * 이 테이블은 정리하지 않으면 무한히 커진다(시간당 약 184행). 쿼리
         * 플래너 통계가 있으면 인덱스가 비용을 가려주지만, 테이블이 페이지
         * 캐시를 넘어서면 다시 느려진다.
         * 근거: docs/PERFORMANCE_PROFILING_REPORT_1H.md
         */
        std::chrono::hours retentionPeriod{24 * 30};
        /** 정리 실행 간격. 매 반복마다 돌리지 않는다. */
        std::chrono::minutes purgeInterval{60};
        /** 1회 정리에서 지울 최대 행 수. 잠금 보유 시간을 제한한다. */
        std::size_t purgeBatchSize{2000};
        std::function<void(SlotActorCheckpoint)> checkpoint;
    };

    SlotTransitionActor(database::SessionTransitionStore& store,
                        Config config,
                        EffectSink effectSink);
    ~SlotTransitionActor();

    SlotTransitionActor(const SlotTransitionActor&) = delete;
    SlotTransitionActor& operator=(const SlotTransitionActor&) = delete;

    bool start();
    SlotSubmitResult submit(const SlotTransitionCommand& command);
    void closeIngress();
    bool stopAndDrain(std::chrono::milliseconds timeout);
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);
    [[nodiscard]] bool ingressOpen() const;

private:
    struct IngressItem {
        SlotTransitionCommand command;
        std::uint64_t transportOrdinal{};
        std::int64_t nextAttemptAtEpochMs{};
    };

    void run();
    bool processIngress(std::int64_t nowEpochMs);
    bool processRunnable(std::int64_t nowEpochMs);
    bool processDueDeadlines(std::int64_t nowEpochMs);
    bool processEffects(std::int64_t nowEpochMs);
    /**
     * @brief 정리 간격이 지났으면 종결 INBOX 행을 한 배치 삭제한다.
     *
     * 워커 스레드에서만 호출되므로 별도 동기화가 필요 없다. 진행 여부를
     * 반환하지 않는다 — 정리는 액터의 작업 진행(progress)이 아니므로
     * 폴링 대기를 건너뛰게 만들면 안 된다.
     */
    void purgeSettledIfDue(std::int64_t nowEpochMs);
    [[nodiscard]] static std::int64_t nowEpochMs();

    database::SessionTransitionStore& store_;
    Config config_;
    EffectSink effect_sink_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idle_condition_;
    std::thread worker_;
    std::unordered_map<std::string, std::deque<IngressItem>>
        ingress_by_slot_;
    std::unordered_map<std::string, std::string> ingress_identity_index_;
    std::size_t ingress_size_{};
    std::uint64_t next_transport_ordinal_{};
    /** 마지막 정리 시각(epoch ms). 0이면 아직 한 번도 돌지 않았다. */
    std::int64_t last_purge_epoch_ms_{};
    bool admitting_{};
    bool deadline_admitting_{};
    bool started_{};
    bool ingress_open_{};
    bool stop_requested_{};
    bool processing_{};
    std::int64_t drain_cutoff_epoch_ms_{};
};

}  // namespace parking

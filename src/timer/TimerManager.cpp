#include "parking_timer/TimerManager.hpp"

#include "parking_timer/Types.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace parking_timer {

/**
 * @brief `priority_queue`에서 더 늦은 항목의 우선순위를 낮추는 비교 연산자.
 *
 * @param[in] left 비교할 왼쪽 타이머 항목.
 * @param[in] right 비교할 오른쪽 타이머 항목.
 * @return `left`가 `right`보다 나중에 처리되어야 하면 `true`.
 * @note C++ `priority_queue`는 기본적으로 최대 힙이므로 비교 방향을 뒤집어 가장 이른
 *       deadline을 top에 둔다. deadline이 같으면 작은 sequence가 먼저 온다.
 */
bool TimerManager::LaterDeadline::operator()(const TimerItem& left,
                                             const TimerItem& right) const noexcept {
    if (left.deadline != right.deadline) {
        return left.deadline > right.deadline;
    }
    return left.sequence > right.sequence;
}

/**
 * @brief DB와 callback을 연결하고 단일 타이머 worker 스레드를 시작한다.
 *
 * @param[in,out] database 만료 시 로그 상태를 바꿀 SQLite 접근 객체.
 * @param[in] callback 위반 확정 후 호출할 callback.
 * @param[in] error_callback worker 오류를 보고할 선택 callback.
 * @param[in,out] transition_mutex 입차·출차·만료 전이를 직렬화할 선택 mutex 포인터.
 * @throws std::system_error worker 스레드를 생성할 수 없는 경우.
 * @note 참조와 mutex는 이 `TimerManager`보다 오래 살아 있어야 한다.
 */
TimerManager::TimerManager(EventDatabase& database,
                           ViolationCallback callback,
                           ErrorCallback error_callback,
                           std::mutex* transition_mutex)
    : database_(database),
      callback_(std::move(callback)),
      error_callback_(std::move(error_callback)),
      transition_mutex_(transition_mutex) {
    // ctor body에 도달한 뒤 worker를 시작해야 stopping_/queue_/callback을 초기화 전에
    // 읽는 경쟁을 피할 수 있다. worker 최외곽에서도 예외가 프로세스를 끝내지 못하게 한다.
    worker_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& error) {
            reportError(TimerItem{}, error.what());
        } catch (...) {
            reportError(TimerItem{}, "unknown timer worker loop failure");
        }
    });
}

/**
 * @brief worker에 종료를 알리고 스레드가 완전히 끝날 때까지 기다린다.
 *
 * @note stop flag 설정 → condition variable 깨우기 → join 순서를 지켜 소멸 후 callback이
 *       외부 객체를 참조하는 일을 막는다. 큐의 미만료 항목은 소멸 시 처리하지 않는다.
 */
TimerManager::~TimerManager() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

/**
 * @brief 주차 세션의 만료시각을 계산해 최소 우선순위 큐에 등록한다.
 *
 * @param[in] log_id `timer_log`의 불변 세션 ID.
 * @param[in] slot_id 주차면 ID.
 * @param[in] car_number 차량번호. 큐 항목이 자체 소유하도록 값으로 받는다.
 * @param[in] delay 현재부터 만료까지의 단조시간 간격.
 * @throws std::invalid_argument `delay`가 0 이하인 경우.
 * @throws std::runtime_error 관리자가 이미 종료 중인 경우.
 */
void TimerManager::schedule(const std::int64_t log_id,
                            std::string slot_id,
                            std::string car_number,
                            const std::chrono::milliseconds delay) {
    if (delay <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("timer delay must be positive");
    }

    // system_clock 보정/NTP 변경에 흔들리지 않도록 deadline은 steady_clock으로만 계산한다.
    TimerItem item{Clock::now() + delay, 0, log_id, std::move(slot_id),
                   std::move(car_number), 0, {}};
    bool became_earliest{};
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("TimerManager is stopping");
        }
        item.sequence = next_sequence_++;
        became_earliest = queue_.empty() || item.deadline < queue_.top().deadline;
        queue_.push(std::move(item));
    }
    // 기존 top보다 빠른 항목만 현재 wait_until의 목표를 바꾸므로 이때만 worker를 깨운다.
    if (became_earliest) {
        cv_.notify_one();
    }
}

/**
 * @brief 실제 우선순위 큐에 남아 있는 노드 수를 반환한다.
 *
 * @return 활성 노드, DB 재시도 노드, 아직 만료되지 않은 lazy-canceled 노드를 모두 포함한 수.
 */
std::size_t TimerManager::pendingCount() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

/**
 * @brief 가장 이른 deadline만 기다리며 만료 노드를 처리하는 worker 루프.
 *
 * @note 큐가 비면 무기한 대기하고, 항목이 있으면 top의 deadline까지 `wait_until()`한다.
 *       DB 작업 중에는 큐 mutex를 풀어 다른 스레드의 `schedule()`을 막지 않는다.
 */
void TimerManager::run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (queue_.empty()) {
            // polling하지 않고 첫 타이머 등록 또는 종료 신호가 올 때까지 잠든다.
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            continue;
        }

        const auto deadline = queue_.top().deadline;
        const auto sequence = queue_.top().sequence;
        // 기다리던 top을 캡처해 두고, 더 빠른 top이 새로 생긴 경우에만 대기 목표를 재계산한다.
        // predicate 사용으로 spurious wake-up도 안전하게 다시 대기한다.
        cv_.wait_until(lock, deadline, [this, deadline, sequence] {
            return stopping_ || queue_.empty() ||
                   queue_.top().deadline < deadline ||
                   (queue_.top().deadline == deadline &&
                    queue_.top().sequence < sequence);
        });
        if (stopping_) {
            break;
        }

        // 한 번 깨어났을 때 이미 만료된 동시 deadline 항목을 모두 연속으로 비운다.
        while (!queue_.empty() && queue_.top().deadline <= Clock::now()) {
            TimerItem expired = queue_.top();
            queue_.pop();
            // SQLite I/O와 사용자 callback이 느려도 새 입차 타이머 등록은 계속 가능해야 한다.
            lock.unlock();
            processExpired(std::move(expired));
            lock.lock();
            if (stopping_) {
                break;
            }
        }
    }
}

/**
 * @brief 만료 노드를 DB 조건부 UPDATE로 검증하고 위반 callback을 발생시킨다.
 *
 * @param[in] item 큐에서 제거된 만료 타이머 항목.
 * @note 출차와 동시에 실행되면 공용 전이 mutex를 먼저 얻은 처리가 우선한다. DB UPDATE가
 *       0행인 경우는 이미 출차·취소된 정상 lazy deletion 경로이므로 callback하지 않는다.
 */
void TimerManager::processExpired(TimerItem item) {
    try {
        std::unique_lock<std::mutex> transition_lock;
        if (transition_mutex_ != nullptr) {
            // 입차/출차의 DB 변경과 위반 UPDATE·이벤트 발행을 하나의 관찰 가능한 순서로 만든다.
            transition_lock = std::unique_lock<std::mutex>{*transition_mutex_};
        }

        // DB 장애로 재시도하더라도 실제 최초 deadline 시각이 복구 시각으로 바뀌지 않게 고정한다.
        if (item.violation_at.empty()) {
            item.violation_at = utcNow();
        }
        const auto image_path = "snapshots/" + item.car_number + "_" +
                                std::to_string(item.log_id) + "_violation.jpg";

        bool marked{};
        try {
            // 출차된 로그는 is_canceled=1이므로 UPDATE 대상이 0행이 된다.
            // 이것이 명세의 log-id 기반 lazy deletion이다.
            marked = database_.markViolation(item.log_id, item.violation_at, image_path);
        } catch (const std::exception& error) {
            // 재큐와 오류 발행 동안 다른 입출차까지 막지 않도록 외부 전이 lock을 먼저 놓는다.
            if (transition_lock.owns_lock()) {
                transition_lock.unlock();
            }
            retryAfterDatabaseError(std::move(item), error.what());
            return;
        } catch (...) {
            if (transition_lock.owns_lock()) {
                transition_lock.unlock();
            }
            retryAfterDatabaseError(std::move(item), "unknown SQLite failure");
            return;
        }
        if (!marked) {
            // 출차가 먼저 is_canceled=1로 만들었다면 이것이 의도한 lazy deletion 결과다.
            return;
        }

        if (callback_) {
            callback_(ViolationEvent{item.log_id, item.slot_id, item.car_number,
                                     item.violation_at, image_path});
        }
    } catch (const std::exception& error) {
        reportError(item, error.what());
    } catch (...) {
        reportError(item, "unknown timer worker failure");
    }
}

/**
 * @brief 일시적인 SQLite 오류가 난 타이머를 지수 backoff로 다시 예약한다.
 *
 * @param[in] item DB 갱신에 실패한 만료 항목. 최초 위반 시각을 그대로 보존한다.
 * @param[in] message 오류 callback에 전달할 SQLite 오류 설명.
 * @note 재시도 간격은 1, 2, 4, 8, 16, 32초이며 이후 32초로 포화된다.
 *       모든 내부 예외를 흡수하므로 worker 스레드 밖으로 예외를 던지지 않는다.
 */
void TimerManager::retryAfterDatabaseError(TimerItem item, std::string message) noexcept {
    reportError(item, "violation DB update failed; retry scheduled: " + message);
    try {
        // 일시적인 busy/I/O 장애가 위반 판정을 영구 유실시키지 않도록 1,2,4,8,16,32초
        // 이후에는 32초 간격으로 재시도한다. 출차된 행은 복구 후 조건부 UPDATE 0행으로
        // 자연스럽게 lazy-delete 된다.
        const auto exponent = std::min(item.retry_count, std::uint32_t{5});
        const auto retry_delay = std::chrono::seconds{std::int64_t{1} << exponent};
        // retry_count를 5에서 포화시켜 장기 장애에서도 shift와 간격이 계속 커지지 않게 한다.
        if (item.retry_count < std::uint32_t{5}) {
            ++item.retry_count;
        }
        item.deadline = Clock::now() + retry_delay;

        bool became_earliest{};
        {
            std::lock_guard lock(mutex_);
            // 종료가 시작됐다면 소멸을 지연시키는 새 노드를 만들지 않는다.
            if (stopping_) {
                return;
            }
            item.sequence = next_sequence_++;
            became_earliest = queue_.empty() || item.deadline < queue_.top().deadline;
            queue_.push(std::move(item));
        }
        // 재시도 노드가 새 top이 된 경우 기존 wait_until을 다시 계산시킨다.
        if (became_earliest) {
            cv_.notify_one();
        }
    } catch (const std::exception& error) {
        reportError(item, std::string{"failed to requeue timer: "} + error.what());
    } catch (...) {
        reportError(item, "failed to requeue timer: unknown error");
    }
}

/**
 * @brief 타이머 오류를 등록된 callback 또는 표준 오류 출력으로 안전하게 보고한다.
 *
 * @param[in] item 오류가 발생한 타이머 문맥. 루프 자체 오류면 기본값일 수 있다.
 * @param[in] message 오류 설명.
 * @note 오류 callback 자체가 예외를 던져도 흡수해 `std::terminate`를 방지한다.
 */
void TimerManager::reportError(const TimerItem& item, std::string message) noexcept {
    try {
        if (error_callback_) {
            error_callback_(TimerError{item.log_id, item.slot_id, item.car_number,
                                       std::move(message)});
            return;
        }
        std::cerr << "timer worker error: " << message << '\n';
    } catch (...) {
        // 어떤 사용자 callback도 worker thread 밖으로 예외를 전파해 std::terminate를
        // 일으키지 못하게 하는 최종 예외 경계다.
        std::cerr << "timer worker error callback failed\n";
    }
}

}  // namespace parking_timer

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace app {

class CallbackLeaseGate {
private:
    struct State {
        mutable std::mutex mutex;
        std::condition_variable condition;
        bool open{true};
        std::size_t active{};
    };

public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), active_(other.active_) {
            other.active_ = false;
        }

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) return *this;
            release();
            state_ = std::move(other.state_);
            active_ = other.active_;
            other.active_ = false;
            return *this;
        }

        ~Lease() { release(); }

        explicit operator bool() const noexcept { return active_; }

    private:
        friend class CallbackLeaseGate;
        explicit Lease(std::shared_ptr<State> state)
            : state_(std::move(state)), active_(true) {}

        void release() noexcept {
            if (!active_ || !state_) return;
            std::lock_guard lock(state_->mutex);
            --state_->active;
            active_ = false;
            if (!state_->open && state_->active == 0)
                state_->condition.notify_all();
        }

        std::shared_ptr<State> state_;
        bool active_{false};
    };

    CallbackLeaseGate() : state_(std::make_shared<State>()) {}

    std::optional<Lease> tryAcquire() const {
        std::lock_guard lock(state_->mutex);
        if (!state_->open) return std::nullopt;
        ++state_->active;
        return Lease(state_);
    }

    void close() noexcept {
        std::lock_guard lock(state_->mutex);
        state_->open = false;
        if (state_->active == 0) state_->condition.notify_all();
    }

    void waitForDrained() noexcept {
        std::unique_lock lock(state_->mutex);
        state_->condition.wait(lock, [this] { return state_->active == 0; });
    }

    [[nodiscard]] bool waitForDrainedFor(
        const std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(state_->mutex);
        return state_->condition.wait_for(
            lock, timeout, [this] { return state_->active == 0; });
    }

    void closeAndWait() noexcept {
        close();
        waitForDrained();
    }

    [[nodiscard]] bool isOpen() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->open;
    }

    [[nodiscard]] std::size_t activeCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->active;
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace app

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// Nested loaders append their work to the same ordered job. Inputs captured by
// those callbacks must be owned by the job or by the callback itself.
using LoadStep = std::function<bool()>;
using EnqueueLoadStep = std::function<void(LoadStep)>;

// A step that is waiting on something outside the job (a worker thread) calls this after re-queuing itself: run_for
// then ends the pump after that step instead of running it again, so the frame is not held for the rest of its budget.
// Main thread only (one flag per thread, shared by every LoadJob type); the work and its order are unchanged, only the
// frame the next step runs in.
inline bool& load_yield_requested() noexcept {
    static thread_local bool requested = false;
    return requested;
}
inline void yield_load_frame() noexcept { load_yield_requested() = true; }

// Main-thread load callbacks retain their inputs until completion or cancellation.
// A callback is indivisible; a time budget only yields between callbacks. While the player's input is pending
// run_for starts no callback at all (bounded, so a long interaction cannot starve the load): the work and its order
// are unchanged, only the frames it runs in.
template<class Context>
class LoadJob final {
public:
    using Step = std::function<bool(Context&)>;
    // Reports each finished callback's ordinal (0-based, in execution order) and duration; unset costs nothing.
    using StepObserver = std::function<void(std::size_t, std::chrono::steady_clock::duration)>;
    // Consecutive input-deferred pumps after which one callback runs anyway.
    static constexpr std::size_t max_deferred_pumps = 300;
    enum class Status { pending, complete, failed };

    LoadJob(std::unique_ptr<Context> context, std::vector<Step> steps)
        : context_(std::move(context)), steps_(steps.begin(), steps.end()) {}
    LoadJob(const LoadJob&) = delete;
    LoadJob& operator=(const LoadJob&) = delete;

    [[nodiscard]] Status status() const noexcept { return status_; }

    void observe_steps(StepObserver observer) { observer_ = std::move(observer); }

    void append(Step step) {
        if (!context_) return;
        // Child steps run before the caller's continuation. The current callback
        // has been removed from the deque, so insertion cannot invalidate it.
        if (running_) {
            steps_.insert(steps_.begin() + static_cast<std::ptrdiff_t>(inserted_), std::move(step));
            ++inserted_;
        } else {
            steps_.push_back(std::move(step));
        }
    }

    [[nodiscard]] Status advance() {
        if (status_ != Status::pending) return status_;
        if (!context_) return finish(Status::failed);
        if (steps_.empty()) return finish(Status::complete);
        // Also terminal if a callback throws: a partially performed step is never retried.
        status_ = Status::failed;
        auto step = std::move(steps_.front());
        steps_.pop_front();
        inserted_ = 0;
        running_ = true;
        bool succeeded = false;
        const auto started = observer_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const std::size_t ordinal = executed_++;
        try {
            succeeded = step(*context_);
        } catch (...) {
            running_ = false;
            step = {};
            finish(Status::failed);
            throw;
        }
        running_ = false;
        step = {};
        if (observer_) observer_(ordinal, std::chrono::steady_clock::now() - started);
        if (!succeeded) return finish(Status::failed);
        status_ = Status::pending;
        return steps_.empty() ? finish(Status::complete) : status_;
    }

    // `input_pending`: the player is interacting now. No callback starts for up to max_deferred_pumps calls in a row.
    [[nodiscard]] Status run_for(const std::chrono::steady_clock::duration budget, const bool input_pending = false) {
        if (status_ != Status::pending) return status_;
        if (input_pending && deferred_ < max_deferred_pumps) {
            ++deferred_;
            return status_;
        }
        deferred_ = 0;
        load_yield_requested() = false;
        const auto start = std::chrono::steady_clock::now();
        do {
            if (advance() != Status::pending) return status_;
            if (load_yield_requested()) {
                load_yield_requested() = false;
                return status_;
            }
        } while (std::chrono::steady_clock::now() - start < budget);
        return status_;
    }

    [[nodiscard]] bool run_to_completion() {
        while (advance() == Status::pending) {}
        return status_ == Status::complete;
    }

private:
    Status finish(const Status status) {
        status_ = status;
        steps_.clear();
        context_.reset();
        return status_;
    }

    // Callbacks are destroyed before the storage they may reference.
    std::unique_ptr<Context> context_;
    std::deque<Step> steps_;
    StepObserver observer_;
    std::size_t inserted_{};
    std::size_t executed_{};
    std::size_t deferred_{};
    bool running_{};
    Status status_{Status::pending};
};

} // namespace eawr::presentation::godot_backend

#pragma once

#include "upload_identity.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// Pure CPU jobs: either the complete canonical upload bytes, or a job that owns shared immutable snapshots and
// evaluates the identity itself (UploadIdentityInputs). No renderer state or Godot resources cross the worker
// boundary. Retained futures own only a digest, including on MSVC where packaged-task captures can survive: a job
// drops its inputs before its digest becomes observable. A job that throws (an allocation failing while it builds the
// identity, say) does not end the worker or the process: the exception is stored in the job's future, so the caller's
// get() rethrows it where the identity is read, and the pool keeps serving.
class UploadIdentityPool final {
public:
    struct Stats final {
        std::size_t pending{};
        std::size_t input_capacity{};
        std::size_t peak_input_capacity{};
        std::size_t peak_pending{};
    };

    explicit UploadIdentityPool(const std::size_t workers = 4,
        const std::size_t byte_budget = 64U * 1024U * 1024U,
        const std::size_t job_budget = 256)
        : byte_budget_(byte_budget), job_budget_(job_budget) {
        if (workers == 0 || byte_budget == 0 || job_budget == 0)
            throw std::invalid_argument("upload identity pool requires positive limits");
        threads_.reserve(workers);
        try {
            for (std::size_t index = 0; index < workers; ++index)
                threads_.emplace_back([this] { run(); });
        } catch (...) {
            stop();
            throw;
        }
    }

    ~UploadIdentityPool() { stop(); }
    UploadIdentityPool(const UploadIdentityPool&) = delete;
    UploadIdentityPool& operator=(const UploadIdentityPool&) = delete;

    // `bounded` false: the job is accepted at once even over the admission budget (play-time uploads never wait here).
    [[nodiscard]] std::shared_future<UploadIdentity> submit(std::vector<std::uint8_t> bytes, const bool bounded = true) {
        return enqueue(std::make_shared<Work>(std::move(bytes)), bounded);
    }

    // A job that evaluates its own identity; `inputs->cost` (the input bytes it keeps alive) is its admission size.
    [[nodiscard]] std::shared_future<UploadIdentity> submit(std::shared_ptr<const UploadIdentityInputs> inputs, const bool bounded = true) {
        const std::size_t cost = inputs ? inputs->cost : 0U;
        return submit_job([held = std::move(inputs)] { return held->evaluate(); }, cost, bounded);
    }

    // Any pure-CPU job with the bytes it keeps alive as its admission size; what the job captures is dropped before its
    // digest is observable, and an exception it throws is delivered through the future.
    template <typename Job>
    [[nodiscard]] std::shared_future<UploadIdentity> submit_job(Job&& job, const std::size_t cost, const bool bounded = true) {
        return enqueue(std::make_shared<Work>(std::make_unique<JobHolder<std::decay_t<Job>>>(std::forward<Job>(job)), cost), bounded);
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(mutex_);
        return {pending_, input_capacity_, peak_input_capacity_, peak_pending_};
    }

private:
    // Type-erased job; a small holder here instead of std::function so the header needs no <functional>.
    struct JobBase {
        virtual ~JobBase() = default;
        virtual UploadIdentity run() = 0;
    };
    template <typename Callable>
    struct JobHolder final : JobBase {
        explicit JobHolder(Callable&& callable) : callable_(std::move(callable)) {}
        explicit JobHolder(const Callable& callable) : callable_(callable) {}
        UploadIdentity run() override { return callable_(); }
        Callable callable_;
    };

    struct Work final {
        explicit Work(std::vector<std::uint8_t> input) : bytes(std::move(input)), cost(bytes.capacity()) {}
        Work(std::unique_ptr<JobBase> work, const std::size_t declared_cost)
            : job(std::move(work)), cost(declared_cost) {}
        std::vector<std::uint8_t> bytes;
        std::unique_ptr<JobBase> job;
        std::size_t cost{};
        std::promise<UploadIdentity> result;
    };

    [[nodiscard]] std::shared_future<UploadIdentity> enqueue(std::shared_ptr<Work> work, const bool bounded = true) {
        auto result = work->result.get_future().share();
        const std::size_t capacity = work->cost;
        {
            std::unique_lock lock(mutex_);
            space_.wait(lock, [&] {
                // One input larger than the budget runs alone. Count empty
                // jobs too, so metadata cannot grow without a bound.
                return stopping_ || !bounded || pending_ == 0
                    || (pending_ < job_budget_ && input_capacity_ <= byte_budget_
                        && capacity <= byte_budget_ - input_capacity_);
            });
            if (stopping_) throw std::logic_error("upload identity pool is stopping");
            // Queue allocation may throw; publish accounting only after it
            // succeeds, while workers still cannot observe the new job.
            work_.push_back(std::move(work));
            ++pending_;
            peak_pending_ = std::max(peak_pending_, pending_);
            input_capacity_ += capacity;
            peak_input_capacity_ = std::max(peak_input_capacity_, input_capacity_);
        }
        ready_.notify_one();
        return result;
    }

    void run() {
        for (;;) {
            std::shared_ptr<Work> work;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !work_.empty(); });
                if (work_.empty()) return;
                work = std::move(work_.front());
                work_.pop_front();
            }
            const std::size_t capacity = work->cost;
            std::optional<UploadIdentity> digest;
            std::exception_ptr failure;
            try {
                digest = work->job ? work->job->run() : core::sha256(work->bytes);
            } catch (...) {
                failure = std::current_exception();
            }
            // Release both consumed capacity and queue admission before the
            // digest becomes observable. A discarded future is safe too.
            std::vector<std::uint8_t>().swap(work->bytes);
            work->job = nullptr;
            {
                std::lock_guard lock(mutex_);
                input_capacity_ -= capacity;
                --pending_;
            }
            space_.notify_all();
            if (failure) work->result.set_exception(failure);
            else work->result.set_value(*digest);
        }
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        space_.notify_all();
        for (std::thread& thread : threads_) thread.join();
    }

    const std::size_t byte_budget_;
    const std::size_t job_budget_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<std::shared_ptr<Work>> work_;
    std::vector<std::thread> threads_;
    bool stopping_{};
    std::size_t pending_{};
    std::size_t input_capacity_{};
    std::size_t peak_input_capacity_{};
    std::size_t peak_pending_{};
};

} // namespace eawr::presentation::godot_backend::detail

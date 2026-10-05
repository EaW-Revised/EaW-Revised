#pragma once

#include "upload_identity.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// Pure CPU jobs own the complete canonical upload bytes. No asset pointers,
// renderer state or Godot resources cross the worker boundary. Retained futures
// own only a digest, including on MSVC where packaged-task captures can survive.
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

    [[nodiscard]] std::shared_future<UploadIdentity> submit(std::vector<std::uint8_t> bytes) {
        auto work = std::make_shared<Work>(std::move(bytes));
        auto result = work->result.get_future().share();
        const std::size_t capacity = work->bytes.capacity();
        {
            std::unique_lock lock(mutex_);
            space_.wait(lock, [&] {
                // One input larger than the budget runs alone. Count empty
                // jobs too, so metadata cannot grow without a bound.
                return stopping_ || pending_ == 0
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

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(mutex_);
        return {pending_, input_capacity_, peak_input_capacity_, peak_pending_};
    }

private:
    struct Work final {
        explicit Work(std::vector<std::uint8_t> input) : bytes(std::move(input)) {}
        std::vector<std::uint8_t> bytes;
        std::promise<UploadIdentity> result;
    };

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
            const std::size_t capacity = work->bytes.capacity();
            const UploadIdentity digest = core::sha256(work->bytes);
            // Release both consumed capacity and queue admission before the
            // digest becomes observable. A discarded future is safe too.
            std::vector<std::uint8_t>().swap(work->bytes);
            {
                std::lock_guard lock(mutex_);
                input_capacity_ -= capacity;
                --pending_;
            }
            space_.notify_all();
            work->result.set_value(digest);
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

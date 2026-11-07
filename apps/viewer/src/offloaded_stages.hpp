#pragma once

#include "eawr/core/load_profile.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// The first stages of a battle preparation run back to back on ONE worker thread; the main thread polls. The stages
// are the same functions in the same order on the same inputs as the inline path, so the load's results do not change,
// only the thread. Contract for a stage run here: it touches no engine object and no state the main thread reads before
// the job is joined (its outputs are read only after poll() returned true, which joins the worker). Engine-free.
class OffloadedStages final {
public:
    using Stage = std::function<bool(std::string&)>;
    struct StageTime final { double wall_ms{}, cpu_ms{}; bool ran{}; };

    // One stage with the exception semantics of the whole load: a throw is a failed stage ("load stage threw: ..."), never
    // an escape. The worker needs it (an uncaught exception there is std::terminate); the inline steps use the same
    // function so both paths surface the same failure the same way (the attract candidate fails, the runner moves on).
    [[nodiscard]] static bool run_guarded(const Stage& stage, std::string& failure) {
        try {
            return stage(failure);
        } catch (const std::exception& error) {
            failure = std::string("load stage threw: ") + error.what();
        } catch (...) {
            failure = "load stage threw";
        }
        return false;
    }

    // `cancel` is the flag the stages' own long loops check (units::LoadInput::cancel); the destructor sets it, so a
    // dropped load joins within one unit type instead of at the end of the stage. `thread_cpu_ms` (optional) returns
    // the calling thread's CPU time. `record`: the worker keeps its own load_profile recording (merge() it afterwards).
    OffloadedStages(std::vector<Stage> stages, std::shared_ptr<std::atomic<bool>> cancel, const bool record,
                    const bool detailed, std::function<double()> thread_cpu_ms = {})
        : stages_(std::move(stages)), cancel_(std::move(cancel)), thread_cpu_ms_(std::move(thread_cpu_ms)),
          times_(stages_.size()) {
        if (record) {
            recording_.emplace();
            recording_->detailed = detailed;
        }
        if (!cancel_) cancel_ = std::make_shared<std::atomic<bool>>(false);
    }
    OffloadedStages(const OffloadedStages&) = delete;
    OffloadedStages& operator=(const OffloadedStages&) = delete;
    ~OffloadedStages() {
        cancel_->store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool launched() const noexcept { return launched_; }

    // Main thread, once. Starts the worker.
    void launch() {
        if (launched_) return;
        launched_ = true;
        thread_ = std::thread([this] { run(); });
    }

    // Main thread. Waits up to `wait` for the worker (a condition variable, no spinning; zero = a non-blocking check);
    // true once it has finished and been joined. Everything below is valid only after that.
    [[nodiscard]] bool poll(const std::chrono::microseconds wait) {
        if (joined_) return true;
        {
            std::unique_lock lock(mutex_);
            if (!finished_.wait_for(lock, wait, [this] { return done_; })) return false;
        }
        if (thread_.joinable()) thread_.join();
        joined_ = true;
        return true;
    }

    [[nodiscard]] bool ok() const noexcept { return failed_stage_ == none; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] std::size_t failed_stage() const noexcept { return failed_stage_; }
    [[nodiscard]] const std::vector<StageTime>& times() const noexcept { return times_; }
    [[nodiscard]] const core::load_profile::Recording* recording() const noexcept {
        return recording_ ? &*recording_ : nullptr;
    }
    static constexpr std::size_t none = static_cast<std::size_t>(-1);

private:
    void run() noexcept {
        std::size_t index = 0;
        try {
            core::load_profile::active = recording_ ? &*recording_ : nullptr;
            {
                core::load_profile::Scope scope(core::load_profile::Phase::session);
                for (; index < stages_.size(); ++index) {
                    if (cancel_->load(std::memory_order_acquire)) {
                        failed_stage_ = index;
                        failure_ = "load cancelled";
                        break;
                    }
                    const auto started = std::chrono::steady_clock::now();
                    const double cpu_before = thread_cpu_ms_ ? thread_cpu_ms_() : 0.0;
                    const bool succeeded = run_guarded(stages_[index], failure_);
                    auto& time = times_[index];
                    time.ran = true;
                    time.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    time.cpu_ms = thread_cpu_ms_ ? thread_cpu_ms_() - cpu_before : 0.0;
                    if (!succeeded) {
                        failed_stage_ = index;
                        if (failure_.empty()) failure_ = "load stage failed";
                        break;
                    }
                }
            }
        } catch (const std::exception& error) {
            failed_stage_ = index;
            failure_ = std::string("load stage threw: ") + error.what();
        } catch (...) {
            failed_stage_ = index;
            failure_ = "load stage threw";
        }
        core::load_profile::active = nullptr;
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        finished_.notify_all();
    }

    std::vector<Stage> stages_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::function<double()> thread_cpu_ms_;
    std::vector<StageTime> times_;            // written by the worker; read after the join
    std::optional<core::load_profile::Recording> recording_;
    std::string failure_;                     // written by the worker before done_; read after the join
    std::size_t failed_stage_{none};
    std::mutex mutex_;
    std::condition_variable finished_;
    bool done_{};                             // guarded by mutex_
    bool launched_{}, joined_{};              // main thread only
    std::thread thread_;                      // last: destroyed (and joined by ~OffloadedStages) first
};

} // namespace eawr::presentation::godot_backend

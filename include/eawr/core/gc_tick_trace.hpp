#pragma once

// Opt-in per-tick trace of the galactic live session (#6933 hitch hunt). Off unless EAWR_GC_TICK_TRACE=<path> is set:
// then gc_profile's scopes run (see gc_profile.hpp) and every completed tick appends one JSON line with the tick's
// total/command-source/session-step wall time, the wall time since the first traced tick and the per-tick delta of every
// gc_profile phase that ran. Process-local diagnostics: never saved, hashed or read by a decision. When off, the cost is
// one cached bool test per tick. When on, the row is built under the trace's own small mutex and handed to a writer thread:
// no file write or flush ever runs on the simulation thread or under the session mutex.

#include "eawr/core/gc_profile.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#include <share.h>
#endif

namespace eawr::gc_profile {

class TickTrace final {
public:
    // The process-wide trace is allocated once and never destroyed: no static destructor runs, so nothing joins the writer
    // thread during static teardown (#7109; teardown order and a thread already gone at exit made that a hang/crash risk).
    // A run that opened the trace ends it with an explicit stop() (sim_headless does); a trace still open at process exit
    // loses at most the rows queued since the writer's last batch, which is acceptable for an opt-in diagnostic.
    static TickTrace& instance() {
        static TickTrace* const trace = new TickTrace();
        return *trace;
    }
    // A trace that ignores the environment, for tests that scope their own instance.
    struct NoEnvironment {};
    explicit TickTrace(NoEnvironment) {}
    // After an explicit stop() the writer is already joined, so destroying the trace joins nothing.
    ~TickTrace() { stop(); }
    TickTrace(const TickTrace&) = delete;
    TickTrace& operator=(const TickTrace&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_relaxed); }
    // True while a writer thread exists (opened and not yet stopped).
    [[nodiscard]] bool running() const {
        const std::lock_guard lock(mutex_);
        return writer_running_;
    }
    // The same switch as the environment variable, for a launcher that cannot set one (sim_headless --gc-tick-trace).
    bool open(const std::string& path) {
        if (path.empty()) return false;
        stop();
        std::FILE* file = open_file(path);
        if (!file) return false;
        start(file);
        enabled.store(true, std::memory_order_relaxed);
        return true;
    }

    // Ends the trace and writes what is queued (tests and tools that scope it); the profile scopes keep their own switch.
    void stop() {
        {
            const std::lock_guard lock(mutex_);
            if (!writer_running_ || closing_) return; // no writer, or another stop() is already ending it
            closing_ = true;
            active_.store(false, std::memory_order_relaxed);
        }
        wake_.notify_one();
        writer_.join(); // only the stop() that set closing_ reaches this line; flush() never touches the thread object
        {
            const std::lock_guard lock(mutex_);
            writer_running_ = false;
        }
        drained_.notify_all();
    }

    // Waits until every queued row is written and flushed, and leaves the trace open (a session end: the next session in
    // the same process keeps tracing). Safe to call with no writer; never called from a path that holds the trace's mutex.
    // The wait is bounded (5 s): a writer stuck in a write (full disk, stalled share) must not hold a session end or the
    // quit of the viewer; false means the rows were not all written in time (a diagnostic trace, so they are dropped).
    bool flush() {
        std::unique_lock lock(mutex_);
        return drained_.wait_for(lock, std::chrono::seconds(5), [&] { return !writer_running_ || (queue_.empty() && !writing_); });
    }

    void record(const std::uint64_t tick, const double total_ms, const double source_ms, const double step_ms) {
        if (!active()) return;
        {
            const std::lock_guard lock(mutex_);
            if (closing_) return;
            const auto now = std::chrono::steady_clock::now();
            if (!started_) { started_ = true; origin_ = now; previous_ = snapshot(); }
            const Snapshot current = snapshot();
            const Snapshot delta = difference(previous_, current);
            previous_ = current;
            const double at_ms = std::chrono::duration<double, std::milli>(now - origin_).count();
            char head[192];
            std::snprintf(head, sizeof(head), "{\"tick\":%llu,\"t_ms\":%.3f,\"total_ms\":%.3f,\"source_ms\":%.3f,\"step_ms\":%.3f,\"phases\":{",
                static_cast<unsigned long long>(tick), at_ms, total_ms, source_ms, step_ms);
            std::string line = head;
            constexpr auto cpu_begin = static_cast<std::size_t>(Phase::perception);
            bool first = true;
            bool cpu_open = false;
            for (std::size_t i = 0; i < delta.size(); ++i) {
                if (delta[i].calls == 0) continue;
                if (i >= cpu_begin && !cpu_open) { line += "},\"cpu_summed\":{"; first = true; cpu_open = true; }
                char item[160];
                std::snprintf(item, sizeof(item), "%s\"%.*s\":[%llu,%.3f,%.3f]", first ? "" : ",",
                    static_cast<int>(names[i].size()), names[i].data(), static_cast<unsigned long long>(delta[i].calls),
                    static_cast<double>(delta[i].inclusive_ns) / 1.0e6, static_cast<double>(delta[i].exclusive_ns) / 1.0e6);
                line += item;
                first = false;
            }
            line += "}}\n"; // the open object (phases or cpu_summed) and the row
            if (queue_.size() < max_queued_rows) queue_.push_back(std::move(line));
        }
        wake_.notify_one();
    }

private:
    static constexpr std::size_t max_queued_rows = 1U << 20;

    TickTrace() {
        std::string path;
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "EAWR_GC_TICK_TRACE") == 0 && value != nullptr) path = value;
        std::free(value);
#else
        if (const auto* value = std::getenv("EAWR_GC_TICK_TRACE")) path = value;
#endif
        if (path.empty()) return;
        if (std::FILE* file = open_file(path)) start(file);
    }
    static std::FILE* open_file(const std::string& path) {
        std::FILE* file = nullptr;
#ifdef _MSC_VER
        // fopen_s opens the file exclusively (no other handle may read it); _fsopen with a write-deny share mode lets a
        // reader tail the trace while the game runs and lets the flush check of a test read it.
        file = _fsopen(path.c_str(), "wb", _SH_DENYWR);
#else
        file = std::fopen(path.c_str(), "wb");
#endif
        return file;
    }
    void start(std::FILE* file) {
        {
            const std::lock_guard lock(mutex_);
            queue_.clear();
            closing_ = false;
            started_ = false;
            writer_running_ = true;
            active_.store(true, std::memory_order_relaxed);
        }
        writer_ = std::thread([this, file] { run(file); });
    }
    // Owns the file: drains the queue in batches, one flush per batch, then closes. Never holds the mutex while writing.
    void run(std::FILE* file) {
        std::deque<std::string> batch;
        for (;;) {
            bool done = false;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                batch.swap(queue_);
                done = closing_;
                writing_ = true;
            }
            for (const auto& line : batch) std::fwrite(line.data(), 1, line.size(), file);
            batch.clear();
            std::fflush(file);
            {
                const std::lock_guard lock(mutex_);
                writing_ = false;
            }
            drained_.notify_all();
            if (done) {
                const std::lock_guard lock(mutex_);
                if (queue_.empty()) break;
            }
        }
        std::fclose(file);
    }
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    bool writing_{}; // the writer holds a batch it has not flushed yet
    bool writer_running_{}; // a writer thread exists and has not been joined; guarded by mutex_ (the thread object is not read concurrently)
    std::thread writer_;
    std::deque<std::string> queue_;
    std::atomic<bool> active_{false};
    bool closing_{};
    bool started_{};
    std::chrono::steady_clock::time_point origin_{};
    Snapshot previous_{};
};

} // namespace eawr::gc_profile

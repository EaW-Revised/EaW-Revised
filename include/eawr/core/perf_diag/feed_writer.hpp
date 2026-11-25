#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "eawr/core/perf_diag/sim_feed.hpp"

namespace eawr::core::perf_diag {

// The local telemetry feed (design 5.1, P6): one writer thread drains the diagnostics rings of every attached session to
// a JSONL file and, on request, writes the last seconds of them as a Chrome trace-event capture. The record format is
// docs/performance/perf-feed.md; its version is feed_schema_version.
//
// Off (the default): no FeedWriter exists, so there is no thread, no file and no ring (SimFeed allocates its rings on
// the first set_enabled(true), which only an attach does). The sessions pay one atomic pointer load at construction.
//
// On: the writer thread runs at below-normal priority, wakes every FeedOptions::batch, reads the rings (copies of values
// the sessions already published; it never holds a session pointer, never reads a snapshot, never takes a session
// mutex) and writes one buffer per wake-up. A producer never waits for it: a full ring overwrites its oldest records and
// the loss shows up in the `dropped` records. No game thread does any file I/O.
inline constexpr int feed_schema_version = 1;

struct FeedOptions final {
    std::string directory;                 // UTF-8; created when missing; feed segments go here, captures in captures/
    std::string build;                     // game build id for the header record ("" = not given)
    std::string engine;                    // engine signature for the header record
    std::uint64_t rotate_bytes{64ULL << 20}; // a feed segment closes at this size ...
    std::size_t keep_segments{4};          // ... and only this many newest segments stay (256 MiB total by default)
    std::chrono::milliseconds batch{100};  // writer wake-up interval
    std::chrono::seconds capture_window{60}; // how much history a capture holds
    void (*capture_before_write)(void*){}; // optional observer on the writer thread after the source snapshot
    void* capture_before_write_context{};
    bool fine{};                           // switch fine phase sampling on in attached sessions
};

// One rendered frame, pushed by the frame side (the main thread) through push_frame(). The record carries numbers the
// viewer already measured; -1 marks "not measured" (no render-thread timing, no GPU time).
struct FrameRecord final {
    std::uint64_t frame{};
    std::uint64_t wall_us{};        // steady_clock microseconds at the end of the frame
    float frame_ms{};               // present-to-present wall time
    float main_ms{-1.0F};           // main thread work outside limiter sleeps
    float render_cpu_ms{-1.0F};
    float gpu_ms{-1.0F};
    float ui_lock_ms{};             // waited for the session mutex
    std::uint16_t flags{};          // bit 0: the presented tick was clamped at the newest tick (stale frame)
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(FrameRecord) == 48);

struct FrameFlags final {
    static constexpr std::uint16_t stale = 1;
};

class FeedWriter final {
public:
    explicit FeedWriter(FeedOptions options);
    ~FeedWriter();
    FeedWriter(const FeedWriter&) = delete;
    FeedWriter& operator=(const FeedWriter&) = delete;

    // Opens the first segment and starts the thread; this writer becomes the active one. False with `error` set when the
    // directory or file cannot be created (nothing runs then). One active writer per process.
    [[nodiscard]] bool start(std::string& error);
    // Final drain and flush, joins the thread, and stops being the active writer. Safe to call twice.
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // The process's own writer, for the host that turns telemetry on from a command-line flag or the settings file:
    // creates and starts it (false with `error` set on failure, nothing running then). Call stop_process() at the end
    // of the run, before static teardown. At most one per process.
    [[nodiscard]] static bool start_process(FeedOptions options, std::string& error);
    static void stop_process() noexcept;

    // --- process-wide entry points; no-ops (returning -1 / nothing) while no writer is active ---
    // True while a writer is active (one atomic load: the only cost a session pays when telemetry is off).
    [[nodiscard]] static bool active() noexcept;
    // Attaches a session's feed under a label ("tactical", "galactic"): the feed is enabled and drained until detached.
    // Returns the source id, or -1 without a writer. May briefly wait for one drain pass of the writer thread.
    [[nodiscard]] static int attach_active(SimFeed& feed, std::string_view label);
    // Final drain of that source, then the feed is switched off. The feed must still be alive.
    static void detach_active(int source) noexcept;
    // Asks the writer thread for a capture file of the last capture_window seconds; non-blocking, any thread.
    static void request_capture_active() noexcept;
    // Frame side producer (main thread only, one producer): ignored without a writer.
    static void push_frame_active(const FrameRecord& record) noexcept;

    // --- counters, any thread ---
    [[nodiscard]] std::uint64_t records_written() const noexcept { return records_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t captures_written() const noexcept { return captures_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t write_failures() const noexcept { return write_failures_.load(std::memory_order_relaxed); }
    // The feed segment currently open and the newest capture file (writer thread updates; read after stop() or for logs).
    [[nodiscard]] std::string current_segment() const; // UTF-8 path
    [[nodiscard]] std::string last_capture() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> records_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> captures_{0};
    std::atomic<std::uint64_t> write_failures_{0};
};

// Member of a session's implementation, declared right after its SimFeed (so it is destroyed first): attaches the feed to
// the active writer, if any, and detaches it again. Without a writer both calls are one atomic load.
class FeedAttachment final {
public:
    FeedAttachment(SimFeed& feed, const std::string_view label) : source_(FeedWriter::attach_active(feed, label)) {}
    ~FeedAttachment() { FeedWriter::detach_active(source_); }
    FeedAttachment(const FeedAttachment&) = delete;
    FeedAttachment& operator=(const FeedAttachment&) = delete;

private:
    int source_;
};

} // namespace eawr::core::perf_diag

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "eawr/core/perf_diag/phase.hpp"
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
// viewer already measured; -1 marks "not measured" (no render-thread timing, no GPU time). The fight-intensity counters
// are valid only when FrameFlags::fight is set (the frame side could read them).
struct FrameRecord final {
    std::uint64_t frame{};
    std::uint64_t wall_us{};        // steady_clock microseconds at the start of the frame
    float frame_ms{};               // present-to-present wall time
    float main_ms{-1.0F};           // main thread work outside limiter sleeps
    float render_cpu_ms{-1.0F};
    float gpu_ms{-1.0F};            // the newest GPU time the renderer had (its source frame is not known)
    float ui_lock_ms{};             // waited for the session mutex
    std::uint16_t flags{};
    std::uint16_t reserved{};
    // Fight intensity of the frame (what was on screen); draw calls and objects are the renderer's own counts.
    std::uint32_t units{};          // the local player's units the player sees
    std::uint32_t enemy_units{};    // hostile units the player sees
    std::uint32_t attacking{};      // units (both sides) that have an attack target
    std::uint32_t projectiles{};    // projectiles in flight
    std::uint32_t effects{};        // live battle effects (each owns emitters)
    std::uint32_t particles{};      // live particles (battle effects and unit emitters)
    std::uint32_t draw_calls{};
    std::uint32_t objects{};        // visible instances the renderer drew
};
static_assert(sizeof(FrameRecord) == 72);

struct FrameFlags final {
    static constexpr std::uint16_t stale = 1;
    static constexpr std::uint16_t fight = 2; // the fight-intensity counters of the record are valid
};

// The main thread's named phases of one frame (the existing frame timers), summed per name: ids come from
// FeedWriter::intern_frame_phase_active(). The largest phases are kept when a frame carries more than max_entries.
struct FramePhases final {
    static constexpr std::size_t max_entries = 40;
    struct Entry final {
        PhaseId id{no_phase};
        std::uint16_t reserved{};
        float ms{};
    };
    std::uint64_t frame{};
    std::uint32_t count{};
    std::uint32_t reserved{};
    Entry entries[max_entries]{};
};
static_assert(sizeof(FramePhases) == 16 + 8 * FramePhases::max_entries);

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
    // `workers` (when known) is the session simulation worker count, written into the `source` record.
    [[nodiscard]] static int attach_active(SimFeed& feed, std::string_view label, std::size_t workers = 0);
    // Final drain of that source, then the feed is switched off. The feed must still be alive.
    static void detach_active(int source) noexcept;
    // Asks the writer thread for a capture file of the last capture_window seconds; non-blocking, any thread.
    static void request_capture_active() noexcept;
    // Frame side producer (main thread only, one producer): ignored without a writer.
    static void push_frame_active(const FrameRecord& record) noexcept;
    // The id of a main-thread frame phase name (copied on first sight, main thread only); no_phase without a writer or when
    // the table is full.
    [[nodiscard]] static PhaseId intern_frame_phase_active(std::string_view name) noexcept;
    // One frame's phases (main thread only): ignored without a writer.
    static void push_frame_phases_active(const FramePhases& phases) noexcept;
    // The PC and settings context of the `context` record: a JSON object body without braces
    // (`"gpu":"...","vsync":true`), written next to the CPU facts at the start of every segment and again whenever this is
    // called with a different text. Aggregates only; any thread. Ignored without a writer.
    static void set_context_active(std::string_view fields);

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
    FeedAttachment(SimFeed& feed, const std::string_view label, const std::size_t workers = 0)
        : source_(FeedWriter::attach_active(feed, label, workers)) {}
    ~FeedAttachment() { FeedWriter::detach_active(source_); }
    FeedAttachment(const FeedAttachment&) = delete;
    FeedAttachment& operator=(const FeedAttachment&) = delete;

private:
    int source_;
};

} // namespace eawr::core::perf_diag

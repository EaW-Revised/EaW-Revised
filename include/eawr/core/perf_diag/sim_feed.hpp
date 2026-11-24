#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include "eawr/core/perf_diag/marker.hpp"
#include "eawr/core/perf_diag/phase.hpp"
#include "eawr/core/perf_diag/ring.hpp"

namespace eawr::core::perf_diag {

// What the simulation thread of one live session hands to diagnostics: three single-producer rings (the producer is the
// session's simulation thread, the consumer is one reader thread) and two runtime switches. Write-only: nothing in the
// simulation, a hash, a save, a replay or the network reads these records.
//
// Off (the default): the producer does one relaxed bool test per tick. No clock, no allocation, no lock, no ring write.
// Coarse (set_enabled): per completed tick one TickRecord, its coarse phases, the markers, and one thread-CPU read pair.
// Fine (set_fine, on top of coarse): the per-barrier executor rows and serial scopes become phase records too. Phase
// names are interned once on first use (the only allocation, warm-up only); the per-tick path carries ids.

// Unknown on-CPU ratio.
inline constexpr float cpu_ratio_unknown = -1.0F;

struct TickFlags final {
    static constexpr std::uint16_t clock_reset = 1;  // the pacer dropped its backlog before this tick
    static constexpr std::uint16_t land = 2;         // tactical land session
    static constexpr std::uint16_t scripted = 4;
    static constexpr std::uint16_t battle = 8;       // galactic session: a tactical child battle is active
};

// 40 bytes (a multiple of 8 for the ring).
struct TickRecord final {
    std::uint64_t tick{};
    std::uint64_t wall_us{};       // steady_clock microseconds when the tick was published
    float total_ms{};              // wall cost of the tick (the LiveTickCost total)
    float pacing_lag_ms{};         // how late the due tick started (0 when it was on time)
    float session_lock_ms{};       // the sim thread waited for the session mutex (tactical, fine sampling only)
    float cpu_ratio{cpu_ratio_unknown}; // sim-thread on-CPU share of the tick, see ThreadCpu (< 0 = unknown)
    float interval_ms{};           // the requested tick interval
    std::uint16_t flags{};
    std::uint16_t phase_count{};   // PhaseRecords pushed for this tick (coarse and fine)
};
static_assert(sizeof(TickRecord) == 40);

struct PhaseRecord final {
    std::uint64_t tick{};
    float ms{};
    PhaseId id{no_phase};
    std::uint16_t reserved{};
};
static_assert(sizeof(PhaseRecord) == 16);

// Per-thread on-CPU measure, one pair of calls around a tick on the thread itself (about 0.1 us).
// Windows: QueryThreadCycleTime over the elapsed invariant-TSC cycles of the same interval. That ratio is relative to the
// TSC rate (core clock / TSC clock when the core boosts), so it is a share only after calibration: the consumer treats
// it as a relative series and falls back to "unknown" where it cannot trust it. Linux: CLOCK_THREAD_CPUTIME_ID over
// CLOCK_MONOTONIC, a true share. Elsewhere: unknown.
struct ThreadCpuStamp final {
    std::uint64_t cpu{};  // thread cycles (Windows) or thread CPU nanoseconds
    std::uint64_t wall{}; // TSC cycles (Windows) or monotonic nanoseconds
    bool valid{};
};
[[nodiscard]] ThreadCpuStamp thread_cpu_now() noexcept;
// cpu_ratio_unknown for an invalid pair, a zero or backwards interval, or a ratio no CPU can produce.
[[nodiscard]] float thread_cpu_ratio(const ThreadCpuStamp& begin, const ThreadCpuStamp& end) noexcept;

class SimFeed final {
public:
    static constexpr std::size_t tick_capacity = 8192;
    static constexpr std::size_t phase_capacity = 65536;
    static constexpr std::size_t marker_capacity = 4096;

    SimFeed() = default;
    SimFeed(const SimFeed&) = delete;
    SimFeed& operator=(const SimFeed&) = delete;

    // Any thread, one caller at a time. The rings (about 2 MiB) are allocated by the first set_enabled(true), so a
    // session that never records holds none. The enabled flag is published after the rings: a producer that sees it set
    // sees them. Rings stay allocated after a later set_enabled(false).
    void set_enabled(const bool on) {
        if (on && rings_.load(std::memory_order_acquire) == nullptr) {
            owned_rings_ = std::make_unique<Rings>();
            rings_.store(owned_rings_.get(), std::memory_order_release);
        }
        enabled_.store(on, std::memory_order_release);
    }
    void set_fine(const bool on) noexcept { fine_.store(on, std::memory_order_relaxed); }
    // The only test the producer pays when diagnostics are off.
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    // Fine sampling only counts while the feed is enabled.
    [[nodiscard]] bool fine() const noexcept { return enabled() && fine_.load(std::memory_order_relaxed); }

    // --- producer side: the session's simulation thread only, and only while enabled() ---
    // Returns the id of a name, copying it into feed-owned storage on first sight (a warm-up allocation). no_phase when
    // the table is full.
    [[nodiscard]] PhaseId intern(const std::string_view name) {
        const PhaseId found = table_.find(name);
        if (found != no_phase) return found;
        if (table_.size() == table_.capacity()) {
            names_dropped_.fetch_add(1, std::memory_order_relaxed);
            return no_phase;
        }
        owned_names_.emplace_back(name);
        return table_.intern(owned_names_.back());
    }
    // A name that has static storage duration (a literal): no copy.
    [[nodiscard]] PhaseId intern_static(const std::string_view name) noexcept {
        const PhaseId id = table_.intern(name);
        if (id == no_phase) names_dropped_.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    void push_phase(const std::uint64_t tick, const PhaseId id, const double ms) noexcept {
        Rings* const rings = rings_.load(std::memory_order_relaxed);
        if (id == no_phase || rings == nullptr) return;
        rings->phases.push(PhaseRecord{tick, static_cast<float>(ms), id, 0});
    }
    void push_tick(const TickRecord& record) noexcept {
        if (Rings* const rings = rings_.load(std::memory_order_relaxed)) rings->ticks.push(record);
    }
    void mark(const MarkerKind kind, const std::uint64_t tick, const std::int32_t value = 0) noexcept {
        if (!enabled()) return;
        if (Rings* const rings = rings_.load(std::memory_order_relaxed))
            rings->markers.push(Marker{tick, value, kind, 0, 0});
    }
    void note_clock_reset() noexcept { clock_resets_.fetch_add(1, std::memory_order_relaxed); }

    // --- consumer side: one reader thread ---
    [[nodiscard]] bool pop_tick(TickRecord& out) noexcept {
        Rings* const rings = rings_.load(std::memory_order_acquire);
        return rings != nullptr && rings->ticks.pop(out);
    }
    [[nodiscard]] bool pop_phase(PhaseRecord& out) noexcept {
        Rings* const rings = rings_.load(std::memory_order_acquire);
        return rings != nullptr && rings->phases.pop(out);
    }
    [[nodiscard]] bool pop_marker(Marker& out) noexcept {
        Rings* const rings = rings_.load(std::memory_order_acquire);
        return rings != nullptr && rings->markers.pop(out);
    }
    [[nodiscard]] std::string_view phase_name(const PhaseId id) const noexcept { return table_.name(id); }
    [[nodiscard]] std::size_t phase_count() const noexcept { return table_.size(); }
    [[nodiscard]] std::uint64_t ticks_pushed() const noexcept { return count_of(&Rings::ticks); }
    [[nodiscard]] std::uint64_t phases_pushed() const noexcept { return count_of(&Rings::phases); }
    [[nodiscard]] std::uint64_t markers_pushed() const noexcept { return count_of(&Rings::markers); }
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        const Rings* const rings = rings_.load(std::memory_order_acquire);
        return names_dropped_.load(std::memory_order_relaxed) +
               (rings == nullptr ? 0 : rings->ticks.dropped() + rings->phases.dropped() + rings->markers.dropped());
    }
    [[nodiscard]] std::uint64_t names_dropped() const noexcept { return names_dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t clock_resets() const noexcept { return clock_resets_.load(std::memory_order_relaxed); }
    // Whether the rings exist (false until the first set_enabled(true)).
    [[nodiscard]] bool allocated() const noexcept { return rings_.load(std::memory_order_acquire) != nullptr; }

private:
    struct Rings final {
        SpscRing<TickRecord> ticks{tick_capacity};
        SpscRing<PhaseRecord> phases{phase_capacity};
        SpscRing<Marker> markers{marker_capacity};
    };
    template <class Ring>
    [[nodiscard]] std::uint64_t count_of(Ring Rings::*ring) const noexcept {
        const Rings* const rings = rings_.load(std::memory_order_acquire);
        return rings == nullptr ? 0 : (rings->*ring).pushed();
    }

    PhaseTable<256> table_;
    std::deque<std::string> owned_names_; // stable addresses: the table keeps views into it
    std::unique_ptr<Rings> owned_rings_;
    std::atomic<Rings*> rings_{nullptr};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> fine_{false};
    std::atomic<std::uint64_t> clock_resets_{0};
    std::atomic<std::uint64_t> names_dropped_{0};
};

} // namespace eawr::core::perf_diag

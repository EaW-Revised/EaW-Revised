#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "eawr/core/perf_diag/phase.hpp"

namespace eawr::core::perf_diag {

// Median over the last N values: a FIFO ring plus a sorted copy, updated by one binary search and one memmove per
// sample. Fixed size, no allocation, ~N/2 float moves per push. Write-only diagnostics.
template <std::size_t N = 256>
class RollingMedian final {
public:
    void push(const float value) noexcept {
        if (count_ == N) {
            const float oldest = fifo_[next_];
            const auto old_at = std::lower_bound(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(count_), oldest);
            std::move(old_at + 1, sorted_.begin() + static_cast<std::ptrdiff_t>(count_), old_at);
            --count_;
        }
        const auto at = std::upper_bound(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(count_), value);
        std::move_backward(at, sorted_.begin() + static_cast<std::ptrdiff_t>(count_),
                           sorted_.begin() + static_cast<std::ptrdiff_t>(count_) + 1);
        *at = value;
        ++count_;
        fifo_[next_] = value;
        next_ = (next_ + 1) % N;
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // 0 while empty. Even counts take the upper middle value.
    [[nodiscard]] float median() const noexcept { return count_ == 0 ? 0.0F : sorted_[count_ / 2]; }

private:
    std::array<float, N> fifo_{};
    std::array<float, N + 1> sorted_{};
    std::size_t count_{0};
    std::size_t next_{0};
};

enum class HitchKind : std::uint8_t { tick, frame };
enum class HitchMode : std::uint8_t { tactical_space, tactical_land, galactic, viewer };

// Starting thresholds from the design (3.1): tuned later with recorded data, kept in one place.
struct HitchThresholds final {
    float tick_ratio{2.0F};
    float tick_add_ms{5.0F};
    float tick_floor_ms{4.0F};
    float frame_ratio{2.0F};
    float frame_add_ms{8.3F};
    std::size_t warm_up{32}; // samples before a hitch can be called
};

struct TickObservation final {
    float tick_ms{};
    float pacing_lag_ms{};  // how late the due tick started
    float interval_ms{};    // 1000 / requested ticks per second
    bool clock_reset{};
};

// Fixed 256-sample rolling median; observe() is the only call on the sim thread's path and allocates nothing.
class TickHitchDetector final {
public:
    explicit TickHitchDetector(const HitchThresholds thresholds = {}) noexcept : thresholds_(thresholds) {}
    [[nodiscard]] bool observe(const TickObservation& tick) noexcept {
        const float median = window_.median();
        const bool warm = window_.size() >= thresholds_.warm_up;
        const float limit = std::max({thresholds_.tick_ratio * median, median + thresholds_.tick_add_ms, thresholds_.tick_floor_ms});
        const bool hitch = tick.clock_reset
            || (warm && (tick.tick_ms > limit || (tick.interval_ms > 0.0F && tick.pacing_lag_ms > tick.interval_ms)));
        window_.push(tick.tick_ms);
        return hitch;
    }
    [[nodiscard]] float median_ms() const noexcept { return window_.median(); }

private:
    HitchThresholds thresholds_;
    RollingMedian<256> window_;
};

class FrameHitchDetector final {
public:
    explicit FrameHitchDetector(const HitchThresholds thresholds = {}) noexcept : thresholds_(thresholds) {}
    [[nodiscard]] bool observe(const float frame_ms) noexcept {
        const float median = window_.median();
        const bool warm = window_.size() >= thresholds_.warm_up;
        const bool hitch = warm && frame_ms > std::max(thresholds_.frame_ratio * median, median + thresholds_.frame_add_ms);
        window_.push(frame_ms);
        return hitch;
    }
    [[nodiscard]] float median_ms() const noexcept { return window_.median(); }

private:
    HitchThresholds thresholds_;
    RollingMedian<256> window_;
};

struct PhaseSample final {
    PhaseId id{no_phase};
    float ms{};
};

struct PhaseExcess final {
    PhaseId id{no_phase};
    float excess_ms{};
};

// Per-phase median over the last 256 ticks, so a hitch is attributed to the phase with the largest EXCESS over its own
// median (the biggest phase is always the same one and explains nothing). The default capacity is the PhaseTable
// default (256 ids, about 0.5 MB): keep it on the heap or in static storage, not on a stack. An id at or above the
// capacity is never tracked and never dropped silently: skipped() counts every such sample, and a diagnostics consumer
// is expected to surface a non-zero count. A phase has no excess until it has warm_up samples (its median is not
// meaningful before that), so its first samples cannot read as full-size excess.
template <std::size_t MaxPhases = 256>
class PhaseExcessTracker final {
public:
    static constexpr std::size_t warm_up = 16;

    // The phase with the largest excess of this tick (call before commit() so the tick does not move its own median).
    [[nodiscard]] PhaseExcess top_excess(const std::span<const PhaseSample> phases) const noexcept {
        PhaseExcess best{};
        for (const PhaseSample& sample : phases) {
            if (sample.id >= MaxPhases) { ++skipped_; continue; }
            const float excess = excess_of(sample.id, sample.ms);
            if (best.id == no_phase || excess > best.excess_ms) best = {sample.id, excess};
        }
        return best;
    }
    // Excess of one phase over its own median.
    [[nodiscard]] float excess(const PhaseId id, const float ms) const noexcept {
        if (id >= MaxPhases) { ++skipped_; return 0.0F; }
        return excess_of(id, ms);
    }
    void commit(const std::span<const PhaseSample> phases) noexcept {
        for (const PhaseSample& sample : phases) {
            if (sample.id >= MaxPhases) { ++skipped_; continue; }
            medians_[sample.id].push(sample.ms);
        }
    }
    // Samples carrying an id this tracker cannot hold (size it from the phase registry if this is ever non-zero).
    [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

private:
    [[nodiscard]] float excess_of(const PhaseId id, const float ms) const noexcept {
        return medians_[id].size() < warm_up ? 0.0F : ms - medians_[id].median();
    }

    std::array<RollingMedian<256>, MaxPhases> medians_{};
    mutable std::uint64_t skipped_{0};
};

// One captured hitch, plain data; the log keeps the last 256.
struct HitchRecord final {
    HitchKind kind{HitchKind::tick};
    HitchMode mode{HitchMode::tactical_space};
    PhaseId top_phase{no_phase};
    std::uint32_t unit_count{};
    std::uint64_t number{};      // tick or frame number
    std::uint64_t wall_us{};     // steady-clock microseconds
    std::uint64_t marker_mask{}; // marker kinds within +-2 ticks (see marker_bit)
    std::uint32_t galactic_day{};
    float speed{1.0F};
    float total_ms{};
    float median_ms{};
    float top_excess_ms{};
};

// Fixed ring of the last Capacity hitches; overwrite-oldest, single writer, read on the same thread (main).
template <std::size_t Capacity = 256>
class HitchLog final {
public:
    void add(const HitchRecord& record) noexcept {
        records_[next_] = record;
        next_ = (next_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
        ++total_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    // 0 is the newest hitch.
    [[nodiscard]] const HitchRecord& recent(const std::size_t back) const noexcept {
        return records_[(next_ + Capacity - 1 - back) % Capacity];
    }

private:
    std::array<HitchRecord, Capacity> records_{};
    std::size_t next_{0};
    std::size_t size_{0};
    std::uint64_t total_{0};
};

// Compact long history for pattern detection: 16 bytes per tick, 64Ki ticks (1 MiB, 36 minutes at 1x). The record is
// defined here; its producer and the long-history consumer arrive with the P2 wiring. P1's analyzer works on the last
// 256 hitch rows (PatternSample), which is what the once-per-second readout needs.
struct CompactTick final {
    std::uint64_t tick{};
    float total_ms{};
    PhaseId top_phase{no_phase};
    std::uint16_t flags{};
};
static_assert(sizeof(CompactTick) == 16);

} // namespace eawr::core::perf_diag

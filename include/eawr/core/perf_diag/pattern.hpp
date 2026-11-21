#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "eawr/core/perf_diag/hitch.hpp"
#include "eawr/core/perf_diag/marker.hpp"

namespace eawr::core::perf_diag {

// Hitch input of the pattern analyzer: one row per tick hitch, oldest first.
struct PatternSample final {
    std::uint64_t tick{};
    std::uint64_t wall_us{};
    PhaseId top_phase{no_phase};
    float top_excess_ms{};
    std::uint64_t marker_mask{}; // marker_bit() of the kinds within +-2 ticks
};

// Work that runs at fixed intervals (design 3.4). A hint list for the fold test only: it changes no behaviour, and the
// marker match, when there is one, is stronger evidence (the readout line names it last, after the period and phase).
// Periods of 3 and 4 ticks are listed for completeness but are never fold-tested: a +-1 window covers the whole period
// (see min_fold_period), and one hitch per 3-4 ticks is not a hitch pattern.
struct CadenceEntry final {
    std::uint32_t period_ticks;
    std::string_view what;
};
inline constexpr std::array<CadenceEntry, 9> cadence_table{{
    {3, "gc ai planning/execution (aligned)"}, {4, "capture-point progress"}, {16, "fog-grid decay"},
    {30, "one-second services (damage rate, lua ServiceRate, scans)"}, {90, "shield/energy recharge, gc hints"},
    {150, "attack positioning, mp hash exchange"}, {300, "gc ai perception/learning (aligned)"},
    {900, "gc gambling evaluation"}, {1350, "fiscal cycle (galactic day)"}}};

enum class PatternKind : std::uint8_t { none, tick_locked, wall_locked };

struct PatternThresholds final {
    std::size_t min_occurrences{4};   // of the dominant gap
    float dominant_share{0.60F};      // of all gaps
    float fold_share{0.70F};          // of hitches within +-1 residue
    std::uint32_t gap_slack_ticks{2}; // gap bin: +-2 ticks or +-3 %, whichever is larger
    float gap_slack_ratio{0.03F};
    float marker_share{0.50F};        // hitches carrying the marker before it is named
};

struct PatternReadout final {
    PatternKind kind{PatternKind::none};
    std::size_t hitches{};
    std::uint64_t period{};  // ticks for tick_locked, milliseconds for wall_locked
    std::uint64_t phase{};   // residue of the cluster centre
    std::size_t locked{};    // hitches inside the fold window
    bool speed_ambiguous{};  // tick and wall spacing are both regular: constant speed, change it to tell them apart
    std::uint32_t cadence_hint{}; // cadence_table index + 1 when period matches one, else 0
    PhaseId phase_id{no_phase};   // phase carrying the excess in most hitches
    std::size_t phase_count{};
    float phase_excess_ms{};      // its mean excess
    bool has_marker{};
    MarkerKind marker{MarkerKind::save};
    std::size_t marker_count{};
    bool allocation_driven{};     // irregular spacing with lua markers on most hitches
};

class PatternAnalyzer final {
public:
    static constexpr std::size_t max_hitches = 256;

    explicit PatternAnalyzer(const PatternThresholds thresholds = {}) noexcept : thresholds_(thresholds) {}

    // Runs on the main thread about once per second; fixed scratch, no allocation. Uses the most recent 256 hitches.
    [[nodiscard]] PatternReadout analyze(std::span<const PatternSample> samples) const noexcept {
        if (samples.size() > max_hitches) samples = samples.last(max_hitches);
        PatternReadout out;
        out.hitches = samples.size();
        attribute(samples, out);
        if (samples.size() < thresholds_.min_occurrences + 1) return out;

        std::array<std::uint64_t, max_hitches> tick_gaps{};
        std::array<std::uint64_t, max_hitches> wall_gaps{};
        const std::size_t gaps = samples.size() - 1;
        for (std::size_t i = 0; i < gaps; ++i) {
            tick_gaps[i] = samples[i + 1].tick - samples[i].tick;
            wall_gaps[i] = (samples[i + 1].wall_us - samples[i].wall_us) / 1000;
        }
        const Dominant tick_dominant = dominant_gap({tick_gaps.data(), gaps}, thresholds_.gap_slack_ticks);
        const Dominant wall_dominant = dominant_gap({wall_gaps.data(), gaps}, 1);

        std::array<std::uint64_t, max_hitches> ticks{};
        std::array<std::uint64_t, max_hitches> walls{};
        for (std::size_t i = 0; i < samples.size(); ++i) {
            ticks[i] = samples[i].tick;
            walls[i] = samples[i].wall_us / 1000;
        }

        // The bin's middle gap is only as good as one gap; the whole span divided by its cycle count is as good as the
        // series (a +-1 tick or +-30 ms wobble per hitch no longer drifts across 20 cycles).
        const auto refine = [](const Dominant& dominant, const std::uint64_t span) {
            if (!dominant.valid || dominant.gap == 0 || span < dominant.gap) return dominant.gap;
            const std::uint64_t cycles = (span + dominant.gap / 2) / dominant.gap;
            return cycles == 0 ? dominant.gap : (span + cycles / 2) / cycles;
        };
        const std::uint64_t tick_period = refine(tick_dominant, samples.back().tick - samples.front().tick);
        const std::uint64_t wall_period = refine(wall_dominant, (samples.back().wall_us - samples.front().wall_us) / 1000);

        Fold tick_fold;
        std::array<std::uint64_t, 3 + cadence_table.size()> candidates{};
        std::size_t candidate_count = 0;
        const auto add = [&](const std::uint64_t period) {
            if (period >= min_fold_period(1) && std::find(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(candidate_count), period)
                    == candidates.begin() + static_cast<std::ptrdiff_t>(candidate_count))
                candidates[candidate_count++] = period;
        };
        if (tick_dominant.valid) {
            add(tick_period);
            add(tick_period / 2);
            add(tick_period * 2);
        }
        for (const CadenceEntry& entry : cadence_table) add(entry.period_ticks);
        for (std::size_t i = 0; i < candidate_count; ++i) {
            const Fold fold = fold_series({ticks.data(), samples.size()}, candidates[i], 1);
            if (fold.locked > tick_fold.locked) tick_fold = fold;
        }
        const bool tick_ok = tick_fold.locked >= share_of(samples.size(), thresholds_.fold_share) && tick_fold.period != 0
            && tick_fold.locked >= thresholds_.min_occurrences;

        Fold wall_fold;
        bool wall_ok = false;
        if (wall_dominant.valid) {
            const std::uint64_t slack = std::max<std::uint64_t>(1,
                static_cast<std::uint64_t>(static_cast<float>(wall_period) * thresholds_.gap_slack_ratio));
            if (wall_period >= min_fold_period(slack))
                wall_fold = fold_series({walls.data(), samples.size()}, wall_period, slack);
            wall_ok = wall_fold.period != 0 && wall_fold.locked >= share_of(samples.size(), thresholds_.fold_share);
        }

        if (tick_ok) {
            out.kind = PatternKind::tick_locked;
            out.period = tick_fold.period;
            out.phase = tick_fold.centre;
            out.locked = tick_fold.locked;
            out.speed_ambiguous = wall_ok;
            for (std::size_t i = 0; i < cadence_table.size(); ++i)
                if (cadence_table[i].period_ticks == out.period) out.cadence_hint = static_cast<std::uint32_t>(i + 1);
        } else if (wall_ok) {
            out.kind = PatternKind::wall_locked;
            out.period = wall_fold.period;
            out.phase = wall_fold.centre;
            out.locked = wall_fold.locked;
        }
        out.allocation_driven = out.kind == PatternKind::none && out.has_marker
            && (out.marker == MarkerKind::lua_memory_walk || out.marker == MarkerKind::lua_collect);
        return out;
    }

private:
    struct Dominant final {
        bool valid{};
        std::uint64_t gap{};
        std::size_t count{};
    };
    struct Fold final {
        std::uint64_t period{};
        std::uint64_t centre{};
        std::size_t locked{};
    };

    // A fold window of +-slack must stay a small part of the period, or every series folds perfectly.
    [[nodiscard]] static constexpr std::uint64_t min_fold_period(const std::uint64_t slack) noexcept { return 3 * (2 * slack + 1); }

    [[nodiscard]] static std::size_t share_of(const std::size_t total, const float share) noexcept {
        // smallest count whose share of total reaches the threshold
        std::size_t need = static_cast<std::size_t>(static_cast<float>(total) * share);
        if (static_cast<float>(need) < static_cast<float>(total) * share) ++need;
        return need;
    }

    // The gap value with the most gaps inside its bin; ties go to the smaller gap. Valid when it has enough
    // occurrences and a large enough share of all gaps.
    [[nodiscard]] Dominant dominant_gap(const std::span<const std::uint64_t> gaps, const std::uint32_t slack_floor) const noexcept {
        // Sorted copy, then two binary searches per candidate: O(n log n) instead of comparing every pair.
        std::array<std::uint64_t, max_hitches> sorted{};
        const std::size_t n = std::min(gaps.size(), sorted.size());
        std::copy_n(gaps.begin(), static_cast<std::ptrdiff_t>(n), sorted.begin());
        const auto first = sorted.begin();
        const auto last = sorted.begin() + static_cast<std::ptrdiff_t>(n);
        std::sort(first, last);
        Dominant best;
        auto best_mid = first;
        for (auto at = first; at != last; ++at) {
            const std::uint64_t candidate = *at;
            if (candidate == 0 || (at != first && candidate == *(at - 1))) continue;
            const std::uint64_t slack = std::max<std::uint64_t>(
                slack_floor, static_cast<std::uint64_t>(static_cast<float>(candidate) * thresholds_.gap_slack_ratio));
            const auto from = std::lower_bound(first, last, candidate > slack ? candidate - slack : 0);
            const auto to = std::upper_bound(first, last, candidate + slack);
            const std::size_t count = static_cast<std::size_t>(to - from);
            if (count > best.count) {  // ascending: ties keep the smaller candidate
                best = {true, candidate, count};
                best_mid = from + (to - from) / 2;
            }
        }
        if (best.valid) best.gap = *best_mid; // the middle gap of the bin, not its smallest member
        best.valid = best.valid && best.count >= thresholds_.min_occurrences
            && best.count >= share_of(n, thresholds_.dominant_share);
        return best;
    }

    // Epoch folding: hitches per residue of (value mod period); the best centre counts every hitch within +-slack
    // (circular). One modulo per hitch, a sort, then two binary searches per centre: O(n log n) per candidate period, so
    // one analyze() call stays in the tens of microseconds at 256 hitches. period >= min_fold_period(slack) keeps the
    // wrap-around ranges disjoint.
    [[nodiscard]] static Fold fold_series(const std::span<const std::uint64_t> values, const std::uint64_t period,
                                          const std::uint64_t slack) noexcept {
        Fold best;
        best.period = period;
        std::array<std::uint64_t, max_hitches> residues{};
        const std::size_t n = std::min(values.size(), residues.size());
        for (std::size_t i = 0; i < n; ++i) residues[i] = values[i] % period;
        const auto first = residues.begin();
        const auto last = residues.begin() + static_cast<std::ptrdiff_t>(n);
        std::sort(first, last);
        const auto count_in = [&](const std::uint64_t low, const std::uint64_t high) {
            return static_cast<std::size_t>(std::upper_bound(first, last, high) - std::lower_bound(first, last, low));
        };
        for (auto at = first; at != last; ++at) {
            const std::uint64_t centre = *at;
            std::size_t count = count_in(centre > slack ? centre - slack : 0, centre + slack);
            if (centre < slack) count += count_in(period - (slack - centre), period - 1);
            if (centre + slack >= period) count += count_in(0, centre + slack - period);
            if (count > best.locked) { best.locked = count; best.centre = centre; }
        }
        return best;
    }

    void attribute(const std::span<const PatternSample> samples, PatternReadout& out) const noexcept {
        std::array<std::size_t, 64> marker_counts{};
        // dominant phase: mode of top_phase, ties to the larger mean excess
        std::array<PhaseId, max_hitches> seen{};
        std::array<std::size_t, max_hitches> counts{};
        std::array<float, max_hitches> excess{};
        std::size_t distinct = 0;
        for (const PatternSample& sample : samples) {
            for (unsigned bit = 0; bit < 64; ++bit)
                if ((sample.marker_mask >> bit) & 1U) ++marker_counts[bit];
            if (sample.top_phase == no_phase) continue;
            std::size_t slot = 0;
            while (slot < distinct && seen[slot] != sample.top_phase) ++slot;
            if (slot == distinct) { seen[slot] = sample.top_phase; ++distinct; }
            ++counts[slot];
            excess[slot] += sample.top_excess_ms;
        }
        for (std::size_t i = 0; i < distinct; ++i) {
            const float mean = excess[i] / static_cast<float>(counts[i]);
            if (counts[i] > out.phase_count || (counts[i] == out.phase_count && mean > out.phase_excess_ms)) {
                out.phase_id = seen[i];
                out.phase_count = counts[i];
                out.phase_excess_ms = mean;
            }
        }
        std::size_t best = 0;
        for (unsigned bit = 0; bit < static_cast<unsigned>(MarkerKind::count_); ++bit)
            if (marker_counts[bit] > marker_counts[best]) best = bit;
        if (!samples.empty() && marker_counts[best] >= share_of(samples.size(), thresholds_.marker_share) && marker_counts[best] > 0) {
            out.has_marker = true;
            out.marker = static_cast<MarkerKind>(best);
            out.marker_count = marker_counts[best];
        }
    }

    PatternThresholds thresholds_;
};

// "pattern: every 300 ticks (18/20, tick-locked, phase 0), command-source +14 ms, marker gc.ai.perception 18/20".
// name_of maps a PhaseId to its name (any callable returning string_view). Runs about once per second on the main thread.
template <class NameOf>
[[nodiscard]] std::string format_pattern(const PatternReadout& pattern, NameOf&& name_of) {
    char text[256];
    std::string line;
    if (pattern.kind == PatternKind::none) {
        std::snprintf(text, sizeof text, "pattern: none (%zu hitches, irregular)", pattern.hitches);
        line = text;
        if (pattern.allocation_driven) line += ", allocation-driven (lua memory)";
    } else {
        const bool ticks = pattern.kind == PatternKind::tick_locked;
        std::snprintf(text, sizeof text, "pattern: every %llu %s (%zu/%zu, %s, phase %llu)",
                      static_cast<unsigned long long>(pattern.period), ticks ? "ticks" : "ms", pattern.locked, pattern.hitches,
                      ticks ? "tick-locked" : "wall-locked", static_cast<unsigned long long>(pattern.phase));
        line = text;
        if (pattern.speed_ambiguous) line += ", constant speed: change speed to tell tick- from wall-locked";
        if (pattern.cadence_hint != 0) {
            line += ", cadence ";
            line += cadence_table[pattern.cadence_hint - 1].what;
        }
    }
    if (pattern.phase_id != no_phase) {
        std::snprintf(text, sizeof text, ", %.*s +%.1f ms", static_cast<int>(name_of(pattern.phase_id).size()),
                      name_of(pattern.phase_id).data(), static_cast<double>(pattern.phase_excess_ms));
        line += text;
    }
    if (pattern.has_marker) {
        const std::string_view name = marker_name(pattern.marker);
        std::snprintf(text, sizeof text, ", marker %.*s %zu/%zu", static_cast<int>(name.size()), name.data(),
                      pattern.marker_count, pattern.hitches);
        line += text;
    }
    return line;
}

} // namespace eawr::core::perf_diag

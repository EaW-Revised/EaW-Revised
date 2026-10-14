#pragma once

#include "eawr/assets/assets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// #3027 GM-04 (docs/performance/gpu-memory.md): the adaptive texture budget. Presentation only;
// it never reaches the simulation. Every pooled 2D texture counts its resident bytes here. When
// the device's memory is known and the pool plus everything else on the device (render
// targets, meshes, buffers: the device's own counter less the pool) passes the usable share of
// it, the process-wide mip drop rises one level and resident textures are rebuilt without
// their top mip, so every texture keeps the same detail whatever order it loaded in. A card
// with room for everything never drops a mip.
class TextureBudget final {
public:
    // The most top mips ever dropped, and the smallest top edge a drop leaves: a texture
    // already this small costs too little to be worth blurring.
    static constexpr std::uint32_t max_drop = 4;
    static constexpr std::uint32_t min_edge = 64;
    // The share of the device's budget the process may fill before textures give way; the rest
    // is slack for what the device counter does not see yet (allocations in flight, drivers).
    static constexpr std::uint64_t usable_numerator = 9, usable_denominator = 10;
    // The engine releases a freed texture's memory a few frames later; until then the device
    // counter still holds it, and it is not counted as memory outside the pool.
    static constexpr std::uint64_t release_frames = 3;
    // The device counter asks the allocator for its heap budgets, so the process reads it once
    // per this many frames (every frame only until it first reads nonzero: 0 until frames draw).
    static constexpr std::uint64_t measure_frames = 30;
    // In-frame rebuilds per frame and renderer: a raised drop reaches every resident texture over
    // several frames rather than in one (the engine's own mip-level change likewise resizes a
    // bounded number per frame). Rebuilds at upload time (loading) are not bounded.
    static constexpr std::size_t rebuilds_per_frame = 4;

    struct State final {
        std::uint64_t device_bytes{};   // 0: unknown, never constrained
        std::string source{"unprobed"}; // where device_bytes came from
        std::uint64_t texture_budget{}; // what the pool may hold now: the usable share less other_bytes
        std::uint64_t other_bytes{};    // device memory outside the pool at the last measure
        std::uint64_t resident_bytes{};
        std::uint32_t drop{};           // the current process-wide drop
        std::uint32_t minimum_drop{};   // the detail setting's floor
        std::uint64_t downsized{};      // textures rebuilt smaller after their upload
        std::uint64_t over_budget{};    // checks (uploads, frames) that found it over budget at the deepest drop
        std::uint64_t rebuild_failures{}; // rebuilds the engine refused; the texture keeps its older drop
        bool constrained{};             // a device budget applies (device_bytes known, not off)
    };

    // The device's memory, once known. An override (a forced budget for tests and small-card
    // checks) wins over whatever the device reports.
    void set_device(const std::uint64_t bytes, std::string source) {
        std::lock_guard lock(mutex_);
        if (override_) return;
        apply_device(bytes, std::move(source));
    }
    void set_override(const std::uint64_t bytes) {
        std::lock_guard lock(mutex_);
        override_ = bytes != 0;
        apply_device(bytes, override_ ? "override" : "unprobed");
    }
    // No budget at all: no device probe, no counter reads (measurement switch).
    void disable() {
        std::lock_guard lock(mutex_);
        override_ = true;
        apply_device(0, "off");
    }
    [[nodiscard]] bool probed() const {
        std::lock_guard lock(mutex_);
        return state_.source != "unprobed";
    }
    // The detail setting: at least this many top mips are dropped, budget or not.
    void set_minimum_drop(const std::uint32_t drop) {
        std::lock_guard lock(mutex_);
        state_.minimum_drop = std::min(drop, max_drop);
        state_.drop = std::max(state_.drop, state_.minimum_drop);
    }
    [[nodiscard]] std::uint32_t drop() const {
        std::lock_guard lock(mutex_);
        return state_.drop;
    }

    void add(const std::uint64_t bytes) {
        std::lock_guard lock(mutex_);
        state_.resident_bytes += bytes;
    }
    // Releases carry the frame they happen in (the engine's frames drawn), so freed memory is
    // left out of `other_bytes` for release_frames frames from its own release, wherever in the
    // frame it happened. An empty pool starts over from the detail setting: the next battle may
    // fit again.
    void remove(const std::uint64_t bytes, const std::uint64_t frame) {
        std::lock_guard lock(mutex_);
        state_.resident_bytes -= std::min(bytes, state_.resident_bytes);
        released(bytes, frame);
        if (state_.resident_bytes == 0) state_.drop = state_.minimum_drop;
    }
    void downsized(const std::uint64_t from, const std::uint64_t to, const std::uint64_t frame) {
        std::lock_guard lock(mutex_);
        const std::uint64_t freed = from > to ? from - to : 0;
        state_.resident_bytes -= std::min(freed, state_.resident_bytes);
        released(freed, frame);
        ++state_.downsized;
    }
    // Whether the device counter is due for a read on `frame` (process-wide cadence).
    [[nodiscard]] bool due(const std::uint64_t frame) const {
        std::lock_guard lock(mutex_);
        return state_.device_bytes != 0
            && (!counter_live_ || !measured_ || frame >= measured_frame_ + measure_frames);
    }
    // The device's memory in use now (its own counter, pool included); 0 when unknown keeps the
    // last measure. What is not the pool's is the rest of the process's share.
    void measure(const std::uint64_t device_in_use, const std::uint64_t frame) {
        std::lock_guard lock(mutex_);
        measured_ = true;
        measured_frame_ = frame;
        counter_live_ = device_in_use != 0;
        if (frame >= pending_frame_ + release_frames) pending_bytes_ = 0;
        if (device_in_use != 0) {
            const std::uint64_t held = state_.resident_bytes + pending_bytes_;
            state_.other_bytes = device_in_use > held ? device_in_use - held : 0;
        }
        update_budget();
    }
    [[nodiscard]] bool constrained() const {
        std::lock_guard lock(mutex_);
        return state_.device_bytes != 0;
    }
    [[nodiscard]] bool over() const {
        std::lock_guard lock(mutex_);
        return state_.device_bytes != 0 && state_.resident_bytes > state_.texture_budget;
    }
    void rebuild_failed() {
        std::lock_guard lock(mutex_);
        ++state_.rebuild_failures;
    }
    // One level deeper; false when already at the deepest drop (counted once per call).
    [[nodiscard]] bool raise() {
        std::lock_guard lock(mutex_);
        if (state_.drop >= max_drop) {
            ++state_.over_budget;
            return false;
        }
        ++state_.drop;
        return true;
    }

    [[nodiscard]] State state() const {
        std::lock_guard lock(mutex_);
        State state = state_;
        state.constrained = state_.device_bytes != 0;
        return state;
    }
    // Back to the start-up state (tests).
    void reset() {
        std::lock_guard lock(mutex_);
        state_ = {};
        override_ = false;
        pending_bytes_ = pending_frame_ = measured_frame_ = 0;
        measured_ = counter_live_ = false;
    }

    // Each stored mip level's edges, top first.
    using MipEdges = std::vector<std::array<std::uint32_t, 2>>;
    [[nodiscard]] static MipEdges mip_edges(const assets::Texture& texture) {
        MipEdges edges;
        for (const assets::MipLevel& mip : texture.mips) edges.push_back({mip.width, mip.height});
        return edges;
    }
    // How many top mips a drop of `wanted` actually removes: never the last stored level, never
    // below `min_edge`, and none from a texture without a stored chain.
    [[nodiscard]] static std::uint32_t usable_drop(const MipEdges& edges, const std::uint32_t wanted) {
        std::uint32_t drop = 0;
        while (drop < wanted && drop + 1 < edges.size()) {
            const auto& next = edges[drop + 1];
            if (std::max(next[0], next[1]) < min_edge) break;
            ++drop;
        }
        return drop;
    }

private:
    void apply_device(const std::uint64_t bytes, std::string source) {
        state_.device_bytes = bytes;
        state_.source = std::move(source);
        update_budget();
    }
    void released(const std::uint64_t bytes, const std::uint64_t frame) {
        pending_bytes_ += bytes;
        pending_frame_ = std::max(pending_frame_, frame);
    }
    void update_budget() {
        const std::uint64_t usable = state_.device_bytes / usable_denominator * usable_numerator;
        state_.texture_budget = usable > state_.other_bytes ? usable - state_.other_bytes : 0;
    }

    mutable std::mutex mutex_;
    State state_{};
    bool override_{};
    std::uint64_t pending_bytes_{}, pending_frame_{}, measured_frame_{};
    bool measured_{}, counter_live_{};
};

// One renderer's progress through the process-wide drop: the drop its pool has fully reached and
// whether an in-frame rebuild toward the current one is still under way. Kept apart from the
// renderer so the order is testable without a device. `apply(limit)` rebuilds at most `limit` of
// the renderer's textures at the budget's current drop and returns whether none is left above it.
class TextureDropProgress final {
public:
    static constexpr std::size_t unbounded = ~std::size_t{};

    [[nodiscard]] bool behind(const std::uint32_t drop) const { return pending_ || applied_ != drop; }

    // At upload time (loading): over budget, the drop rises a level at a time until the pool fits
    // or the deepest drop is reached, and then every texture reaches the drop, including those an
    // unfinished in-frame rebuild had not yet reached.
    template <class Apply>
    void at_upload(TextureBudget& budget, Apply&& apply) {
        if (!budget.over()) return;
        while (budget.over() && budget.raise()) apply(unbounded);
        if (behind(budget.drop())) advance(budget, apply, unbounded);
    }
    // Once a frame: a drop this renderer has not reached (raised by any renderer) goes on a few
    // textures per frame; else the device counter is read when due, and over budget the drop
    // rises one level. `in_use()` reads the device counter.
    template <class InUse, class Apply>
    // Returns remaining pressure only when measurement/drop work runs. Fitting frames
    // between measurements need no extra budget query for demand residency.
    bool at_frame(TextureBudget& budget, const std::uint64_t frame, InUse&& in_use, Apply&& apply) {
        if (behind(budget.drop())) {
            advance(budget, apply, TextureBudget::rebuilds_per_frame);
            return budget.over();
        }
        if (!budget.due(frame)) return false;
        budget.measure(in_use(), frame);
        const bool pressure = budget.over();
        if (pressure && budget.raise()) advance(budget, apply, TextureBudget::rebuilds_per_frame);
        return pressure && budget.over();
    }

private:
    template <class Apply>
    void advance(TextureBudget& budget, Apply& apply, const std::size_t limit) {
        const std::uint32_t target = budget.drop();
        pending_ = !apply(limit);
        if (!pending_) applied_ = target;
    }

    std::uint32_t applied_{};
    bool pending_{};
};

[[nodiscard]] inline TextureBudget& texture_budget() {
    static TextureBudget budget;
    return budget;
}

} // namespace eawr::presentation::godot_backend::detail

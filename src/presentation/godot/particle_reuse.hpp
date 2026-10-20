#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <string>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

struct ParticleResidencyPolicy final {
    [[nodiscard]] static constexpr bool immediate(const bool terminal, const bool reuse_enabled,
        const bool reuse_allowed) noexcept { return terminal && !(reuse_enabled && reuse_allowed); }
    [[nodiscard]] static constexpr bool keep_identity(const bool terminal, const bool opaque,
        const bool fog = false) noexcept {
        return !terminal && (opaque || fog);
    }
    [[nodiscard]] static constexpr std::size_t live_rids(const std::size_t active, const std::size_t pooled,
        const std::size_t retained, const std::size_t shared) noexcept { return (active + pooled + retained) * 3 + shared; }
};

// Reuse only identical material state. Geometry and per-effect simulation state
// are deliberately absent: a new owner uploads its own stream before visibility.
struct ParticleReusePolicy final {
    std::string texture, normal, program;
    std::int32_t priority{};
    friend bool operator==(const ParticleReusePolicy&, const ParticleReusePolicy&) = default;
};

// Resource-failure policy only. It never gates CPU emission or RNG. The first
// failed attempt requests a diagnostic; further failures back off to 120 frames.
class ParticleRestoreRetry final {
public:
    [[nodiscard]] bool admits(const std::uint64_t frame) const noexcept { return frame >= next_; }
    [[nodiscard]] bool failed(const std::uint64_t frame) noexcept {
        const bool first = !reported_;
        reported_ = true;
        next_ = frame > std::numeric_limits<std::uint64_t>::max() - delay_
            ? std::numeric_limits<std::uint64_t>::max() : frame + delay_;
        delay_ = std::min<std::uint64_t>(delay_ * 2, 120);
        return first;
    }
    void succeeded() noexcept { next_ = 0; delay_ = 1; reported_ = false; }
private:
    std::uint64_t next_{}, delay_{1};
    bool reported_{};
};

// The acquisition entry points count unconditionally. Scope identifies restore
// work even if a future implementation accidentally reintroduces an acquisition.
class ParticleAssetWork final {
public:
    class RestoreScope final {
    public:
        explicit RestoreScope(ParticleAssetWork& work) : work_(work) { ++work_.restore_depth_; }
        ~RestoreScope() { --work_.restore_depth_; }
        RestoreScope(const RestoreScope&) = delete;
        RestoreScope& operator=(const RestoreScope&) = delete;
    private:
        ParticleAssetWork& work_;
    };
    [[nodiscard]] RestoreScope restoring() { return RestoreScope{*this}; }
    void acquired() noexcept { ++total_; if (restore_depth_ != 0) ++restores_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t restores() const noexcept { return restores_; }
private:
    std::size_t restore_depth_{};
    std::uint64_t total_{}, restores_{};
};

// Saturation makes oversized authored resources fail cache admission; it never
// restricts an emitter's live geometry or changes authored particle capacity.
[[nodiscard]] inline std::uint64_t particle_reuse_charge(const std::uint64_t vertices,
    const std::uint64_t vertex_stride, const std::uint64_t attribute_stride,
    const std::uint64_t indices, const std::uint64_t index_stride,
    const std::uint64_t colour_texture, const std::uint64_t normal_texture) noexcept {
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bytes{};
    const auto add = [&](const std::uint64_t count, const std::uint64_t stride) {
        if (stride != 0 && count > (limit - bytes) / stride) return false;
        bytes += count * stride;
        return true;
    };
    if (!add(vertices, vertex_stride) || !add(vertices, attribute_stride)
        || !add(indices, index_stride) || !add(1, colour_texture) || !add(1, normal_texture)) return limit;
    return bytes;
}

// Retired resources only. Shared textures are conservatively charged in full
// for every retained emitter. Refusal leaves ownership with the caller, which
// uses the ordinary destruction path; there are no deferred-free queues.
template<class Value>
class ParticleReuseCache final {
public:
    static constexpr std::size_t max_emitters = 128;
    static constexpr std::uint64_t max_bytes = 32U * 1024U * 1024U;

    [[nodiscard]] bool retain(ParticleReusePolicy policy, const std::uint64_t bytes, Value& value,
        const std::uint64_t frame) {
        if (entries_.size() >= max_emitters || bytes > max_bytes - bytes_) {
            ++refusals_;
            return false;
        }
        entries_.push_back({std::move(policy), bytes, std::move(value), frame});
        bytes_ += bytes;
        peak_bytes_ = std::max(peak_bytes_, bytes_);
        peak_emitters_ = std::max(peak_emitters_, entries_.size());
        return true;
    }

    [[nodiscard]] std::optional<Value> take(const ParticleReusePolicy& policy) {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
            [&](const auto& entry) { return entry.policy == policy; });
        if (found == entries_.end()) { ++misses_; return std::nullopt; }
        std::optional<Value> result(std::move(found->value));
        bytes_ -= found->bytes;
        entries_.erase(found);
        ++hits_;
        return result;
    }

    template<class Release>
    void expire(const std::uint64_t frame, Release&& release) {
        // Residency policy only: two seconds at 60 Hz, with a bounded retirement
        // burst. Authored emission clocks and live geometry never use this age.
        constexpr std::uint64_t cold_frames = 120;
        constexpr std::size_t retirement_budget = 8;
        for (std::size_t retired = 0; retired < retirement_budget && !entries_.empty(); ++retired) {
            auto& entry = entries_.front();
            if (frame < entry.frame || frame - entry.frame < cold_frames) break;
            release(entry.value);
            bytes_ -= entry.bytes;
            entries_.pop_front();
        }
    }

    template<class Release>
    void drain(Release&& release) {
        for (auto& entry : entries_) release(entry.value);
        entries_.clear();
        bytes_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t peak_emitters() const noexcept { return peak_emitters_; }
    [[nodiscard]] std::uint64_t peak_bytes() const noexcept { return peak_bytes_; }
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    [[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_; }

private:
    struct Entry final { ParticleReusePolicy policy; std::uint64_t bytes; Value value; std::uint64_t frame; };
    std::list<Entry> entries_;
    std::uint64_t bytes_{}, peak_bytes_{}, hits_{}, misses_{}, refusals_{};
    std::size_t peak_emitters_{};
};

} // namespace eawr::presentation::godot_backend::detail

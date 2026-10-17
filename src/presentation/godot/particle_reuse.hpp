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

// Reuse only identical material state. Geometry and per-effect simulation state
// are deliberately absent: a new owner uploads its own stream before visibility.
struct ParticleReusePolicy final {
    std::string texture, normal, program;
    std::int32_t priority{};
    friend bool operator==(const ParticleReusePolicy&, const ParticleReusePolicy&) = default;
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

    [[nodiscard]] bool retain(ParticleReusePolicy policy, const std::uint64_t bytes, Value& value) {
        if (entries_.size() >= max_emitters || bytes > max_bytes - bytes_) {
            ++refusals_;
            return false;
        }
        entries_.push_back({std::move(policy), bytes, std::move(value)});
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
    struct Entry final { ParticleReusePolicy policy; std::uint64_t bytes; Value value; };
    std::list<Entry> entries_;
    std::uint64_t bytes_{}, peak_bytes_{}, hits_{}, misses_{}, refusals_{};
    std::size_t peak_emitters_{};
};

} // namespace eawr::presentation::godot_backend::detail

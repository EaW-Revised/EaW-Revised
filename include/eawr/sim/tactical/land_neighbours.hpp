#pragma once

#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace eawr::sim::tactical {

// Scratch-only uniform buckets. Each member owns one input slot; the ordered build
// stores each start point once. Queries expand by the maximum swept reach, then
// reject disjoint swept boxes and return ascending member ranks. No duplicate pairs.
class LandNeighbours final {
public:
    struct Body {
        math::Vec2 from{}, to{};
        math::Fixed radius{};
        bool present{true}, valid{true};
    };
    void resize(std::size_t count) { bodies.resize(count); entries_.resize(count); }
    std::vector<Body> bodies;

    void build(math::Fixed minimum_pitch) {
        safe_ = true;
        reach_ = 0;
        // Arithmetic guard, not a content bound: outside this conservative range use
        // the original scan, preserving even its overflow diagnostics for distant pairs.
        constexpr auto limit = std::int64_t{32768} * math::Fixed::scale;
        for (const auto& b : bodies) {
            safe_ &= b.valid && b.radius.raw() >= 0 && b.radius.raw() <= limit;
            for (auto v : {b.from.x, b.from.y, b.to.x, b.to.y})
                safe_ &= v.raw() >= -limit && v.raw() <= limit;
        }
        if (!safe_) return;
        for (const auto& b : bodies) {
            const auto dx = b.to.x.raw() - b.from.x.raw(), dy = b.to.y.raw() - b.from.y.raw();
            reach_ = std::max(reach_, std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) + b.radius.raw() + padding);
        }
        pitch_ = std::max<std::int64_t>(std::max<std::int64_t>(1, minimum_pitch.raw()), 2 * reach_);
        for (std::size_t i = 0; i < bodies.size(); ++i)
            entries_[i] = {cell(bodies[i].from.x.raw()), cell(bodies[i].from.y.raw()), i};
        std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a.key() < b.key(); });
    }
    [[nodiscard]] bool safe() const noexcept { return safe_; }
    void query(std::size_t rank, std::vector<std::size_t>& found) const {
        found.clear();
        const auto& a = bodies[rank];
        const auto lx = low(a.from.x, a.to.x, a.radius), hx = high(a.from.x, a.to.x, a.radius);
        const auto ly = low(a.from.y, a.to.y, a.radius), hy = high(a.from.y, a.to.y, a.radius);
        for (auto x = cell(lx - reach_); x <= cell(hx + reach_); ++x)
            for (auto y = cell(ly - reach_); y <= cell(hy + reach_); ++y) {
                auto at = std::lower_bound(entries_.begin(), entries_.end(), std::pair{x, y},
                    [](const auto& e, const auto& key) { return std::pair{e.x, e.y} < key; });
                for (; at != entries_.end() && at->x == x && at->y == y; ++at) {
                    const auto& b = bodies[at->rank];
                    if (!b.present || at->rank == rank
                        || lx > high(b.from.x, b.to.x, b.radius) || hx < low(b.from.x, b.to.x, b.radius)
                        || ly > high(b.from.y, b.to.y, b.radius) || hy < low(b.from.y, b.to.y, b.radius)) continue;
                    found.push_back(at->rank);
                }
            }
        std::sort(found.begin(), found.end());
    }
private:
    // Covers rounded squared length/sqrt close to zero as well as the half-raw
    // interpolation error. sqrt(1 Q24 raw) = 4096 raw; each box gets twice that.
    static constexpr std::int64_t padding = 8192;
    static std::int64_t low(math::Fixed a, math::Fixed b, math::Fixed r) {
        return std::min(a.raw(), b.raw()) - r.raw() - padding;
    }
    static std::int64_t high(math::Fixed a, math::Fixed b, math::Fixed r) {
        return std::max(a.raw(), b.raw()) + r.raw() + padding;
    }
    std::int64_t cell(std::int64_t raw) const { return raw / pitch_ - (raw % pitch_ < 0 ? 1 : 0); }
    struct Entry {
        std::int64_t x{}, y{};
        std::size_t rank{};
        std::tuple<std::int64_t, std::int64_t, std::size_t> key() const { return {x, y, rank}; }
    };
    std::vector<Entry> entries_;
    std::int64_t reach_{}, pitch_{1};
    bool safe_{};
};
} // namespace eawr::sim::tactical

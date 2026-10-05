#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

namespace {

[[nodiscard]] std::int64_t floor_divide(const std::int64_t value, const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient * divisor != value && value < 0 ? quotient - 1 : quotient;
}

constexpr std::int64_t cell_raw = combat_cell_size * Fixed::scale;

} // namespace

CombatCell combat_cell_of(const Vec3& point) noexcept {
    CombatCell cell;
    cell.y = static_cast<std::int32_t>(floor_divide(point.y.raw(), cell_raw));
    auto x = point.x.raw();
    if ((cell.y & 1) != 0) x -= cell_raw / 2;
    cell.x = static_cast<std::int32_t>(floor_divide(x, cell_raw));
    return cell;
}

Vec3 combat_cell_point(const CombatCell cell, const Fixed z) noexcept {
    auto x = static_cast<std::int64_t>(cell.x) * cell_raw + cell_raw / 2;
    if ((cell.y & 1) != 0) x += cell_raw / 2;
    return {Fixed::from_raw(x), Fixed::from_raw(static_cast<std::int64_t>(cell.y) * cell_raw + cell_raw / 2), z};
}

namespace {

constexpr std::int64_t idle_raw = idle_cell_size * Fixed::scale;

} // namespace

CombatCell idle_cell_of(const Vec3& point) noexcept {
    CombatCell cell;
    cell.y = static_cast<std::int32_t>(floor_divide(point.y.raw(), idle_raw));
    auto x = point.x.raw();
    if ((cell.y & 1) != 0) x -= idle_raw / 2;
    cell.x = static_cast<std::int32_t>(floor_divide(x, idle_raw));
    return cell;
}

Vec3 idle_cell_point(const CombatCell cell, const Fixed z) noexcept {
    auto x = static_cast<std::int64_t>(cell.x) * idle_raw + idle_raw / 2;
    if ((cell.y & 1) != 0) x += idle_raw / 2;
    return {Fixed::from_raw(x), Fixed::from_raw(static_cast<std::int64_t>(cell.y) * idle_raw + idle_raw / 2), z};
}

std::optional<CombatCell> idle_cell_claim(const Vec3& desired, const std::span<const CombatCell> taken) {
    const auto held = [&taken](const CombatCell cell) { return std::find(taken.begin(), taken.end(), cell) != taken.end(); };
    const CombatCell start = idle_cell_of(desired);
    if (!held(start)) return start;
    // FM-24: rings around the start cell, row by row from the low corner; the start cell counts
    // as the first cell examined.
    std::uint32_t examined = 1;
    for (std::int32_t ring = 1; examined < idle_cell_search_limit; ++ring) {
        std::optional<CombatCell> best;
        math::detail::UInt192 best_score{};
        for (std::int32_t y = start.y - ring; y <= start.y + ring && examined < idle_cell_search_limit; ++y) {
            for (std::int32_t x = start.x - ring; x <= start.x + ring && examined < idle_cell_search_limit; ++x) {
                const std::int32_t across = x > start.x ? x - start.x : start.x - x;
                const std::int32_t along = y > start.y ? y - start.y : start.y - y;
                if (std::max(across, along) != ring) continue;
                ++examined;
                const CombatCell cell{x, y};
                if (held(cell)) continue;
                const Vec3 point = idle_cell_point(cell, Fixed{});
                const auto dx = math::detail::unsigned_magnitude(point.x.raw() - desired.x.raw());
                const auto dy = math::detail::unsigned_magnitude(point.y.raw() - desired.y.raw());
                auto score = math::detail::multiply_u64(dx, dx);
                static_cast<void>(math::detail::add_magnitude(score, math::detail::multiply_u64(dy, dy)));
                if (!best || math::detail::compare(score, best_score) < 0) {
                    best = cell;
                    best_score = score;
                }
            }
        }
        if (best) return best;
    }
    return std::nullopt;
}

bool within_combat_cell_reach(const Vec3& point, const CombatCell cell) noexcept {
    // FD-03: 400 / sqrt(2), compared squared (80000 square units) in raw units.
    const Vec3 centre = combat_cell_point(cell, Fixed{});
    const auto dx = math::detail::unsigned_magnitude(point.x.raw() - centre.x.raw());
    const auto dy = math::detail::unsigned_magnitude(point.y.raw() - centre.y.raw());
    auto across = math::detail::multiply_u64(dx, dx);
    static_cast<void>(math::detail::add_magnitude(across, math::detail::multiply_u64(dy, dy)));
    const auto half = static_cast<std::uint64_t>(combat_cell_size * combat_cell_size / 2);
    return math::detail::compare(across,
               math::detail::multiply_u64(half * static_cast<std::uint64_t>(Fixed::scale), static_cast<std::uint64_t>(Fixed::scale)))
        <= 0;
}

core::Result<bool> in_follow_cone(const CraftView& self, const Vec3& target, Fixed& yaw, Fixed& pitch) {
    if (self.profile == nullptr) {
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "follow test without a craft profile"));
    }
    Calc calc;
    const bool result = followable(calc, self.position, self.state, target, self.profile->attack_distance, yaw, pitch);
    if (!calc.ok()) {
        return core::Result<bool>::failure(calc.error("follow test of craft " + std::to_string(self.id)));
    }
    return core::Result<bool>::success(result);
}

ChaseCandidateIndex::ChaseCandidateIndex(std::vector<ChaseCandidate> candidates)
    : candidates_(std::move(candidates)) {
    for (std::size_t row = 0; row < candidates_.size(); ++row) {
        const auto& candidate = candidates_[row];
        // FD-06: after expiry, a running timer can only stay running through the
        // ordered commit. That craft cannot be chosen this tick, whatever its cone.
        if (candidate.craft == nullptr || candidate.craft->state.chase_until != 0) continue;
        auto& cell = cells_[{candidate.joined_cell.x, candidate.joined_cell.y}];
        cell.rows.push_back(row);
        const auto& point = candidate.craft->position;
        cell.positions[{floor_divide(point.x.raw(), cell_raw), floor_divide(point.y.raw(), cell_raw)}].push_back(row);
    }
}

std::span<const CraftView* const> ChaseCandidateIndex::query(const CraftView& self, const CombatCell joined_cell,
    const TeamId team, ChaseQueryScratch& scratch) const {
    auto& rows = scratch.rows;
    auto& result = scratch.result;
    rows.clear();
    result.clear();
    const auto found = cells_.find({joined_cell.x, joined_cell.y});
    if (found == cells_.end() || self.profile == nullptr) return result;
    const auto& cell = found->second;
    const auto reach = self.profile->attack_distance.raw();
    const auto low_x = floor_divide(self.position.x.raw() - reach, cell_raw);
    const auto high_x = floor_divide(self.position.x.raw() + reach, cell_raw);
    const auto low_y = floor_divide(self.position.y.raw() - reach, cell_raw);
    const auto high_y = floor_divide(self.position.y.raw() + reach, cell_raw);
    const auto in_box = [&](const std::size_t row) {
        const auto& candidate = candidates_[row];
        const auto& point = candidate.craft->position;
        return candidate.team != team && point.x.raw() >= self.position.x.raw() - reach
            && point.x.raw() <= self.position.x.raw() + reach
            && point.y.raw() >= self.position.y.raw() - reach && point.y.raw() <= self.position.y.raw() + reach;
    };
    // Very large data-driven ranges use the cell's canonical list rather than walking
    // empty spatial buckets. Validated coordinates/ranges keep these products in int64.
    if ((high_x - low_x + 1) * (high_y - low_y + 1) > static_cast<std::int64_t>(cell.positions.size())) {
        for (const auto row : cell.rows) {
            if (in_box(row)) rows.push_back(row);
        }
    } else {
        for (auto y = low_y; y <= high_y; ++y) {
            for (auto x = low_x; x <= high_x; ++x) {
                const auto bucket = cell.positions.find({x, y});
                if (bucket == cell.positions.end()) continue;
                for (const auto row : bucket->second) {
                    if (in_box(row)) rows.push_back(row);
                }
            }
        }
        std::sort(rows.begin(), rows.end());
    }
    result.reserve(rows.size());
    for (const auto row : rows) result.push_back(candidates_[row].craft);
    return result;
}


} // namespace eawr::sim::tactical

// WLAI-21..23 (#3209, #5899): where a land AI player's reinforcement company is offered a landing position. The space search
// (rings of a fixed step around the requested point) cannot reach a land battle's admission rule, which needs an allied
// reinforcement point in range (LC-44), so a land battle ranks the allied points and tries a ring around each.
//
// Rule rows (docs/behaviour/walks/land-ai.md); a constant the walk does not state is marked PROJECT CHOICE:
//  - WLAI-22: candidates are allied points with a positive enabling radius; they are ranked by their distance to the
//    requested location. The walk ranks by the zone distance when both zones exist and by XY distance otherwise; land
//    perception zones are not modelled yet (G2, #3208), so every ranking here is the XY fallback. Equal distances keep
//    every candidate; their order is PROJECT CHOICE: ascending entity id.
//  - WLAI-23: for each candidate in ranked order, a ring of positions at half the enabling radius, starting on the positive X
//    direction, stepping 0.7853982 rad (45 degrees to float precision) while the angle stays below 6.2831855: eight positions.
//    The first position the land landing admission (LC-43/44/45) accepts is the landing position; the company is not
//    placed anywhere else, and the search ends without a position when no candidate has one.
//  - WLAI-21: no room gives an unfinished result, never a landing at the requested location. The block retries on the next
//    tick (PROJECT CHOICE: no back-off; the cheap gates below run first and an exhausted search is eight admissions per
//    candidate).
//  - Without Use_Reinforcement_Points no point is needed (LC-44), so the requested location itself is the only candidate
//    (PROJECT CHOICE; the walk's candidate rule presumes points).
#include "session_impl.hpp"
#include "session_tick.hpp"
#include "tactical_internal.hpp"
#include "../math/wide.hpp"
#include "diagnostic_timer.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;

// WLAI-23: the authored step is 0.7853982 rad and the loop bound 6.2831855 rad; the accumulated float angle reaches the bound
// at the eighth step, so eight positions are tried. Expressed as turns: k / 8.
constexpr std::uint32_t ring_positions = 8;

struct Ranked {
    math::detail::UInt192 distance;
    EntityId id{};
    math::Vec3 position;
    Fixed radius;
};

math::detail::UInt192 planar_distance_squared(const math::Vec3& from, const math::Vec3& to) noexcept {
    const auto dx = math::detail::unsigned_magnitude(from.x.raw() - to.x.raw());
    const auto dy = math::detail::unsigned_magnitude(from.y.raw() - to.y.raw());
    auto total = math::detail::multiply_u64(dx, dx);
    static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(dy, dy)));
    return total;
}
} // namespace

// The position the player's `type` company is offered, or none. `candidates` counts the admissions tried.
core::Result<std::optional<math::Vec3>> TacticalSession::Impl::land_landing_search(const PlayerId player, const TypeId type,
    const math::Vec3& target, const Fixed yaw_degrees, const UnitStage& staged,
    const std::map<EntityId, PopulationShare>& shares, std::uint32_t& candidates) const {
    using Result = core::Result<std::optional<math::Vec3>>;
    const auto& rules = land->content.reinforcement;
    const auto binding = std::find_if(land->content.companies.begin(), land->content.companies.end(),
        [type](const LandCompanyBinding& row) { return row.type == type; });
    if (!rules.enabled || binding == land->content.companies.end()) return Result::success(std::nullopt);
    // LC-46..48: the cap is the cheap refusal; a full cap spends no ring.
    const std::int64_t population = binding->company.population_value.value_or(0);
    if (population > land_company_cap(player, land_capacity(player, staged)) - land_company_count(player, shares, staged))
        return Result::success(std::nullopt);
    const auto admit = [&](const math::Vec3& point) -> core::Result<bool> {
        ++candidates;
        return land_company_admission(*binding, player, point, yaw_degrees, staged);
    };
    if (!rules.use_points) {
        auto admitted = admit(target);
        if (!admitted) return Result::failure(admitted.error());
        return Result::success(admitted.value() ? std::optional<math::Vec3>(target) : std::nullopt);
    }
    // WLAI-22: allied live points with a positive enabling radius.
    std::vector<Ranked> ranked;
    for (const auto& [id, unit] : staged) {
        if (unit.durability && unit.durability->hull.raw() <= 0) continue;
        const auto* point = rules.point(unit.state.type_id);
        if (point == nullptr || point->enabling_radius.raw() <= 0 || !allied(unit.state.owner, player)) continue;
        ranked.push_back({planar_distance_squared(unit.state.position, target), id, unit.state.position, point->enabling_radius});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        const int order = math::detail::compare(left.distance, right.distance);
        return order != 0 ? order < 0 : left.id < right.id;
    });
    for (const auto& candidate : ranked) {
        // WLAI-23: half the enabling radius, first position on the positive X direction.
        Fixed ring;
        if (!math::try_divide(candidate.radius, Fixed::from_integer(2).value(), ring))
            return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "landing ring radius overflow"));
        for (std::uint32_t step = 0; step < ring_positions; ++step) {
            const auto turns = math::divide(Fixed::from_integer(step).value(), Fixed::from_integer(ring_positions).value());
            if (!turns) return Result::failure(turns.error());
            const auto direction = math::sin_cos_turn(turns.value());
            Fixed dx, dy, x, y;
            if (!math::try_multiply(ring, direction.cosine, dx) || !math::try_multiply(ring, direction.sine, dy)
                || !math::try_add(candidate.position.x, dx, x) || !math::try_add(candidate.position.y, dy, y))
                return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "landing ring position overflow"));
            const math::Vec3 point{x, y, Fixed{}};
            auto admitted = admit(point);
            if (!admitted) return Result::failure(admitted.error());
            if (admitted.value()) return Result::success(std::optional<math::Vec3>(point));
        }
    }
    return Result::success(std::nullopt);
}

// Resolves the AI search request queued for `key` when the land economy step reaches the command (the land step does not use
// the shared search lane), against the ground and account the landing is admitted in. True: the company is offered
// `payload.position`, now the chosen one, and the command goes on as an ordinary reinforcement; false: no position, the command
// is dropped (the caller also removes it from the record) and the requester asks again. The result is
// what reinforcement_search_result() reports. Nothing here depends on the worker count: the land step is serial.
core::Result<bool> TacticalSession::Impl::resolve_land_reinforcement_search(const CommandKey& key, ReinforcePayload& payload,
    UnitStage& staged, const std::vector<PlayerEconomy>& staged_ledgers, const std::map<EntityId, PopulationShare>& staged_shares) {
    using Result = core::Result<bool>;
    const auto request = reinforcement_searches.find(key);
    if (request == reinforcement_searches.end()) return Result::success(true);
    const auto player = key.player_id;
    TacticalSession::ReinforcementSearchResult result;
    result.next_attempt = request->second.attempt;
    result.position = payload.position;
    const auto* account = economy.player(player);
    const auto ledger = std::find_if(staged_ledgers.begin(), staged_ledgers.end(), [player](const PlayerEconomy& entry) { return entry.player == player; });
    // The same cheap refusals as the ordered command: an unknown account, a decided battle, a disabled type, an empty pool.
    if (account != nullptr && ledger != staged_ledgers.end() && !outcome && !economy.disabled_types.contains(payload.type)
        && std::find(ledger->pool.begin(), ledger->pool.end(), payload.type) != ledger->pool.end()) {
        const DiagnosticTimer timer;
        auto chosen = land_landing_search(player, payload.type, payload.position, payload.facing_yaw.value_or(account->reinforcement_yaw),
            staged, staged_shares, result.candidates);
        if (!chosen) return Result::failure(chosen.error());
        result.nanoseconds = timer.elapsed_nanoseconds();
        if (chosen.value()) { result.valid = true; result.position = *chosen.value(); }
    }
    reinforcement_search_results.insert_or_assign(std::pair{player, request->second.token}, result);
    reinforcement_searches.erase(request);
    if (result.valid) payload.position = result.position;
    return Result::success(result.valid);
}

} // namespace eawr::sim::tactical

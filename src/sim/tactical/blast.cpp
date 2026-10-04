#include "blast_internal.hpp"
#include "../math/wide.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::tactical::detail {
namespace {
using math::Fixed;

[[nodiscard]] bool same_mesh(const std::string_view left, const std::string_view right) noexcept {
    const auto folded = [](const unsigned char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; };
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [&](const unsigned char a, const unsigned char b) { return folded(a) == folded(b); });
}

[[nodiscard]] std::uint32_t mesh_route(const CombatProfile& profile, const std::uint32_t selected) {
    if (selected >= profile.hardpoint_meshes.size() || profile.hardpoint_meshes[selected].empty()) return hull_target;
    const auto& name = profile.hardpoint_meshes[selected];
    for (std::uint32_t index = 0; index < profile.hardpoint_meshes.size(); ++index) {
        if (same_mesh(name, profile.hardpoint_meshes[index])) return index;
    }
    return hull_target;
}

[[nodiscard]] core::Result<Fixed> distance_delay(const BlastProfile& profile, const Fixed distance, const Fixed radius) {
    if (radius.raw() <= 0 || profile.max_delay.raw() == 0) return core::Result<Fixed>::success(Fixed{});
    auto portion = math::divide(distance, radius);
    if (!portion) return portion;
    return math::multiply(portion.value(), profile.max_delay);
}

[[nodiscard]] core::Result<Fixed> planar_distance(const math::Vec3& a, const math::Vec3& b) {
    auto x = math::subtract(a.x, b.x);
    auto y = math::subtract(a.y, b.y);
    if (!x) return core::Result<Fixed>::failure(x.error());
    if (!y) return core::Result<Fixed>::failure(y.error());
    return math::length(math::Vec3{x.value(), y.value(), Fixed{}});
}

[[nodiscard]] core::Result<bool> overlaps(const CombatUnit& unit, const math::Vec3& centre, const Fixed radius) {
    using Result = core::Result<bool>;
    const auto& profile = *unit.profile;
    if (!profile.collision) return Result::success(false);
    const auto& matrix = unit.transform.rows;
    std::array<Fixed, 3> delta{};
    const std::array<Fixed, 3> point{centre.x, centre.y, centre.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto value = math::subtract(point[axis], matrix[axis][3]);
        if (!value) return Result::failure(value.error());
        delta[axis] = value.value();
    }
    std::array<Fixed, 3> local{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t row = 0; row < 3; ++row) {
            auto term = math::multiply(matrix[row][axis], delta[row]);
            if (!term) return Result::failure(term.error());
            auto sum = math::add(local[axis], term.value());
            if (!sum) return Result::failure(sum.error());
            local[axis] = sum.value();
        }
    }
    // WAD-12: sphere against the transformed model box, rather than its larger cull AABB.
    const auto& box = *profile.collision;
    const math::Vec3 here{local[0], local[1], local[2]};
    const math::Vec3 nearest{std::clamp(here.x, box.min.x, box.max.x),
        std::clamp(here.y, box.min.y, box.max.y), std::clamp(here.z, box.min.z, box.max.z)};
    return Result::success(within_range(here, nearest, radius, RangeMetric::spatial));
}

[[nodiscard]] core::Result<Fixed> refined_distance(const CombatUnit& unit, const math::Vec3& centre,
    const Fixed distance) {
    const auto& profile = *unit.profile;
    std::optional<Fixed> fraction;
    if (profile.meshes.empty() && profile.collision) {
        auto contact = segment_enters_box(*profile.collision, unit.transform, centre, unit.position);
        if (!contact) return core::Result<Fixed>::failure(contact.error());
        fraction = contact.value();
    } else if (!profile.meshes.empty()) {
        // U-01 project policy: reuse the existing recipient collision service and mesh grid.
        // It tests this recipient only; intervening geometry never occludes an explosion.
        const auto enabled = [&](const std::size_t index) {
            const auto source = profile.meshes[index].source_hardpoint;
            return source == no_hardpoint || unit.durability == nullptr
                || source >= unit.durability->hardpoints.size() || unit.durability->hardpoints[source].raw() > 0;
        };
        auto contact = segment_hits_meshes(profile.meshes, enabled, unit.transform, centre, unit.position);
        if (!contact) return core::Result<Fixed>::failure(contact.error());
        if (contact.value()) fraction = contact.value()->fraction;
    }
    if (!fraction) return core::Result<Fixed>::success(distance);
    auto refined = math::multiply(distance, *fraction);
    if (!refined) return refined;
    return core::Result<Fixed>::success(std::min(distance, refined.value()));
}
} // namespace

math::Fixed primary_damage(const Projectile& projectile) noexcept {
    return projectile.damage.raw() == 0 && projectile.blast.enabled() ? projectile.blast.damage : projectile.damage;
}

bool blast_after_hit(const Projectile& projectile, const HitOutcome& outcome) noexcept {
    return projectile.blast.enabled() && !outcome.cancelled; // WAD-03: shield absorption is independent
}

core::Result<math::Fixed> blast_factor(const BlastProfile& profile, const math::Fixed distance,
    const math::Fixed radius) {
    constexpr auto one = math::Fixed::scale;
    if (!profile.dropoff || radius.raw() <= 0 || profile.tiers == 0 || distance.raw() <= 0) {
        return core::Result<Fixed>::success(Fixed::from_raw(one));
    }
    // WAD-16: integer quotient avoids rounding a value just below a tier up to its boundary.
    // Current radius modes can enlarge the validated authored range: retain the exact wide product.
    std::uint64_t high{}, low{}, remainder{};
    math::detail::multiply_64(static_cast<std::uint64_t>(distance.raw()),
        static_cast<std::uint64_t>(profile.tiers), high, low);
    const auto divisor = static_cast<std::uint64_t>(radius.raw());
    if (high >= divisor) return core::Result<Fixed>::success(Fixed{});
    const auto band = math::detail::divide_128(high, low, divisor, remainder);
    if (band >= static_cast<std::uint64_t>(profile.tiers) + 1) return core::Result<Fixed>::success(Fixed{});
    return math::divide(Fixed::from_raw((profile.tiers + 1 - static_cast<std::int64_t>(band)) * one),
        Fixed::from_raw((static_cast<std::int64_t>(profile.tiers) + 1) * one));
}

core::Result<BlastStep> prepare_blast(const CombatWorld& world, const Projectile& projectile,
    const math::Vec3& centre, const std::optional<EntityId> direct, const bool source_available,
    const std::string_view direct_mesh) {
    using Result = core::Result<BlastStep>;
    BlastStep result;
    if (!projectile.blast.enabled()) return Result::success(std::move(result));
    const auto owner = std::lower_bound(world.players.begin(), world.players.end(), projectile.owner,
        [](const Player& player, const PlayerId id) { return player.player_id < id; });
    if (owner == world.players.end() || owner->player_id != projectile.owner) return Result::success(std::move(result));
    auto radius = projectile.blast.radius;
    if (const auto* source = source_available ? world.find(projectile.shooter) : nullptr) {
        auto scaled = math::multiply(radius, source->scatter_radius);
        if (!scaled) return Result::failure(scaled.error());
        radius = scaled.value();
    }
    if (radius.raw() < 0) return Result::success(std::move(result));
    auto low_x = math::subtract(centre.x, radius);
    auto low_y = math::subtract(centre.y, radius);
    auto high_x = math::add(centre.x, radius);
    auto high_y = math::add(centre.y, radius);
    for (const auto* value : {&low_x, &low_y, &high_x, &high_y}) {
        if (!*value) return Result::failure(value->error());
    }
    // WAD-11: Q24 cannot encode +/-1e18; the full representable Z interval is equivalent.
    const CullBox query{{low_x.value(), low_y.value(), Fixed::from_raw(std::numeric_limits<std::int64_t>::min())},
        {high_x.value(), high_y.value(), Fixed::from_raw(std::numeric_limits<std::int64_t>::max())}};
    result.players.resize(world.players.size());
    for (std::size_t player_index = 0; player_index < world.players.size(); ++player_index) {
        const auto& player = world.players[player_index];
        auto& group = result.players[player_index];
        if (!projectile.blast.immune_faction && !world.hostile(projectile.owner, player.player_id)) continue;
        // WAD-13 / U-02 project policy: retain the existing CO tree history and player-table order.
        // No nearest-first or entity-ID resort; retail enumeration equivalence remains unverified.
        const auto candidates = world.collection != nullptr ? world.collection->collect(player.player_id, query)
            : world.index.box(centre, {radius, radius, Fixed::from_raw(std::numeric_limits<std::int64_t>::max())}, player.player_id);
        result.examined += candidates.size();
        std::int64_t victims = 0;
        for (const auto id : candidates) {
            const auto* unit = world.find(id);
            if (unit == nullptr || unit->profile == nullptr || unit->in_limbo) continue;
            auto distance = planar_distance(unit->position, centre);
            if (!distance) return Result::failure(distance.error());
            if (!within_range(centre, unit->position, radius, RangeMetric::planar)) {
                auto admitted = overlaps(*unit, centre, radius);
                if (!admitted) return Result::failure(admitted.error());
                if (!admitted.value()) continue;
            }
            const bool destroyable = unit->durability_profile != nullptr
                && std::any_of(unit->durability_profile->hardpoints.begin(), unit->durability_profile->hardpoints.end(),
                    [](const HardpointProfile& hardpoint) { return hardpoint.destroyable; });
            if ((direct == id && !destroyable) || !unit->profile->living_projectile_collision
                || (projectile.blast.immune_faction && player.faction_id == *projectile.blast.immune_faction)) continue;
            ++victims; // WAD-29: before delivery, reset per player, post-recipient exit from the whole blast
            auto d = distance.value();
            if (projectile.blast.dropoff && radius.raw() > 0) {
                auto refined = refined_distance(*unit, centre, d);
                if (!refined) return Result::failure(refined.error());
                d = refined.value();
            }
            auto factor = blast_factor(projectile.blast, d, radius);
            if (!factor) return Result::failure(factor.error());
            auto amount = math::multiply(projectile.blast.damage, factor.value());
            if (!amount) return Result::failure(amount.error());
            if (!destroyable) {
                auto delay = d.raw() < Fixed::scale ? core::Result<Fixed>::success(Fixed{})
                    : distance_delay(projectile.blast, d, radius);
                if (!delay) return Result::failure(delay.error());
                group.recipients.push_back({id, amount.value(), delay.value()});
            } else {
                // WAD-19/20: eligibility ignores health/targetability; destroyed shares dilute
                // the budget. Exclude the final direct mesh before dividing, not its aimed index.
                std::vector<std::pair<std::uint32_t, Fixed>> selected;
                for (const auto& hardpoint : unit->profile->hardpoints) {
                    const auto index = hardpoint.hardpoint;
                    if (index >= unit->durability_profile->hardpoints.size()
                        || !unit->durability_profile->hardpoints[index].destroyable) continue;
                    if (direct == id && !direct_mesh.empty() && index < unit->profile->hardpoint_meshes.size()
                        && same_mesh(direct_mesh, unit->profile->hardpoint_meshes[index])) continue;
                    auto point = math::transform_point(unit->transform, hardpoint.position);
                    if (!point) return Result::failure(point.error());
                    if (!strictly_within_range(centre, point.value(), radius, RangeMetric::spatial)) continue;
                    auto dx = math::subtract(point.value().x, centre.x);
                    auto dy = math::subtract(point.value().y, centre.y);
                    auto dz = math::subtract(point.value().z, centre.z);
                    if (!dx) return Result::failure(dx.error());
                    if (!dy) return Result::failure(dy.error());
                    if (!dz) return Result::failure(dz.error());
                    auto distance3 = math::length(math::Vec3{dx.value(), dy.value(), dz.value()});
                    if (!distance3) return Result::failure(distance3.error());
                    auto delay = distance_delay(projectile.blast, distance3.value(), radius);
                    if (!delay) return Result::failure(delay.error());
                    selected.emplace_back(index, delay.value());
                }
                // WAD-21/22: freeze the complete authored-order list; no qualifying shares
                // means no hull fallback. F is the object's factor for every share.
                std::sort(selected.begin(), selected.end());
                if (!selected.empty()) {
                    auto share = math::divide(amount.value(), Fixed::from_raw(
                        static_cast<std::int64_t>(selected.size()) * Fixed::scale));
                    if (!share) return Result::failure(share.error());
                    for (const auto& [index, delay] : selected) {
                        auto route = mesh_route(*unit->profile, index);
                        if (!damage_target_valid(*unit->durability_profile, route)) route = hull_target;
                        group.recipients.push_back({id, share.value(), delay, route});
                    }
                }
            }
            // WAD-10/29: stage later players too, since commit can select their unmodified
            // radius after an earlier player group kills the source. Only the selected cap exits.
            if (victims >= projectile.blast.max_victims) { group.capped = true; break; }
        }
    }
    return Result::success(std::move(result));
}

std::string_view direct_blast_mesh(const CombatWorld& world, const ProjectileStep& flight) {
    const auto* target = flight.hit ? world.find(*flight.hit) : nullptr;
    if (target == nullptr || target->profile == nullptr) return {};
    const auto index = flight.meshed ? flight.mesh_hardpoint
        : (*flight.hit == flight.projectile.target ? flight.projectile.target_hardpoint : no_hardpoint);
    const auto& names = target->profile->hardpoint_meshes;
    return index < names.size() ? std::string_view(names[index]) : std::string_view{};
}

core::Result<Hit> area_hit(const Projectile& projectile, const BlastRecipient& recipient, const Fixed defense) {
    auto caused = math::multiply(recipient.amount, projectile.source_damage_factor);
    if (!caused) return core::Result<Hit>::failure(caused.error());
    // WAD-23/24/25: preserve projectile routing inputs, with the secondary selector and area
    // context. The projectile's original aim and direct instance damage do not enter this hit.
    return core::Result<Hit>::success(Hit{caused.value(), projectile.damage_type, true, projectile.shield_damage,
        projectile.hitpoint_damage, recipient.hardpoint, projectile.allow_diminishing_firepower,
        projectile.internal_damage_misc, defense, projectile.energy_damage, true});
}
} // namespace eawr::sim::tactical::detail

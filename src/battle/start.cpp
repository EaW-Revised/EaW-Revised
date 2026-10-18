#include "eawr/battle/start.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <utility>

namespace eawr::battle {

namespace detail {

bool iequals(const std::string_view left, const std::string_view right) noexcept {
    const auto fold = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [&](const char a, const char b) { return fold(a) == fold(b); });
}

} // namespace detail

std::string_view to_string(const UnitRole role) noexcept {
    switch (role) {
    case UnitRole::station: return "station";
    case UnitRole::free_unit: return "free_unit";
    case UnitRole::fleet: return "fleet";
    case UnitRole::map_object: return "map_object";
    case UnitRole::craft: return "craft";
    case UnitRole::special_weapon: return "special_weapon";
    case UnitRole::placed: return "placed";
    }
    return "station";
}

std::string_view to_string(const Removal reason) noexcept {
    switch (reason) {
    case Removal::playable_faction: return "playable_faction";
    case Removal::no_player: return "no_player";
    case Removal::roster_gated: return "roster_gated";
    case Removal::campaign_branch: return "campaign_branch";
    }
    return "playable_faction";
}

std::string_view to_string(const MarkerUse use) noexcept {
    switch (use) {
    case MarkerUse::station: return "station";
    case MarkerUse::base_position: return "base_position";
    case MarkerUse::spawn: return "spawn";
    case MarkerUse::spawn_unused: return "spawn_unused";
    }
    return "spawn_unused";
}

sim::tactical::TypeId type_id(const std::string_view type) noexcept { return assets::object_type_crc(type); }

sim::tactical::FactionId faction_id(const std::string_view faction) noexcept { return assets::object_type_crc(faction); }

core::Result<Vec3> layer_position(Vec3 at, const std::optional<Fixed> layer_z_adjust) {
    auto z = sim::math::add(at.z, layer_z_adjust.value_or(Fixed{}));
    if (!z) return core::Result<Vec3>::failure(z.error());
    at.z = z.value();
    return core::Result<Vec3>::success(at);
}

core::Result<std::vector<Vec3>> tactical_super_weapon_targets(const std::vector<MapPlacement>& placements, const std::string_view own_faction) {
    std::vector<Vec3> targets;
    for (const auto& placement : placements) {
        if (!placement.tactical_super_weapon_target || !placement.position) continue;
        if (!own_faction.empty() && detail::iequals(placement.owner_faction, own_faction)) continue;
        targets.push_back(*placement.position);
    }
    return core::Result<std::vector<Vec3>>::success(std::move(targets));
}

core::Result<TacticalSuperWeaponStart> tactical_super_weapon_start(const units::UnitType::TacticalSuperWeapon& data,
    const Vec3 current, const std::span<const Vec3> targets, const bool owner_defends) {
    using StartResult = core::Result<TacticalSuperWeaponStart>;
    const auto fail = [](core::Diagnostic diagnostic) { return StartResult::failure(std::move(diagnostic)); };
    TacticalSuperWeaponStart start;
    start.position = current;
    const Vec3* nearest = nullptr;
    Fixed nearest_distance{};
    for (const auto& target : targets) {
        const auto dx = sim::math::subtract(target.x, current.x);
        const auto dy = sim::math::subtract(target.y, current.y);
        const auto dz = sim::math::subtract(target.z, current.z);
        if (!dx) return fail(dx.error());
        if (!dy) return fail(dy.error());
        if (!dz) return fail(dz.error());
        const auto distance = sim::math::length(Vec3{dx.value(), dy.value(), dz.value()});
        if (!distance) return fail(distance.error());
        if (nearest == nullptr || distance.value() < nearest_distance) { nearest = &target; nearest_distance = distance.value(); }
    }
    const Fixed distance = owner_defends ? data.attack_distance : data.start_distance;
    if (nearest != nullptr && distance > Fixed{} && nearest_distance > Fixed{}) {
        const auto ox = sim::math::subtract(current.x, nearest->x);
        const auto oy = sim::math::subtract(current.y, nearest->y);
        const auto oz = sim::math::subtract(current.z, nearest->z);
        if (!ox) return fail(ox.error());
        if (!oy) return fail(oy.error());
        if (!oz) return fail(oz.error());
        const auto direction = sim::math::normalize(Vec3{ox.value(), oy.value(), oz.value()});
        if (!direction) return fail(direction.error());
        const auto sx = sim::math::multiply(direction.value().x, distance);
        const auto sy = sim::math::multiply(direction.value().y, distance);
        const auto sz = sim::math::multiply(direction.value().z, distance);
        if (!sx) return fail(sx.error());
        if (!sy) return fail(sy.error());
        if (!sz) return fail(sz.error());
        const auto x = sim::math::add(nearest->x, sx.value());
        const auto y = sim::math::add(nearest->y, sy.value());
        const auto base_z = data.start_pos_match_target_z ? core::Result<Fixed>::success(nearest->z) : sim::math::add(nearest->z, sz.value());
        if (!base_z) return fail(base_z.error());
        const auto z = sim::math::add(base_z.value(), data.z_adjust);
        if (!x) return fail(x.error());
        if (!y) return fail(y.error());
        if (!z) return fail(z.error());
        start.position = {x.value(), y.value(), z.value()};
    }
    if (start.position.x == Fixed{} && start.position.y == Fixed{}) return StartResult::success(start);
    const auto toward_y = sim::math::negate(start.position.y);
    const auto toward_x = sim::math::negate(start.position.x);
    if (!toward_y) return fail(toward_y.error());
    if (!toward_x) return fail(toward_x.error());
    const auto turn = sim::math::atan2_turn(toward_y.value(), toward_x.value());
    if (!turn) return fail(turn.error());
    const auto full = Fixed::from_integer(360);
    if (!full) return fail(full.error());
    const auto yaw = sim::math::multiply(turn.value(), full.value());
    if (!yaw) return fail(yaw.error());
    start.yaw_degrees = yaw.value();
    return StartResult::success(start);
}

} // namespace eawr::battle

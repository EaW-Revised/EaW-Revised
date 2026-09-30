#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::tactical::detail {
namespace {

using math::Fixed;

// Rounded Q24 arithmetic that keeps its first failure.
struct Arithmetic {
    std::optional<core::Diagnostic> error;

    template <typename T>
    T take(core::Result<T> value) {
        if (!value) {
            if (!error) error = value.error();
            return T{};
        }
        return value.value();
    }
};

[[nodiscard]] Fixed absolute(const Fixed value) noexcept { return Fixed::from_raw(std::llabs(value.raw())); }

// DG-37: whether the start, the end or the midpoint of the step lies strictly within `radius` of
// `centre`; exact (squares in 192 bits).
[[nodiscard]] bool within_sphere(const math::Vec3& centre, const Fixed radius, const math::Vec3& from, const math::Vec3& to) {
    namespace wide = math::detail;
    const auto limit = wide::multiply_u64(wide::unsigned_magnitude(radius.raw()), wide::unsigned_magnitude(radius.raw()));
    const auto inside = [&](const math::Vec3& point) {
        wide::UInt192 total{};
        for (const auto& [a, b] : {std::pair{point.x, centre.x}, std::pair{point.y, centre.y}, std::pair{point.z, centre.z}}) {
            const auto d = wide::unsigned_magnitude(a.raw() - b.raw());
            static_cast<void>(wide::add_magnitude(total, wide::multiply_u64(d, d)));
        }
        return wide::compare(total, limit) < 0;
    };
    const auto mid = [](const Fixed a, const Fixed b) { return Fixed::from_raw(a.raw() + (b.raw() - a.raw()) / 2); };
    return inside(from) || inside(to) || inside(math::Vec3{mid(from.x, to.x), mid(from.y, to.y), mid(from.z, to.z)});
}

// DG-37: the collision box placed by `transform` and made axis-aligned (a turned box grows: each
// world half extent is the sum over the model axes of |rotation| x half extent), and its largest
// half extent.
struct WorldBox {
    CollisionBox box{};
    Fixed largest_half{};
};

[[nodiscard]] WorldBox world_box(Arithmetic& q, const CollisionBox& box, const math::Mat3x4& transform) {
    const std::array<Fixed, 3> low{box.min.x, box.min.y, box.min.z};
    const std::array<Fixed, 3> high{box.max.x, box.max.y, box.max.z};
    std::array<Fixed, 3> centre{};
    std::array<Fixed, 3> half{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        centre[axis] = Fixed::from_raw((low[axis].raw() + high[axis].raw()) / 2);
        half[axis] = Fixed::from_raw((high[axis].raw() - low[axis].raw()) / 2);
    }
    WorldBox result;
    std::array<Fixed, 3> world_low{};
    std::array<Fixed, 3> world_high{};
    const auto& m = transform.rows;
    for (std::size_t row = 0; row < 3; ++row) {
        Fixed at = m[row][3];
        Fixed extent{};
        for (std::size_t column = 0; column < 3; ++column) {
            at = q.take(math::add(at, q.take(math::multiply(m[row][column], centre[column]))));
            extent = q.take(math::add(extent, q.take(math::multiply(absolute(m[row][column]), half[column]))));
        }
        world_low[row] = q.take(math::subtract(at, extent));
        world_high[row] = q.take(math::add(at, extent));
        result.largest_half = std::max(result.largest_half, extent);
    }
    result.box = CollisionBox{math::Vec3{world_low[0], world_low[1], world_low[2]},
        math::Vec3{world_high[0], world_high[1], world_high[2]}};
    return result;
}

// The world frame itself: identity rotation, no translation.
const math::Mat3x4 placed_at_origin{{{{Fixed::from_raw(Fixed::scale), Fixed{}, Fixed{}, Fixed{}},
    {Fixed{}, Fixed::from_raw(Fixed::scale), Fixed{}, Fixed{}}, {Fixed{}, Fixed{}, Fixed::from_raw(Fixed::scale), Fixed{}}}}};

using motion_detail::whole;

// FoC's facing difference: degrees wrapped to (-180, 180].
[[nodiscard]] Fixed wrap_difference(const Fixed degrees) noexcept {
    constexpr std::int64_t full = 360 * Fixed::scale;
    constexpr std::int64_t half = 180 * Fixed::scale;
    std::int64_t raw = degrees.raw() % full;
    if (raw > half) {
        raw -= full;
    } else if (raw <= -half) {
        raw += full;
    }
    return Fixed::from_raw(raw);
}

struct Facing {
    Fixed yaw{};
    Fixed pitch{};
};

// MS-02: the facing from `from` toward `to`, FoC's way: yaw in [0, 360) from the planar
// direction (0 with no planar part), pitch = -atan2(dz, planar distance), so a point above has
// a negative pitch (0 with no height difference).
[[nodiscard]] Facing facing_toward(motion_detail::Calc& calc, const math::Vec3& from, const math::Vec3& to) {
    Facing facing;
    const auto dx = calc.sub(to.x, from.x);
    const auto dy = calc.sub(to.y, from.y);
    const auto dz = calc.sub(to.z, from.z);
    if (dx.raw() != 0 || dy.raw() != 0) {
        facing.yaw = calc.atan2_deg(dy, dx);
        if (facing.yaw.raw() < 0) facing.yaw = calc.add(facing.yaw, whole(360));
    }
    if (dz.raw() != 0) {
        facing.pitch = calc.neg(calc.atan2_deg(dz, calc.length(dx, dy)));
    }
    return facing;
}

// The unit direction of a facing: the x axis turned by the pitch about y, then by the yaw about z.
[[nodiscard]] math::Vec3 direction(motion_detail::Calc& calc, const Fixed yaw, const Fixed pitch) {
    const auto cos_pitch = calc.cos_deg(pitch);
    return math::Vec3{calc.mul(cos_pitch, calc.cos_deg(yaw)), calc.mul(cos_pitch, calc.sin_deg(yaw)),
        calc.neg(calc.sin_deg(pitch))};
}

[[nodiscard]] math::Vec3 scaled(motion_detail::Calc& calc, const math::Vec3& value, const Fixed factor) {
    return math::Vec3{calc.mul(value.x, factor), calc.mul(value.y, factor), calc.mul(value.z, factor)};
}

// MS-04: one frame's turn, at most `rate` degrees in yaw and in pitch, the short way.
[[nodiscard]] Facing turned(motion_detail::Calc& calc, const Facing& current, const Facing& wanted, const Fixed rate) {
    const auto limit = [&](const Fixed difference) {
        return std::clamp(wrap_difference(difference), calc.neg(rate), rate);
    };
    Facing next;
    next.yaw = calc.add(current.yaw, limit(calc.sub(wanted.yaw, current.yaw)));
    if (next.yaw >= whole(360)) {
        next.yaw = calc.sub(next.yaw, whole(360));
    } else if (next.yaw.raw() < 0) {
        next.yaw = calc.add(next.yaw, whole(360));
    }
    next.pitch = calc.add(current.pitch, limit(calc.sub(wanted.pitch, current.pitch)));
    if (next.pitch >= whole(360)) {
        next.pitch = calc.sub(next.pitch, whole(360));
    } else if (next.pitch <= whole(-360)) {
        next.pitch = calc.add(next.pitch, whole(360));
    }
    return next;
}

// MS-03: where a homing projectile steers: its target hardpoint, else the target's position, plus
// its offset turned by the target's rotation when every part of the offset is nonzero.
[[nodiscard]] math::Vec3 homing_point(motion_detail::Calc& calc, const CombatUnit& target, const Projectile& projectile) {
    math::Vec3 local{};
    bool on_hardpoint = false;
    if (projectile.target_hardpoint != no_hardpoint && target.profile != nullptr) {
        for (const auto& hardpoint : target.profile->hardpoints) {
            if (hardpoint.hardpoint == projectile.target_hardpoint) {
                local = hardpoint.position;
                on_hardpoint = true;
                break;
            }
        }
    }
    const auto& o = projectile.offset;
    const bool offset = o.x.raw() != 0 && o.y.raw() != 0 && o.z.raw() != 0;
    if (offset) {
        local = math::Vec3{calc.add(local.x, o.x), calc.add(local.y, o.y), calc.add(local.z, o.z)};
    }
    if (!on_hardpoint && !offset) return target.position;
    const auto& m = target.transform.rows;
    const auto row = [&](const std::size_t i, const Fixed origin) {
        return calc.add(origin, calc.add(calc.add(calc.mul(m[i][0], local.x), calc.mul(m[i][1], local.y)), calc.mul(m[i][2], local.z)));
    };
    return math::Vec3{row(0, target.position.x), row(1, target.position.y), row(2, target.position.z)};
}

} // namespace

core::Result<Projectile> launch_projectile(const CombatEvent& shot, const ShotProfile& profile, const PlayerId owner,
    const bool allow_diminishing_firepower, const std::uint64_t id, const math::Vec3& offset) {
    Arithmetic q;
    Projectile projectile;
    projectile.id = id;
    projectile.shooter = shot.shooter;
    projectile.owner = owner;
    projectile.weapon = shot.weapon;
    projectile.target = shot.target;
    projectile.target_hardpoint = shot.target_hardpoint;
    projectile.position = shot.origin;
    projectile.speed = profile.speed;
    projectile.damage = profile.damage;
    projectile.damage_type = profile.damage_type;
    projectile.shield_damage = profile.shield_damage;
    projectile.hitpoint_damage = profile.hitpoint_damage;
    // EN-07, IS-01 (#561): an ion shot drains energy and stuns what it hits.
    projectile.energy_damage = profile.energy_damage;
    projectile.ion_stun = profile.ion_stun;
    // DG-05: the shooter's mode flag and the projectile's internal damage type, both snapshotted at
    // the shot (docs/behaviour/space-damage.md DP-05).
    projectile.allow_diminishing_firepower = allow_diminishing_firepower;
    projectile.internal_damage_misc = profile.internal_damage_misc;
    // DG-22: it flies straight at the (scattered) aim point at Max_Speed per frame.
    const math::Vec3 delta{q.take(math::subtract(shot.aim.x, shot.origin.x)),
        q.take(math::subtract(shot.aim.y, shot.origin.y)), q.take(math::subtract(shot.aim.z, shot.origin.z))};
    const auto distance = q.take(math::length(delta));
    if (distance.raw() > 0) {
        const auto scale = q.take(math::divide(profile.speed, distance));
        projectile.step = math::Vec3{q.take(math::multiply(delta.x, scale)), q.take(math::multiply(delta.y, scale)),
            q.take(math::multiply(delta.z, scale))};
    }
    // DG-23: its travel ends at the weapon's range (the soft radius is zero) plus the height
    // between its origin and aim point.
    projectile.max_travel = q.take(math::add(profile.max_travel, absolute(delta.z)));
    if (q.error) return core::Result<Projectile>::failure(*q.error);
    // MS-02: a homing projectile faces the aim point and holds its target.
    if (profile.homing) {
        motion_detail::Calc calc;
        const auto facing = facing_toward(calc, shot.origin, shot.aim);
        projectile.homing = true;
        projectile.locked = shot.target != invalid_entity_id;
        projectile.turn_rate = profile.turn_rate;
        projectile.yaw = facing.yaw;
        projectile.pitch = facing.pitch;
        projectile.offset = offset;
        projectile.step = scaled(calc, direction(calc, facing.yaw, facing.pitch), profile.speed);
        if (!calc.ok()) return core::Result<Projectile>::failure(calc.error("missile launch"));
    }
    return core::Result<Projectile>::success(projectile);
}

core::Result<ProjectileStep> step_projectile(const CombatWorld& world, const Projectile& projectile, const TeamId owner_team,
    ProjectileScratch& scratch) {
    Arithmetic q;
    ProjectileStep result;
    result.projectile = projectile;
    result.from = projectile.position;
    // MS-03 to MS-05: a homing projectile that holds its target turns toward it, then flies along
    // its new facing; once the target has left it keeps its last facing and never takes another.
    if (projectile.homing && projectile.locked) {
        const auto* target = world.find(projectile.target);
        if (target == nullptr) {
            result.projectile.locked = false;
        } else {
            motion_detail::Calc calc;
            const auto wanted = facing_toward(calc, projectile.position, homing_point(calc, *target, projectile));
            const auto next = turned(calc, Facing{projectile.yaw, projectile.pitch}, wanted, projectile.turn_rate);
            result.projectile.yaw = next.yaw;
            result.projectile.pitch = next.pitch;
            result.projectile.step = scaled(calc, direction(calc, next.yaw, next.pitch), projectile.speed);
            if (!calc.ok()) return core::Result<ProjectileStep>::failure(calc.error("missile steering"));
        }
    }
    const auto& step = result.projectile.step;
    const auto& from = projectile.position;
    const math::Vec3 to{q.take(math::add(from.x, step.x)), q.take(math::add(from.y, step.y)),
        q.take(math::add(from.z, step.z))};
    if (q.error) return core::Result<ProjectileStep>::failure(*q.error);

    // Broad phase: unit positions within the segment's box grown by the largest collision reach.
    const auto half = [&](const Fixed a, const Fixed b) {
        return Fixed::from_raw(std::llabs(b.raw() - a.raw()) / 2 + world.collision_reach.raw());
    };
    const auto mid = [](const Fixed a, const Fixed b) { return Fixed::from_raw(a.raw() + (b.raw() - a.raw()) / 2); };
    const math::Vec3 centre{mid(from.x, to.x), mid(from.y, to.y), mid(from.z, to.z)};
    const math::Vec3 extent{half(from.x, to.x), half(from.y, to.y), half(from.z, to.z)};
    // #636: every point of the segment lies within this of its midpoint (half its length in each
    // axis summed, which is at least half its length, and 2 for the midpoint's rounding).
    const auto half_length = (std::llabs(to.x.raw() - from.x.raw()) + std::llabs(to.y.raw() - from.y.raw())
                                 + std::llabs(to.z.raw() - from.z.raw())) / 2 + 2;
    // DG-32: any unit of another team whose type has a collision box. #636: and whose own reach
    // about its position meets the segment; the rest cannot be hit, so the exact tests below
    // (the costly model-space transform) never see them.
    world.index.box_positions(centre, extent, scratch.candidates);
    scratch.near.clear();
    for (const auto position : scratch.candidates) {
        const auto& unit = world.units[position];
        if (unit.team == owner_team || unit.profile == nullptr || !unit.profile->collision) continue;
        const auto reach = unit.collision_reach.raw();
        if (reach <= std::numeric_limits<std::int64_t>::max() - half_length
            && !within_range(centre, unit.position, Fixed::from_raw(reach + half_length), RangeMetric::spatial)) {
            continue;
        }
        scratch.near.push_back(position);
    }
    // The index keeps the units' ascending-ID order, so ascending positions are ascending IDs.
    std::sort(scratch.near.begin(), scratch.near.end());
    scratch.candidate_count += scratch.candidates.size();
    scratch.exact_count += scratch.near.size();
    std::optional<Fixed> best;
    for (const auto position : scratch.near) {
        const auto* unit = &world.units[position];
        const auto id = unit->id;
        const auto& profile = *unit->profile;
        std::optional<Fixed> entry;
        std::uint32_t mesh_hardpoint = no_hardpoint;
        if (profile.meshes.empty()) {
            auto box = segment_enters_box(*profile.collision, unit->transform, from, to);
            if (!box) return core::Result<ProjectileStep>::failure(box.error());
            entry = box.value();
        } else {
            // DG-36: the meshes, gated by the box around them. DG-38: the shield mesh only for a
            // shield-damaging projectile while the shield is up; a destroyed hardpoint's
            // attached model no longer collides.
            auto gate = segment_enters_box(*profile.mesh_bounds, unit->transform, from, to);
            if (!gate) return core::Result<ProjectileStep>::failure(gate.error());
            if (gate.value()) {
                const auto enabled = [&](const std::size_t index) {
                    const auto& mesh = profile.meshes[index];
                    if (mesh.shield) {
                        return projectile.shield_damage && unit->durability != nullptr
                            && unit->durability->shields.raw() > 0;
                    }
                    const auto source = mesh.source_hardpoint;
                    if (source == no_hardpoint || unit->durability == nullptr || unit->durability_profile == nullptr
                        || source >= unit->durability_profile->hardpoints.size()
                        || source >= unit->durability->hardpoints.size()) {
                        return true;
                    }
                    return !(unit->durability_profile->hardpoints[source].max_health.raw() > 0
                             && unit->durability->hardpoints[source].raw() <= 0);
                };
                auto meshes = segment_hits_meshes(profile.meshes, enabled, unit->transform, from, to);
                if (!meshes) return core::Result<ProjectileStep>::failure(meshes.error());
                if (meshes.value()) {
                    entry = meshes.value()->fraction;
                    mesh_hardpoint = profile.meshes[meshes.value()->mesh].hardpoint;
                }
            }
        }
        // DG-37: a small craft is also hit, at the step's start, when the step meets its world box
        // and starts, ends or has its midpoint within its sphere.
        if (!entry && profile.sphere_modifier) {
            const auto placed = world_box(q, *profile.collision, unit->transform);
            if (q.error) return core::Result<ProjectileStep>::failure(*q.error);
            auto met = segment_enters_box(placed.box, placed_at_origin, from, to);
            if (!met) return core::Result<ProjectileStep>::failure(met.error());
            if (met.value()) {
                const auto radius = q.take(math::multiply(placed.largest_half, *profile.sphere_modifier));
                if (q.error) return core::Result<ProjectileStep>::failure(*q.error);
                if (within_sphere(unit->position, radius, from, to)) {
                    entry = Fixed{};
                    mesh_hardpoint = no_hardpoint;
                }
            }
        }
        // #669 (DG-39): a shot that reaches the unit it was fired at, aimed at one of its
        // hardpoints, damages the hardpoint that aim names, whichever mesh it met.
        if (entry && !profile.meshes.empty() && id == projectile.target && projectile.target_hardpoint != no_hardpoint
            && projectile.target_hardpoint < profile.aimed_routes.size()) {
            mesh_hardpoint = profile.aimed_routes[projectile.target_hardpoint];
        }
        // The first along the segment; on a tie the lowest ID (the candidates ascend).
        if (entry && (!best || *entry < *best)) {
            best = entry;
            result.hit = id;
            result.mesh_hardpoint = mesh_hardpoint;
            result.meshed = !profile.meshes.empty();
        }
    }
    if (best) {
        result.contact = math::Vec3{q.take(math::add(from.x, q.take(math::multiply(step.x, *best)))),
            q.take(math::add(from.y, q.take(math::multiply(step.y, *best)))),
            q.take(math::add(from.z, q.take(math::multiply(step.z, *best))))};
    }
    // DG-33: it moves on and counts its travel; without a hit it ends at its maximum travel.
    result.projectile.position = to;
    result.projectile.travelled = q.take(math::add(projectile.travelled, projectile.speed));
    result.expired = !result.hit && result.projectile.travelled >= projectile.max_travel;
    if (q.error) return core::Result<ProjectileStep>::failure(*q.error);
    return core::Result<ProjectileStep>::success(std::move(result));
}

void append_projectile(std::vector<std::uint8_t>& bytes, const Projectile& projectile) {
    sim::detail::append_u64(bytes, projectile.id);
    sim::detail::append_u64(bytes, projectile.shooter);
    sim::detail::append_u64(bytes, projectile.target);
    sim::detail::append_u32(bytes, projectile.owner);
    sim::detail::append_u32(bytes, projectile.weapon);
    sim::detail::append_u32(bytes, projectile.target_hardpoint);
    sim::detail::append_u32(bytes, projectile.damage_type);
    // Bits 2 and 3 are the DG-05 gates' negation, so the M2 default (both gates true) keeps this
    // word's value exactly as before #440; bits 4 and 5 mark a homing projectile (#361); bits 6
    // and 7 an energy-damage and an ion-stun projectile (#561), so no earlier projectile changes.
    sim::detail::append_u32(bytes, (projectile.shield_damage ? 1U : 0U) | (projectile.hitpoint_damage ? 2U : 0U)
            | (projectile.allow_diminishing_firepower ? 0U : 4U) | (projectile.internal_damage_misc ? 0U : 8U)
            | (projectile.homing ? 16U : 0U) | (projectile.locked ? 32U : 0U) | (projectile.energy_damage ? 64U : 0U)
            | (projectile.ion_stun ? 128U : 0U));
    sim::detail::append_u32(bytes, 0);
    for (const auto& point : {projectile.position, projectile.step}) {
        sim::detail::append_i64(bytes, point.x.raw());
        sim::detail::append_i64(bytes, point.y.raw());
        sim::detail::append_i64(bytes, point.z.raw());
    }
    for (const auto value : {projectile.speed, projectile.travelled, projectile.max_travel, projectile.damage}) {
        sim::detail::append_i64(bytes, value.raw());
    }
    // A homing projectile (#361) appends its turn rate, facing and target-frame offset.
    if (projectile.homing) {
        for (const auto value : {projectile.turn_rate, projectile.yaw, projectile.pitch, projectile.offset.x,
                 projectile.offset.y, projectile.offset.z}) {
            sim::detail::append_i64(bytes, value.raw());
        }
    }
    // An ion-stun projectile (#561) appends its stun.
    if (projectile.ion_stun) {
        sim::detail::append_u32(bytes, projectile.ion_stun->frames);
        sim::detail::append_i64(bytes, projectile.ion_stun->speed_reduction.raw());
        sim::detail::append_i64(bytes, projectile.ion_stun->rate_reduction.raw());
        sim::detail::append_u32(bytes, projectile.ion_stun->stack ? 1U : 0U);
    }
}

} // namespace eawr::sim::tactical::detail

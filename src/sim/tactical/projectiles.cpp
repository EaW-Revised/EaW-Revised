#include "../math/wide.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "motion_internal.hpp"
#include "rocket_internal.hpp"
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
    const bool allow_diminishing_firepower, const std::uint64_t id, const math::Vec3& offset, const math::Fixed target_radius) {
    Arithmetic q;
    Projectile projectile;
    projectile.id = id;
    if (profile.appearance_delay_frames != 0) {
        if (shot.tick > std::numeric_limits<std::uint64_t>::max() - profile.appearance_delay_frames) {
            return core::Result<Projectile>::failure(diagnostic(diagnostic_codes::invalid_setup, "projectile appearance frame overflow"));
        }
        projectile.muzzle_delay_until = shot.tick + profile.appearance_delay_frames;
    }
    projectile.shooter = shot.shooter;
    projectile.owner = owner;
    projectile.weapon = shot.weapon;
    projectile.target = shot.target;
    projectile.target_hardpoint = shot.target_hardpoint;
    projectile.position = shot.origin;
    projectile.speed = profile.speed;
    projectile.damage = profile.damage;
    projectile.blast = profile.blast;
    projectile.damage_type = profile.damage_type;
    projectile.shield_damage = profile.shield_damage;
    projectile.hitpoint_damage = profile.hitpoint_damage;
    // EN-07, IS-01 (#561): an ion shot drains energy and stuns what it hits.
    projectile.energy_damage = profile.energy_damage;
    projectile.disable_engines_frames = profile.disable_engines_frames;
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
    // DG-23: travel includes the target's soft radius and the origin-to-aim height difference.
    projectile.max_travel = q.take(math::add(q.take(math::add(profile.max_travel, target_radius)), absolute(delta.z)));
    if (profile.flight) {
        if (!valid_flight(*profile.flight) || (profile.homing && profile.flight->kind == FlightKind::rocket)) {
            return core::Result<Projectile>::failure(diagnostic(diagnostic_codes::invalid_setup,
                "unsupported flight profile or rocket classified as homing (WAD-04)"));
        }
        projectile.flight = FlightState{};
        projectile.flight->profile = *profile.flight;
        projectile.flight->origin = shot.origin;
        projectile.flight->aim = shot.aim;
    }
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

core::Result<ProjectileStep> step_projectile(const CombatWorld& world, const Projectile& projectile,
    ProjectileScratch& scratch) {
    Arithmetic q;
    ProjectileStep result;
    result.projectile = projectile;
    result.from = projectile.position;
    if (projectile.explosion_requested) {
        result.expired = true;
        return core::Result<ProjectileStep>::success(std::move(result));
    }
    // WAD-37/40: equality makes it visible but still does not move it. The source resets
    // flight age on that equality frame, so path age begins with the next movement.
    if (projectile.muzzle_delay_until != 0 && world.frame <= projectile.muzzle_delay_until) {
        return core::Result<ProjectileStep>::success(std::move(result));
    }
    std::optional<math::Vec3> path_point;
    if (result.projectile.flight) {
        auto& flight = *result.projectile.flight;
        ++flight.age_frames;
        if (flight.profile.kind == FlightKind::rocket) {
            if (!flight.path_initialized) {
                auto prepared = prepare_rocket_path(result.projectile);
                if (!prepared) return core::Result<ProjectileStep>::failure(prepared.error());
            }
            flight.path_distance = q.take(math::add(flight.path_distance, projectile.speed));
            auto point = rocket_point(flight, flight.path_distance);
            if (!point) return core::Result<ProjectileStep>::failure(point.error());
            if (!point.value()) {
                // RFL-06 / WAD-04: exhausted lookup forces terminal advancement at
                // the current pose without another collision query or snapping to aim.
                result.projectile.travelled = q.take(math::add(projectile.travelled, projectile.speed));
                result.expired = true;
                if (q.error) return core::Result<ProjectileStep>::failure(*q.error);
                return core::Result<ProjectileStep>::success(std::move(result));
            }
            path_point = *point.value();
            result.projectile.step = {q.take(math::subtract(path_point->x, projectile.position.x)),
                q.take(math::subtract(path_point->y, projectile.position.y)),
                q.take(math::subtract(path_point->z, projectile.position.z))};
        }
    }
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
    const math::Vec3 to = path_point.value_or(math::Vec3{q.take(math::add(from.x, step.x)),
        q.take(math::add(from.y, step.y)), q.take(math::add(from.z, step.z))});
    if (q.error) return core::Result<ProjectileStep>::failure(*q.error);

    if (world.projectile_collection == nullptr) {
        return core::Result<ProjectileStep>::failure(diagnostic(diagnostic_codes::worker_failure,
            "projectile collision requires its persistent collection"));
    }
    // DG-30: players ascend, but contacts within each player's persistent tree follow its ray
    // collection order. A nearer object or lower ID never displaces the first eligible contact.
    scratch.near.clear();
    for (const auto& player : world.relationships) {
        if (!world.hostile(projectile.owner, player.player_id)) continue;
        world.projectile_collection->ray_collect(player.player_id, from, to, scratch.contacts);
        scratch.candidate_count += scratch.contacts.size();
        for (const auto id : scratch.contacts) {
            const auto* unit = world.find(id);
            if (unit == nullptr || id == projectile.shooter || unit->profile == nullptr
                || !unit->profile->collision || !unit->profile->living_projectile_collision) continue;
            scratch.near.push_back(static_cast<std::uint32_t>(unit - world.units.data()));
        }
    }
    std::optional<Fixed> best;
    for (const auto position : scratch.near) {
        ++scratch.exact_count;
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
                            && unit->durability->shields.raw() > 0 && (world.damage == nullptr
                                || (!shield_depleted(*world.damage, *unit->durability, world.frame)
                                    && !in_ion_storm(*world.damage, *unit->durability, world.frame)));
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
        if (entry) {
            best = entry;
            result.hit = id;
            result.mesh_hardpoint = mesh_hardpoint;
            result.meshed = !profile.meshes.empty();
            break;
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
    bool terminal = result.projectile.travelled >= projectile.max_travel;
    if (result.projectile.flight) {
        const auto& flight = *result.projectile.flight;
        terminal = projectile.max_travel.raw() > 0
            ? result.projectile.travelled >= projectile.max_travel
            : flight.profile.lifetime && flight.age_frames
                > static_cast<std::uint64_t>(flight.profile.lifetime->raw() * 30 / Fixed::scale);
        if (flight.profile.target_radius) {
            if (flight.profile.kind == FlightKind::rocket) {
                terminal = terminal || flight.path_distance >= flight.profile.authored_distance;
            } else if (flight.profile.kind == FlightKind::default_projectile) {
                // RFL-08: exact squared spatial displacement; equality does not expire.
                namespace wide = math::detail;
                const auto squared = [&](const math::Vec3& point) {
                    wide::UInt192 total{};
                    for (const auto& [a, b] : {std::pair{point.x, flight.origin.x},
                         std::pair{point.y, flight.origin.y}, std::pair{point.z, flight.origin.z}}) {
                        const auto magnitude = wide::unsigned_magnitude(a.raw() - b.raw());
                        static_cast<void>(wide::add_magnitude(total, wide::multiply_u64(magnitude, magnitude)));
                    }
                    return total;
                };
                terminal = terminal || wide::compare(squared(to), squared(flight.aim)) > 0;
            }
        }
    }
    result.expired = !result.hit && terminal;
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
            | (projectile.ion_stun ? 128U : 0U) | (projectile.blast.enabled() ? 256U : 0U)
            | (projectile.explosion_requested ? 512U : 0U) | (projectile.flight ? 1024U : 0U)
            | (projectile.muzzle_delay_until != 0 ? 2048U : 0U)
            | (projectile.disable_engines_frames ? 4096U : 0U)); // coordinator-reserved bit 12, EN-08
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
    if (projectile.disable_engines_frames) sim::detail::append_u32(bytes, *projectile.disable_engines_frames);
    if (projectile.blast.enabled()) {
        const auto& blast = projectile.blast;
        for (const auto value : {blast.damage, blast.radius, blast.max_delay}) sim::detail::append_i64(bytes, value.raw());
        sim::detail::append_u32(bytes, blast.dropoff ? 1U : 0U);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(blast.tiers));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(blast.max_victims));
        sim::detail::append_u32(bytes, blast.immune_faction ? 1U : 0U);
        sim::detail::append_u64(bytes, blast.immune_faction.value_or(0));
        sim::detail::append_i64(bytes, projectile.source_damage_factor.raw());
    }
    if (projectile.flight) {
        const auto& flight = *projectile.flight;
        const auto& profile = flight.profile;
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(profile.kind));
        sim::detail::append_u32(bytes, profile.target_radius ? 1U : 0U);
        sim::detail::append_u32(bytes, profile.lifetime ? 1U : 0U);
        if (profile.lifetime) sim::detail::append_i64(bytes, profile.lifetime->raw());
        for (const auto value : {profile.authored_distance, profile.curve_distance, profile.curve_offset, profile.straight_distance}) {
            sim::detail::append_i64(bytes, value.raw());
        }
        for (const auto& point : {flight.origin, flight.aim}) {
            sim::detail::append_i64(bytes, point.x.raw()); sim::detail::append_i64(bytes, point.y.raw());
            sim::detail::append_i64(bytes, point.z.raw());
        }
        sim::detail::append_u64(bytes, flight.age_frames);
        sim::detail::append_i64(bytes, flight.path_distance.raw());
        sim::detail::append_u32(bytes, flight.path_initialized ? 1U : 0U);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(flight.path.size()));
        for (const auto& segment : flight.path) {
            for (const auto& coefficient : {segment.a, segment.b, segment.c, segment.d}) {
                sim::detail::append_i64(bytes, coefficient.x.raw()); sim::detail::append_i64(bytes, coefficient.y.raw());
                sim::detail::append_i64(bytes, coefficient.z.raw());
            }
            sim::detail::append_i64(bytes, segment.length.raw());
        }
    }
    if (projectile.muzzle_delay_until != 0) sim::detail::append_u64(bytes, projectile.muzzle_delay_until);
}

} // namespace eawr::sim::tactical::detail

#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "battle_effects_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_effects_detail;

namespace {

// Every live particle effect draws from its own capacity; the batch keeps at most this many
// effects alive at once and reports the rest as dropped.
constexpr std::size_t effect_capacity = 4096;
constexpr std::size_t max_live_effects = 256;
// A detached effect that never reports finished is released after this many further samples.
constexpr std::uint32_t drain_limit_frames = 300;
// The report keeps this many placed shield hits.
constexpr std::size_t shield_sample_limit = 64;
[[nodiscard]] space::Vec3d dvec(const V value) { return {value.x, value.y, value.z}; }

[[nodiscard]] particles::Basis3 yaw_basis(const double yaw_degrees) {
    const double radians = yaw_degrees * 3.14159265358979323846 / 180.0;
    const auto c = static_cast<float>(std::cos(radians));
    const auto s = static_cast<float>(std::sin(radians));
    return {{c, s, 0.0F}, {-s, c, 0.0F}, {0.0F, 0.0F, 1.0F}};
}
} // namespace

const std::string* BattleEffects::hit_pick(const std::vector<std::string>& list, const space::HitParticleList kind,
                                           const tactical::CombatEvent& event, const SnapshotAt& snapshot_at) {
    if (list.empty()) return nullptr;
    std::optional<std::uint64_t> projectile;
    if (snapshot_at) {
        // A hit's tick names the snapshot before its simulation step; the result is published at tick + 1.
        if (const auto before = snapshot_at(event.tick)) projectile = space::hit_projectile(before->projectiles(), event);
    }
    ++(projectile ? hit_picks_by_projectile_ : hit_picks_by_event_);
    return &list[space::hit_particle_pick(projectile.value_or(space::hit_event_key(event)), kind, list.size())];
}

bool BattleEffects::spawn(const std::string& particle, const std::array<double, 3>& position,
                          const particles::Basis3& basis, const std::string& reason, const std::uint64_t tick) {
    if (particle.empty()) return true;
    const std::string key = reason + ":" + particle;
    const ParticleType* type = particle_type(particle);
    if (type == nullptr || !type->system) {
        ++spawn_failed_[key];
        return true;
    }
    // #370 re-review 2: one whose lifetime is over by the frame that reaches its tick (a stall,
    // or a session that ran ahead of the first frame) would only be spawned to detach at once.
    if (type->lifetime_frames > 0 && due_ > birth_ && due_ - birth_ >= type->lifetime_frames) {
        ++expired_[key];
        if (spawn_log_.size() < spawn_log_limit) spawn_log_.push_back({tick, key, std::nullopt});
        return true;
    }
    if (effects_.size() >= max_live_effects) {
        ++effects_dropped_;
        return true;
    }
    auto handle = registry_->spawn(*type->system, seed_++, effect_capacity);
    if (!handle) {
        ++spawn_failed_[key];
        return true;
    }
    particles::EmitterFrame frame;
    frame.origin = {static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])};
    frame.basis = basis;
    if (!registry_->set_frame(handle.value(), frame)) {
        static_cast<void>(registry_->release(handle.value()));
        ++spawn_failed_[key];
        return true;
    }
    ++spawned_[key];
    const std::size_t row = spawn_log_.size() < spawn_log_limit ? spawn_log_.size() : spawn_log_limit;
    if (row < spawn_log_limit) spawn_log_.push_back({tick, key, std::nullopt});
    LiveEffect effect{handle.value(), particle, birth_, 0U, type->lifetime_frames, false, false, row};
    // Born before the clock's present (a tick the frames had already passed): it catches up.
    bool gone = false;
    for (std::uint64_t sample = birth_; sample < samples_ && !gone; ++sample) {
        if (!step_effect(effect, gone)) return false;
    }
    if (!gone) effects_.push_back(std::move(effect));
    max_live_effects_ = std::max<std::uint64_t>(max_live_effects_, effects_.size());
    return true;
}

bool BattleEffects::step_effect(LiveEffect& effect, bool& gone) {
    gone = false;
    auto advanced = registry_->advance(effect.handle, 1.0F / 30.0F, camera_frame_);
    if (!advanced) {
        failure_ = "battle effect " + effect.particle + ": " + core::format_diagnostic(advanced.error());
        return false;
    }
    after_step(effect, advanced.value(), gone);
    return true;
}

void BattleEffects::after_step(LiveEffect& effect, const particles::EffectFrameStats& advanced, bool& gone) {
    gone = false;
    particles_ += advanced.particles;
    ++effect.age;
    if (!effect.detached && effect.age >= effect.lifetime) {
        // The particle object's lifetime ends: FoC detaches its system, which drains.
        auto detached = registry_->detach(effect.handle);
        effect.detached = true;
        gone = !detached || detached.value() == particles::EffectDetachState::released;
    } else if (effect.detached && (advanced.finished || effect.age >= effect.lifetime + drain_limit_frames)) {
        static_cast<void>(registry_->release(effect.handle));
        gone = true;
    }
}

bool BattleEffects::advance_until(const std::uint64_t target) {
    for (; samples_ < target; ++samples_) {
        particles_ = 0;
        // #638: the effects born by this sample step in one batch on the particle workers; each
        // then ages, detaches or goes in effect order, as one advance after another did.
        batch_handles_.clear();
        for (const LiveEffect& effect : effects_) {
            if (effect.born <= samples_) batch_handles_.push_back(effect.handle);
        }
        if (auto advanced = registry_->advance_all(batch_handles_, 1.0F / 30.0F, camera_frame_, batch_stats_);
            !advanced) {
            failure_ = "battle effect: " + core::format_diagnostic(advanced.error());
            return false;
        }
        std::size_t next = 0;
        for (auto effect = effects_.begin(); effect != effects_.end();) {
            bool gone = false;
            if (effect->born <= samples_) after_step(*effect, batch_stats_[next++], gone);
            effect = gone ? effects_.erase(effect) : effect + 1;
        }
    }
    return true;
}

bool BattleEffects::frame(const std::span<const platform::LiveTickEvents> reached,
                          const tactical::TacticalSnapshot& previous, const tactical::TacticalSnapshot& latest,
                          const double alpha, const UnitLookup& units, const FixedCamera& camera,
                          const double presented_tick, const SnapshotAt& snapshot_at) {
    if (released_) return true;
    ++frames_;
    // The clock starts at the first frame, or earlier at the birth of the oldest event that frame
    // reaches: a session that ran ahead of its first frame (#370 re-review 2) still gives each
    // effect its own tick's birth and ages it by the ticks since.
    if (!clock_start_) {
        clock_start_ = presented_tick;
        if (!reached.empty()) {
            clock_start_ = std::min(presented_tick, static_cast<double>(reached.front().tick) - 1.0);
        }
    }
    camera_frame_ = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    // The clock sample of presented tick `tick`, rounded up to a whole sample.
    const auto sample_of = [this](const double tick) {
        return static_cast<std::uint64_t>(std::max(0.0, std::ceil(tick - *clock_start_ - 1.0e-9)));
    };
    const auto due = static_cast<std::uint64_t>(std::max(0.0, std::floor(presented_tick - *clock_start_ + 1.0e-9)));
    due_ = due;
    note_types(previous, latest);
    // Where each unit stood the last frame the local player saw it; one that is in the session
    // but hidden now is forgotten, so what stays for a unit that left is its last visible frame.
    for (const tactical::TacticalInstance& instance : latest.instances()) {
        entity_types_[instance.entity_id] = instance.type_id;
        if (const auto unit = units(instance.entity_id)) last_seen_[instance.entity_id] = *unit;
        else last_seen_.erase(instance.entity_id);
    }
    // #447: a killed craft spinning away is followed the same way until its spin ends.
    for (const tactical::SpinningCraft& craft : latest.spinning()) {
        if (const auto unit = units(craft.entity_id)) last_seen_[craft.entity_id] = *unit;
        else last_seen_.erase(craft.entity_id);
    }
    note_ability_shots(reached, latest, snapshot_at);
    // Events fire on the frame that first reaches their tick, oldest tick first; the effects
    // already live are aged up to each tick's birth before its effects are born.
    for (const platform::LiveTickEvents& record : reached) {
        birth_ = sample_of(static_cast<double>(record.tick) - 1.0);
        if (!advance_until(std::min(birth_, due))) return false;
        // WHE-63: successful refill emits the authored particle on every current team member.
        // A full-team no-op leaves started_tick/recharge unchanged and emits nothing.
        const auto after_refill = snapshot_at ? snapshot_at(record.tick) : nullptr;
        if (after_refill) for (const auto& instance : after_refill->instances()) {
            const auto active = std::find_if(instance.abilities.begin(), instance.abilities.end(), [&](const auto& ability) {
                return ability.kind == tactical::AbilityKind::replenish_wingmen && ability.remaining_frames > 0
                    && ability.started_tick + 1 == record.tick;
            });
            if (active == instance.abilities.end()) continue;
            const auto type = types_.find(instance.type_id);
            if (type == types_.end() || type->second.replenish_particle.empty()) continue;
            const auto group = std::find_if(after_refill->squadrons().begin(), after_refill->squadrons().end(), [&](const auto& team) {
                return std::find(team.members.begin(), team.members.end(), instance.entity_id) != team.members.end();
            });
            if (group == after_refill->squadrons().end()) continue;
            for (const auto member : group->members) {
                const auto shown = units(member);
                if (shown && !spawn(type->second.replenish_particle, shown->position, {}, "hero_wingmen", record.tick)) return false;
            }
        }
        // WHE-62: countdown expiration presents the spawned type's authored detonation.
        const auto before_spawn = snapshot_at && record.tick > 0 ? snapshot_at(record.tick - 1) : nullptr;
        if (before_spawn) for (const auto& spawn_state : before_spawn->ability_spawns()) {
            if (spawn_state.detonated || spawn_state.due + 1 != record.tick || !units(spawn_state.source)) continue;
            const auto type = types_.find(spawn_state.type);
            if (type == types_.end()) continue;
            const auto look = type->second.weapons.find(tactical::object_weapon);
            if (look == type->second.weapons.end()) continue;
            const std::array<double, 3> position{to_float(spawn_state.position.x), to_float(spawn_state.position.y), to_float(spawn_state.position.z)};
            if (!spawn(look->second.lifetime_detonation, position, {}, "hero_detonation", record.tick)) return false;
        }
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            // Shown when the local player sees the target: a lethal hit's target has left the
            // session by that tick, and its last visible frame stands for it (#370 review 1).
            if (!last_seen_.contains(event.target)) continue;
            ++hit_events_[record.tick];
            const auto shooter = entity_types_.find(event.shooter);
            const ProjectileLook* found = nullptr;
            // #862 (AB-66): an ability shot's hit shows the ability shot's own particles.
            std::optional<std::uint64_t> hit;
            if (snapshot_at) {
                if (const auto before = snapshot_at(event.tick)) hit = space::hit_projectile(before->projectiles(), event);
            }
            const bool ability = hit && ability_projectiles_.contains(*hit);
            const bool barrage = hit && barrage_projectiles_.contains(*hit);
            if (shooter != entity_types_.end()) {
                if (const auto type = types_.find(shooter->second); type != types_.end()) {
                    const auto& weapons = ability && type->second.ability_weapons.contains(event.weapon)
                        ? type->second.ability_weapons
                        : barrage && type->second.barrage_weapons.contains(event.weapon)
                            ? type->second.barrage_weapons : type->second.weapons;
                    if (const auto weapon = weapons.find(event.weapon); weapon != weapons.end()) {
                        found = &weapon->second;
                    }
                }
            }
            if (found == nullptr) {
                ++spawn_failed_["hit:<unknown weapon>"];
                continue;
            }
            const ProjectileLook& look = *found;
            const V contact = vec(event.aim);
            const std::array<double, 3> at{contact.x, contact.y, contact.z};
            if ((event.outcome & tactical::hit_outcome_shield_absorbed) != 0U) {
                // BP-17 to BP-19: where the projectile's flight, cast from its frame-step start,
                // first meets the target's collision meshes (its SHIELD mesh among them), facing
                // that triangle's normal when the model has a SHIELD sub-object and back along the
                // flight when it has none.
                const V flight = sub(contact, vec(event.origin));
                std::array<double, 3> place = at;
                std::optional<space::Vec3d> normal;
                const UnitFrame& target = last_seen_.at(event.target);
                const auto target_looks = types_.find(target.type);
                std::optional<space::ShieldSegmentHit> hit;
                if (target_looks != types_.end() && !target_looks->second.collision.triangles.empty()) {
                    const space::LivePose pose{target.position, target.yaw_degrees, target.roll_degrees,
                                               target_looks->second.scale};
                    if (const auto met = space::shield_flight_hit(target_looks->second.collision, pose,
                                                                  dvec(vec(event.origin)), dvec(flight),
                                                                  look.step_length, &shield_casts_)) {
                        hit = met->hit;
                        ++(met->in_step ? shield_in_step_ : shield_ahead_);
                    }
                }
                if (hit) place = hit->contact;
                if (target_looks == types_.end() || !target_looks->second.shield_mesh) {
                    ++shield_no_mesh_;
                } else if (hit) {
                    normal = hit->normal;
                    ++shield_on_mesh_;
                } else {
                    ++shield_mesh_missed_;
                }
                const ParticleType* particle = particle_type(look.shield_absorbed);
                const space::Vec3d direction = space::shield_hit_direction(normal, dvec(flight));
                const space::ShieldHitAxes axes = space::shield_hit_axes(direction,
                                                                         particle != nullptr && particle->attached_to_collision);
                if (shield_samples_.size() < shield_sample_limit) {
                    shield_samples_.push_back({record.tick, event.target, at, place, direction});
                }
                const particles::Basis3 basis{vec(axes.x), vec(axes.y), vec(axes.z)};
                if (!spawn(look.shield_absorbed, place, basis, "shield", record.tick)) return false;
                // BP-20, BP-63: then one of the target type's Shield_Hit_Particles, at the same
                // place and facing.
                if (target_looks != types_.end()) {
                    if (const std::string* pick = hit_pick(target_looks->second.shield_hits, space::HitParticleList::shield,
                                                           event, snapshot_at);
                        pick != nullptr && !spawn(*pick, place, basis, "shield_hit", record.tick)) {
                        return false;
                    }
                }
                continue;
            }
            if ((event.outcome & tactical::hit_outcome_armor_reduced) != 0U && !look.armor_reduced.empty()) {
                if (!spawn(look.armor_reduced, at, {}, "armor_reduced", record.tick)) return false;
            } else if (!spawn(look.detonation, at, {}, "detonation", record.tick)) {
                return false;
            }
            // BP-63: then one of the target type's Damage_Hit_Particles at the contact.
            if (const auto target_looks = types_.find(last_seen_.at(event.target).type); target_looks != types_.end()) {
                if (const std::string* pick = hit_pick(target_looks->second.damage_hits, space::HitParticleList::damage,
                                                       event, snapshot_at);
                    pick != nullptr && !spawn(*pick, at, {}, "damage_hit", record.tick)) {
                    return false;
                }
            }
        }
        for (const tactical::AsteroidImpact& impact : record.asteroid_impacts) {
            const auto seen = last_seen_.find(impact.target);
            if (seen == last_seen_.end()) continue;
            const auto found = types_.find(seen->second.type);
            if (found == types_.end() || found->second.asteroid_hits.empty()) continue;
            const auto& look = found->second;
            tactical::CombatEvent key;
            key.tick = record.tick;
            key.shooter = impact.hit.source;
            key.target = impact.target;
            key.target_hardpoint = impact.hit.hardpoint;
            const auto seed = space::hit_event_key(key);
            const auto& particle_name = look.asteroid_hits[space::hit_particle_pick(seed, space::HitParticleList::damage, look.asteroid_hits.size())];
            auto place = seen->second.position;
            space::Vec3d direction{0.0, 0.0, 1.0};
            const auto& triangles = look.asteroid_collision.triangles;
            if (!triangles.empty()) {
                const auto& triangle = triangles[space::hit_particle_pick(seed ^ 0x31U, space::HitParticleList::damage, triangles.size())];
                const double u = std::sqrt((static_cast<double>(space::hit_particle_pick(seed ^ 0x32U, space::HitParticleList::damage, 65536)) + 0.5) / 65536.0);
                const double v = (static_cast<double>(space::hit_particle_pick(seed ^ 0x33U, space::HitParticleList::damage, 65536)) + 0.5) / 65536.0;
                space::Vec3d point{};
                space::Vec3d ab{}, ac{}, normal{};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    point[axis] = (1.0 - u) * triangle.a[axis] + u * (1.0 - v) * triangle.b[axis] + u * v * triangle.c[axis];
                    ab[axis] = triangle.b[axis] - triangle.a[axis];
                    ac[axis] = triangle.c[axis] - triangle.a[axis];
                }
                normal = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
                place = space::live_model_point(point, seen->second.position, seen->second.yaw_degrees, seen->second.roll_degrees, look.scale);
                const auto rotated = space::live_model_point(normal, {}, seen->second.yaw_degrees, seen->second.roll_degrees, 1.0);
                const double length = std::sqrt(rotated[0] * rotated[0] + rotated[1] * rotated[1] + rotated[2] * rotated[2]);
                if (length > 0.0) for (std::size_t axis = 0; axis < 3; ++axis) {
                    direction[axis] = rotated[axis] / length;
                    place[axis] += 0.1 * direction[axis];
                }
            }
            const auto* particle = particle_type(particle_name);
            const auto axes = space::shield_hit_axes(direction, particle != nullptr && particle->attached_to_collision);
            if (!spawn(particle_name, place, {vec(axes.x), vec(axes.y), vec(axes.z)}, "asteroid", record.tick)) return false;
        }
        for (const tactical::Event& event : record.events) {
            if (event.kind != tactical::EventKind::unit_destroyed && event.kind != tactical::EventKind::hardpoint_destroyed
                && event.kind != tactical::EventKind::spin_away_ended) {
                continue;
            }
            const auto seen = last_seen_.find(event.unit);
            if (seen == last_seen_.end()) continue;  // never seen by the local player
            const UnitFrame& unit = seen->second;
            const auto type = types_.find(unit.type);
            if (type == types_.end()) continue;
            const particles::Basis3 basis = yaw_basis(unit.yaw_degrees);
            // #447 SP-03, SP-08: a craft that spins away shows its spin-away explosion when it is
            // killed and is followed on; its death explosion comes when the spin ends.
            const bool spins = event.kind == tactical::EventKind::unit_destroyed
                && std::any_of(record.events.begin(), record.events.end(), [&event](const tactical::Event& other) {
                       return other.kind == tactical::EventKind::spin_away_started && other.unit == event.unit;
                   });
            if (spins) {
                if (!spawn(type->second.spin_explosion, unit.position, basis, "spin_away", record.tick)) return false;
                continue;
            }
            if (event.kind == tactical::EventKind::unit_destroyed || event.kind == tactical::EventKind::spin_away_ended) {
                if (!spawn(type->second.death_explosion, unit.position, basis, "death", record.tick)) return false;
                last_seen_.erase(seen);
                continue;
            }
            if (event.hardpoint >= type->second.hardpoint_explosions.size()) continue;
            const V local = vec(type->second.hardpoint_points[event.hardpoint]);
            const V world = add(add(scale(basis.x, local.x), scale(basis.y, local.y)), scale(basis.z, local.z));
            if (!spawn(type->second.hardpoint_explosions[event.hardpoint],
                       {unit.position[0] + world.x, unit.position[1] + world.y, unit.position[2] + world.z}, basis,
                       "hardpoint", record.tick)) {
                return false;
            }
        }
    }
    if (!draw_projectiles(previous, latest, alpha, units, camera)) return false;
    if (!advance_until(due)) return false;
    // What this frame draws: each new effect's age now is the one it is first seen at.
    for (LiveEffect& effect : effects_) {
        if (effect.drawn) continue;
        effect.drawn = true;
        if (effect.log < spawn_log_.size()) spawn_log_[effect.log].first_age = effect.age;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend

#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"
#include "frame_timer.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/snapshot_index.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>

#include "battle_effects_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_effects_detail;

namespace {
// #862: how long an ability shot's projectile ID is remembered after its launch tick (its flight
// and hit fall well inside: a shot's Max_Travel_Distance over its speed is far shorter).
constexpr std::uint64_t ability_projectile_memory = 900;
[[nodiscard]] float dot(const V a, const V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] V cross(const V a, const V b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] float length(const V a) { return std::sqrt(dot(a, a)); }
[[nodiscard]] std::optional<V> normalized(const V a) {
    const float size = length(a);
    if (!(size > 1.0e-6F) || !std::isfinite(size)) return std::nullopt;
    return scale(a, 1.0F / size);
}

// The view in the source basis: eye, unit forward, unit up, clip planes.
struct View final {
    V eye{};
    V forward{0.0F, 1.0F, 0.0F};
    V up{0.0F, 0.0F, 1.0F};
    float near_plane{1.0F};
    float far_plane{20000.0F};
};

[[nodiscard]] View view_of(const FixedCamera& camera) {
    View view;
    view.eye = vec(space::source_from_render(camera.eye));
    const V target = vec(space::source_from_render(camera.target));
    if (const auto forward = normalized(sub(target, view.eye))) view.forward = *forward;
    if (const auto up = normalized(vec(space::source_from_render(camera.up)))) view.up = *up;
    view.near_plane = camera.near_plane;
    view.far_plane = std::max(camera.far_plane, camera.near_plane + 1.0F);
    return view;
}

// BP-04/BP-07: normalised linear view depth, not the projected depth-buffer value.
[[nodiscard]] float normalized_view_z(const View& view, const V point) {
    return (dot(sub(point, view.eye), view.forward) - view.near_plane) / (view.far_plane - view.near_plane);
}

// The screen-aligned unit along `direction` (its part across the view) and the view-plane unit
// perpendicular to it; the across-view length of `direction` (a unit vector) comes back too.
struct ScreenAxes final {
    V along{};
    V side{};
    float across{};
};
[[nodiscard]] ScreenAxes screen_axes(const View& view, const V centre, const V direction) {
    const V across = sub(direction, scale(view.forward, dot(direction, view.forward)));
    ScreenAxes axes;
    axes.across = length(across);
    axes.along = normalized(across).value_or(view.up);
    if (const auto tangent = space::projectile_screen_axis(
            {view.eye.x, view.eye.y, view.eye.z}, {view.forward.x, view.forward.y, view.forward.z},
            {centre.x, centre.y, centre.z}, {direction.x, direction.y, direction.z})) {
        axes.along = vec(*tangent);
    }
    axes.side = normalized(cross(view.forward, axes.along)).value_or(cross(view.forward, view.up));
    return axes;
}

void grow_bounds(particles::VertexStream& stream, const V point) {
    if (stream.vertices.size() == 1) {
        stream.bounds_min = point;
        stream.bounds_max = point;
        return;
    }
    stream.bounds_min = {std::min(stream.bounds_min.x, point.x), std::min(stream.bounds_min.y, point.y),
                         std::min(stream.bounds_min.z, point.z)};
    stream.bounds_max = {std::max(stream.bounds_max.x, point.x), std::max(stream.bounds_max.y, point.y),
                         std::max(stream.bounds_max.z, point.z)};
}

void push(particles::VertexStream& stream, const V position, const particles::Color colour, const float u, const float v) {
    stream.vertices.push_back({position, colour, u, v});
    grow_bounds(stream, position);
}

// A projectile's pose as a live ship's (BP-60): Q24 position and R-ROT-01 yaw and pitch.
[[nodiscard]] std::optional<SpacePopulation::LivePose> live_pose(const std::size_t ship, const space::ProjectilePose& pose) {
    SpacePopulation::LivePose placed;
    placed.ship = ship;
    const std::array<double, 5> source{pose.position[0], pose.position[1], pose.position[2], pose.yaw_degrees,
                                       pose.pitch_degrees};
    std::array<sim::math::Fixed, 5> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto fixed = scene::fixed_from_binary32(static_cast<float>(source[index]));
        if (!fixed) return std::nullopt;
        values[index] = fixed.value();
    }
    placed.position = {values[0], values[1], values[2]};
    placed.yaw_degrees = values[3];
    placed.pitch_degrees = values[4];
    return placed;
}
} // namespace

void BattleEffects::plan_projectile_models(std::vector<SpacePopulation::Options::PlacedShip>& placed_ships) {
    // Between the launch slots (max / 4 up) and the session's own entities.
    constexpr sim::EntityId first_slot_entity = std::numeric_limits<sim::EntityId>::max() / 8U;
    for (const auto& [type, looks] : types_) {
        // #862: an ability shot (AB-66) may fly a model of its own.
        std::vector<const ProjectileLook*> all;
        for (const auto& [slot, look] : looks.weapons) all.push_back(&look);
        for (const auto& [slot, look] : looks.ability_weapons) all.push_back(&look);
        for (const auto& [slot, look] : looks.barrage_weapons) all.push_back(&look);
        for (const ProjectileLook* entry : all) {
            const ProjectileLook& look = *entry;
            if (look.render != Render::model || model_pools_.contains(look.projectile)) continue;
            ModelPool& pool = model_pools_[look.projectile];
            for (std::size_t index = 0; index < model_slots; ++index) {
                SpacePopulation::Options::PlacedShip ship;
                ship.object_id = look.projectile;
                ship.live_entity = first_slot_entity + model_ship_slot_.size();
                ship.projectile_slot = true;
                model_ship_slot_.emplace(placed_ships.size(), std::pair{look.projectile, index});
                pool.ships.push_back(placed_ships.size());
                placed_ships.push_back(std::move(ship));
            }
        }
    }
}

void BattleEffects::note_types(const tactical::TacticalSnapshot& previous, const tactical::TacticalSnapshot& latest) {
    spawned_projectile_types_.clear();
    for (const auto& spawn : previous.ability_spawns()) spawned_projectile_types_[spawn.id] = spawn.type;
    for (const auto& spawn : latest.ability_spawns()) spawned_projectile_types_[spawn.id] = spawn.type;
    for (const tactical::TacticalInstance& instance : previous.instances()) entity_types_[instance.entity_id] = instance.type_id;
    for (const tactical::TacticalInstance& instance : latest.instances()) entity_types_[instance.entity_id] = instance.type_id;
}

const BattleEffects::ProjectileLook* BattleEffects::look_of(const tactical::Projectile& projectile) const {
    if (const auto spawn = spawned_projectile_types_.find(projectile.id); spawn != spawned_projectile_types_.end()) {
        const auto type = types_.find(spawn->second);
        if (type == types_.end()) return nullptr;
        const auto look = type->second.weapons.find(tactical::object_weapon);
        return look == type->second.weapons.end() ? nullptr : &look->second;
    }
    std::optional<tactical::TypeId> shooter_type;
    if (const auto known = projectile_shooters_.find(projectile.id); known != projectile_shooters_.end()) {
        shooter_type = known->second;
    } else if (const auto type = entity_types_.find(projectile.shooter); type != entity_types_.end()) {
        shooter_type = type->second;
    }
    if (!shooter_type) return nullptr;
    const auto type = types_.find(*shooter_type);
    if (type == types_.end()) return nullptr;
    if (ability_projectiles_.contains(projectile.id)) {
        const auto ability = type->second.ability_weapons.find(projectile.weapon);
        if (ability != type->second.ability_weapons.end()) return &ability->second;
    }
    if (barrage_projectiles_.contains(projectile.id)) {
        const auto barrage = type->second.barrage_weapons.find(projectile.weapon);
        if (barrage != type->second.barrage_weapons.end()) return &barrage->second;
    }
    const auto weapon = type->second.weapons.find(projectile.weapon);
    return weapon == type->second.weapons.end() ? nullptr : &weapon->second;
}

void BattleEffects::observe_projectiles(const tactical::TacticalSnapshot& previous,
                                        const tactical::TacticalSnapshot& latest, const double presented_tick,
                                        ProjectileObserver observer) {
    projectile_observer_ = std::move(observer);
    const auto observe = [&](const tactical::Projectile& projectile, const double tick,
                             const tactical::TacticalSnapshot& snapshot) {
        // Historical observations need only this shot's metadata. Reuse the
        // snapshot's ordered indices, without traversing every unit per tick.
        if (const auto* shooter = space::find_instance(snapshot, projectile.shooter))
            entity_types_[projectile.shooter] = shooter->type_id;
        const auto spawns = snapshot.ability_spawns();
        const auto spawn = std::lower_bound(spawns.begin(), spawns.end(), projectile.id,
            [](const auto& item, const std::uint64_t id) { return item.id < id; });
        if (spawn != spawns.end() && spawn->id == projectile.id)
            spawned_projectile_types_[projectile.id] = spawn->type;
        auto& state = projectile_visibility_[projectile.id];
        state.owner = projectile.owner;
        if (const auto* look = look_of(projectile)) state.look = look;
        const auto fog_tick = snapshot.completed_tick();
        const bool limbo = projectile.muzzle_delay_until != 0 && fog_tick <= projectile.muzzle_delay_until;
        state.hide.advance(tick, limbo, state.look && state.look->immediate_fog, [&] {
            return !state.look || !state.look->hide_when_fogged
                || (projectile_observer_ && projectile_observer_(projectile.owner, projectile.position, fog_tick));
        });
    };
    // Retain terminal owner/look even when the latest snapshot removed the shot.
    for (const auto& projectile : previous.projectiles()) {
        if (!projectile_visibility_.contains(projectile.id))
            observe(projectile, static_cast<double>(previous.completed_tick()), previous);
    }
    for (const auto& projectile : latest.projectiles()) observe(projectile, presented_tick, latest);
}

bool BattleEffects::projectile_terminal_admitted(const tactical::CombatEvent& event) const {
    const auto state = projectile_visibility_.find(event.target);
    if (state == projectile_visibility_.end()) return false;
    // WPJ-40: decoration creation requires both model admission and the
    // projectile object's current local fog query, independent of endpoints.
    return state->second.hide.drawn() && projectile_observer_
        && projectile_observer_(state->second.owner, event.aim, event.tick);
}

float BattleEffects::projectile_model_opacity(const std::size_t ship) const {
    const auto slot = model_ship_slot_.find(ship);
    if (slot == model_ship_slot_.end()) return 1.0F;
    const auto pool = model_pools_.find(slot->second.first);
    if (pool == model_pools_.end()) return 0.0F;
    const auto id = pool->second.slots.bound(slot->second.second);
    if (!id) return 0.0F;
    const auto state = projectile_visibility_.find(*id);
    return state != projectile_visibility_.end() ? state->second.hide.opacity() : 0.0F;
}

void BattleEffects::pose_projectile_models(const tactical::TacticalSnapshot& previous,
                                           const tactical::TacticalSnapshot& latest, const double alpha,
                                           const UnitLookup&, std::vector<SpacePopulation::LivePose>& live) {
    modelled_now_.clear();
    if (released_ || model_pools_.empty()) return;
    note_types(previous, latest);
    // WPJ-38/39: per pool, projectile observer admission, ascending ID.
    std::map<std::string, std::vector<const tactical::Projectile*>> flying;
    for (const tactical::Projectile& projectile : latest.projectiles()) {
        if (projectile.muzzle_delay_until != 0 && latest.completed_tick() <= projectile.muzzle_delay_until) continue;
        const ProjectileLook* look = look_of(projectile);
        if (look == nullptr || look->render != Render::model) continue;
        const auto visibility = projectile_visibility_.find(projectile.id);
        if (visibility == projectile_visibility_.end() || !visibility->second.hide.drawn()) continue;
        flying[look->projectile].push_back(&projectile);
    }
    const auto earlier = previous.projectiles();
    for (auto& [name, pool] : model_pools_) {
        const auto found = flying.find(name);
        std::vector<std::uint64_t> ids;
        if (found != flying.end()) {
            for (const tactical::Projectile* projectile : found->second) ids.push_back(projectile->id);
        }
        const auto slots = pool.slots.bind(ids);
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!slots[index]) continue;
            const tactical::Projectile& projectile = *found->second[index];
            const auto before = std::lower_bound(earlier.begin(), earlier.end(), projectile.id,
                [](const tactical::Projectile& item, const std::uint64_t id) { return item.id < id; });
            auto pose = space::interpolate_projectile(
                before != earlier.end() && before->id == projectile.id ? &*before : nullptr, projectile, alpha);
            const auto spawns = latest.ability_spawns();
            const auto entry = std::lower_bound(spawns.begin(), spawns.end(), projectile.id,
                [](const auto& spawn, const std::uint64_t id) { return spawn.id < id; });
            if (entry != spawns.end() && entry->id == projectile.id) {
                const auto& spawn = *entry;
                pose = space::ProjectilePose{{to_float(spawn.position.x), to_float(spawn.position.y), to_float(spawn.position.z)}, 0, 0};
                if (auto frame = sim::math::to_matrix(spawn.rotation, spawn.position)) {
                    pose->yaw_degrees = space::instance_yaw_degrees(frame.value());
                    pose->pitch_degrees = space::instance_pitch_degrees(frame.value());
                }
            }
            if (!pose) continue;
            if (auto placed = live_pose(pool.ships[*slots[index]], *pose)) {
                live.push_back(std::move(*placed));
                modelled_now_.push_back(projectile.id);
                if (ability_projectiles_.contains(projectile.id) || barrage_projectiles_.contains(projectile.id)) ++ability_shots_drawn_[name];
                auto& last = model_last_posed_tick_[projectile.id];
                last = std::max(last, latest.completed_tick());
            }
        }
    }
    std::sort(modelled_now_.begin(), modelled_now_.end());
}

std::optional<SpacePopulation::LivePose> BattleEffects::projectile_model_pose_at(const std::size_t ship,
                                                                                const SnapshotAt& snapshot_at,
                                                                                const double tick) {
    const auto found = model_ship_slot_.find(ship);
    if (found == model_ship_slot_.end() || !snapshot_at) return std::nullopt;
    const auto pool = model_pools_.find(found->second.first);
    if (pool == model_pools_.end()) return std::nullopt;
    // #491: every tick this answers is a catch-up sample, always older than this frame's own, so
    // it must see the binding pose_projectile_models's bind() call started from this frame, not
    // the one it just computed for the frame's own (later) presented tick.
    const auto projectile = pool->second.slots.bound_before(found->second.second);
    if (!projectile) return std::nullopt;
    const auto visibility = projectile_visibility_.find(*projectile);
    if (visibility == projectile_visibility_.end() || !visibility->second.hide.drawn()) return std::nullopt;
    const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(tick + 1.0e-9)));
    const auto before = snapshot_at(base);
    const auto after = snapshot_at(base + 1U);
    if (!after) return std::nullopt;
    const auto find = [id = *projectile](const tactical::TacticalSnapshot& snapshot) -> const tactical::Projectile* {
        const auto list = snapshot.projectiles();
        const auto item = std::lower_bound(list.begin(), list.end(), id,
            [](const tactical::Projectile& entry, const std::uint64_t key) { return entry.id < key; });
        return item != list.end() && item->id == id ? &*item : nullptr;
    };
    const tactical::Projectile* latest = find(*after);
    if (latest == nullptr || (latest->muzzle_delay_until != 0 && after->completed_tick() <= latest->muzzle_delay_until)) return std::nullopt;
    auto pose = space::interpolate_projectile(before ? find(*before) : nullptr, *latest,
                                                    std::max(0.0, tick - static_cast<double>(base)));
    const auto spawns = after->ability_spawns();
    const auto entry = std::lower_bound(spawns.begin(), spawns.end(), latest->id,
        [](const auto& spawn, const std::uint64_t id) { return spawn.id < id; });
    if (entry != spawns.end() && entry->id == latest->id) {
        const auto& spawn = *entry;
        pose = space::ProjectilePose{{to_float(spawn.position.x), to_float(spawn.position.y), to_float(spawn.position.z)}, 0, 0};
        if (auto frame = sim::math::to_matrix(spawn.rotation, spawn.position)) {
            pose->yaw_degrees = space::instance_yaw_degrees(frame.value());
            pose->pitch_degrees = space::instance_pitch_degrees(frame.value());
        }
    }
    if (!pose) return std::nullopt;
    auto placed = live_pose(ship, *pose);
    if (placed) {
        auto& last = model_last_posed_tick_[*projectile];
        last = std::max(last, after->completed_tick());
    }
    return placed;
}

BattleEffects::Batch* BattleEffects::batch(const Render render) {
    Batch& target = render == Render::kite ? kites_ : beams_;
    if (target.resource == 0) {
        // BP-05, BP-09: additive, depth tested, no depth write, no culling (PrimAdditive's state).
        particles::EmitterRenderPlan plan;
        plan.family = render == Render::kite ? particles::RenderFamily::kites : particles::RenderFamily::billboard;
        plan.blend_selector = 1;
        plan.program = "Engine/PrimAdditive.fx";
        plan.technique = "t1";
        plan.blend = particles::Blend::additive;
        plan.phase = particles::DrawPhase::transparent;
        plan.depth_test = true;
        plan.depth_write = false;
        plan.texture = render == Render::kite ? "p_particle_master.tga" : "W_Laser_Pill.tga";
        plan.drawable = true;
        target.resource = backend_->create_emitter(plan);
        if (target.resource == 0) {
            failure_ = "laser batch " + plan.texture + ": " + backend_->failure_cause();
            return nullptr;
        }
    }
    return &target;
}

bool BattleEffects::draw_projectiles(const tactical::TacticalSnapshot& previous, const tactical::TacticalSnapshot& latest,
                                     const double alpha, const UnitLookup& units, const FixedCamera& camera) {
    FrameTimer timer(measure_preparation_ ? &projectile_prepare_ms_ : nullptr, true);
    const View view = view_of(camera);
    kites_.stream.clear();
    beams_.stream.clear();
    const auto earlier = previous.projectiles();
    std::size_t cursor = 0;
    std::map<std::uint64_t, tactical::TypeId> shooters;
    for (const tactical::Projectile& projectile : latest.projectiles()) {
        while (cursor < earlier.size() && earlier[cursor].id < projectile.id) ++cursor;
        const tactical::Projectile* before =
            cursor < earlier.size() && earlier[cursor].id == projectile.id ? &earlier[cursor] : nullptr;
        // The shooter's type, remembered while the projectile flies.
        if (const auto known = projectile_shooters_.find(projectile.id); known != projectile_shooters_.end()) {
            shooters.emplace(projectile.id, known->second);
        } else if (const auto type = entity_types_.find(projectile.shooter); type != entity_types_.end()) {
            shooters.emplace(projectile.id, type->second);
        }
        if (projectile.muzzle_delay_until != 0 && latest.completed_tick() <= projectile.muzzle_delay_until) {
            ++projectiles_hidden_;
            continue;
        }
        // HIDE_WHEN_FOGGED, approximated: shown while the local player sees its shooter or target.
        const auto visibility = projectile_visibility_.find(projectile.id);
        if (visibility == projectile_visibility_.end() || !visibility->second.hide.drawn()) {
            ++projectiles_hidden_;
            continue;
        }
        const ProjectileLook* look = look_of(projectile);
        // #456: a model projectile flies on its pool's placed ship (pose_projectile_models).
        if (look != nullptr && look->render == Render::model
            && std::binary_search(modelled_now_.begin(), modelled_now_.end(), projectile.id)) {
            ++models_drawn_;
            continue;
        }
        if (look == nullptr || look->render == Render::none || look->render == Render::model) {
            ++not_drawn_[look == nullptr ? std::string("<unknown weapon>") : look->projectile];
            continue;
        }
        const V now = vec(projectile.position);
        const V centre = before != nullptr ? add(vec(before->position), scale(sub(now, vec(before->position)), static_cast<float>(alpha)))
                                           : now;
        const auto direction = normalized(vec(projectile.step));
        if (!direction) continue;
        // BP-02: the object axis B1 -> B2 is model +Y, and the model nose is -Y, so B1 -> B2 runs
        // against the flight: B1 is the front end.
        const V backward = scale(*direction, -1.0F);
        const ScreenAxes axes = screen_axes(view, centre, backward);
        Batch* target = batch(look->render);
        if (target == nullptr) return false;
        particles::VertexStream& stream = target->stream;
        const auto base = static_cast<std::uint32_t>(stream.vertices.size());
        if (look->render == Render::kite) {
            // BP-02 to BP-04: a diamond about the projectile, its long point towards B2, i.e. trailing
            // the flight; the glow cell centres on the projectile, so the thick end leads.
            const float width = presentation_constants::laser_width(look->width, laser_scales_.kite, normalized_view_z(view, centre));
            max_kite_width_ = std::max(max_kite_width_, width);
            if (!first_kite_depth_) first_kite_depth_ = std::array<float, 4>{
                dot(sub(centre, view.eye), view.forward), normalized_view_z(view, centre),
                laser_scales_.kite * normalized_view_z(view, centre) + 1.0F, width};
            const float reach = std::max(1.0F, look->length * axes.across);
            const float u0 = look->slot[0] / 8.0F;
            const float v0 = look->slot[1] / 8.0F;
            const particles::Color colour{look->colour[0], look->colour[1], look->colour[2],
                look->colour[3] * visibility->second.hide.opacity()};
            const V tail = add(centre, scale(axes.along, width * reach));
            const V head = sub(centre, scale(axes.along, width));
            push(stream, add(centre, scale(axes.side, width)), colour, u0, v0 + 0.125F);
            push(stream, tail, colour, u0, v0);
            push(stream, sub(centre, scale(axes.side, width)), colour, u0 + 0.125F, v0);
            push(stream, head, colour, u0 + 0.125F, v0 + 0.125F);
            if (collect_axis_diagnostics_) {
                // BP-68: measure the drawn endpoints against an independently projected flight step.
                // This report uses perspective point division, rather than the axis construction.
                const auto project = [&](const V point) -> std::optional<V> {
                    const V ray = sub(point, view.eye);
                    const float depth = dot(ray, view.forward);
                    if (!(depth > view.near_plane)) return std::nullopt;
                    return scale(ray, 1.0F / depth);
                };
                const auto drawn_head = project(head);
                const auto drawn_tail = project(tail);
                const auto start = project(centre);
                const auto end = project(add(centre, vec(projectile.step)));
                if (drawn_head && drawn_tail && start && end) {
                    const auto drawn = normalized(sub(*drawn_head, *drawn_tail));
                    const auto travel = normalized(sub(*end, *start));
                    if (drawn && travel) {
                        ++kite_axis_samples_;
                        kite_axis_max_sine_ = std::max(kite_axis_max_sine_, static_cast<double>(length(cross(*drawn, *travel))));
                        if (dot(*drawn, *travel) < 0.0F) {
                            ++kite_axis_reversed_;
                            ++kites_head_trailing_;
                        } else {
                            ++kites_head_leading_;
                        }
                    }
                }
            }
            for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U}) stream.indices.push_back(base + index);
        } else {
            // BP-06 to BP-09: the pill from B1 (front) to B2 with pointed ends, white.
            const V half = scale(backward, 0.5F * look->length);
            const V b1 = sub(centre, half);
            const V b2 = add(centre, half);
            const float width = presentation_constants::laser_width(look->width, laser_scales_.beam, normalized_view_z(view, centre));
            max_beam_width_ = std::max(max_beam_width_, width);
            if (!first_beam_depth_) first_beam_depth_ = std::array<float, 4>{
                dot(sub(centre, view.eye), view.forward), normalized_view_z(view, centre),
                laser_scales_.beam * normalized_view_z(view, centre) + 1.0F, width};
            const V a = scale(axes.along, width);
            const V p = scale(axes.side, width);
            const float u0 = look->slot[0] / 4.0F;
            const float v0 = look->slot[1] / 4.0F;
            const particles::Color white{1.0F, 1.0F, 1.0F, visibility->second.hide.opacity()};
            push(stream, sub(b1, a), white, u0 + 0.25F, v0);
            push(stream, add(b1, p), white, u0 + 0.125F, v0);
            push(stream, sub(b1, p), white, u0 + 0.25F, v0 + 0.125F);
            push(stream, add(b2, p), white, u0, v0 + 0.125F);
            push(stream, sub(b2, p), white, u0 + 0.125F, v0 + 0.25F);
            push(stream, add(b2, a), white, u0, v0 + 0.25F);
            for (const std::uint32_t index : {0U, 1U, 2U, 1U, 3U, 2U, 2U, 3U, 4U, 3U, 5U, 4U}) {
                stream.indices.push_back(base + index);
            }
        }
        ++stream.quads;
        ++projectiles_drawn_;
        if (ability_projectiles_.contains(projectile.id) || barrage_projectiles_.contains(projectile.id)) ++ability_shots_drawn_[look->projectile];
    }
    projectile_shooters_ = std::move(shooters);
    std::set<std::pair<std::uint64_t, sim::EntityId>> active_weaken;
    for (const auto& spawn_state : latest.ability_spawns()) {
        const auto particle = weaken_particles_.find(spawn_state.type);
        if (particle == weaken_particles_.end() || particle->second.empty()) continue;
        for (const auto& recipient : spawn_state.recipients) {
            const auto pose = units(recipient.target);
            if (!pose) continue;
            const auto key = std::pair{spawn_state.id, recipient.target};
            active_weaken.insert(key);
            auto active = weaken_effects_.find(key);
            if (active == weaken_effects_.end()) {
                const auto count = effects_.size();
                if (!spawn(particle->second, pose->position, {}, "weaken_status", latest.completed_tick())) return false;
                if (effects_.size() > count) {
                    effects_.back().lifetime = std::numeric_limits<std::uint32_t>::max();
                    active = weaken_effects_.emplace(key, effects_.back().handle).first;
                }
            }
            if (active != weaken_effects_.end()) {
                particles::EmitterFrame frame; frame.origin = vec(pose->position);
                if (!registry_->set_frame(active->second, frame)) return false;
            }
        }
    }
    for (auto active = weaken_effects_.begin(); active != weaken_effects_.end();) {
        if (active_weaken.contains(active->first)) { ++active; continue; }
        for (auto& effect : effects_) if (effect.handle == active->second) effect.lifetime = effect.age;
        active = weaken_effects_.erase(active);
    }
    std::set<sim::EntityId> active_energy;
    std::set<sim::EntityId> active_tractors;
    for (auto& look : hero_beams_) {
        look.batch.stream.clear();
        look.sparks.stream.clear();
    }
    // WHE-26/27: tracked endpoints follow the live poses; releasing the slot removes the beam.
    for (const auto& instance : latest.instances()) {
        const auto source = units(instance.entity_id);
        if (!source) continue;
        const auto type = types_.find(instance.type_id);
        if (type == types_.end()) continue;
        for (const auto& status : instance.abilities) {
            if (!status.active || (status.kind != tactical::AbilityKind::energy_weapon && status.kind != tactical::AbilityKind::tractor_beam)) continue;
            const auto target = units(status.target);
            if (!target) continue;
            const auto beam = status.kind == tactical::AbilityKind::energy_weapon ? 0U : 1U;
            const auto origin = space::live_model_point(type->second.beam_origins[beam], source->position,
                source->yaw_degrees, source->roll_degrees, 1.0);
            auto endpoint = target->position;
            if (const auto target_type = types_.find(target->type); target_type != types_.end()
                && status.target_hardpoint < target_type->second.hardpoint_points.size()) {
                const auto point = vec(target_type->second.hardpoint_points[status.target_hardpoint]);
                endpoint = space::live_model_point({point.x, point.y, point.z}, target->position,
                    target->yaw_degrees, target->roll_degrees, 1.0);
            }
            if (beam == 0 && !type->second.energy_owner_particle.empty()) {
                active_energy.insert(instance.entity_id);
                auto active = energy_owner_effects_.find(instance.entity_id);
                if (active == energy_owner_effects_.end()) {
                    const auto before = effects_.size();
                    const particles::Basis3 basis{};
                    if (!spawn(type->second.energy_owner_particle, origin, basis, "energy_owner", latest.completed_tick())) return false;
                    if (effects_.size() > before) {
                        effects_.back().lifetime = std::numeric_limits<std::uint32_t>::max();
                        active = energy_owner_effects_.emplace(instance.entity_id, effects_.back().handle).first;
                    }
                }
                if (active != energy_owner_effects_.end()) {
                    particles::EmitterFrame frame; frame.origin = vec(origin);
                    if (!registry_->set_frame(active->second, frame)) return false;
                }
            }
            auto& look = hero_beams_[beam];
            if (look.texture.empty() || look.width <= 0) continue;
            if (look.batch.resource == 0) {
                particles::EmitterRenderPlan plan;
                plan.family = particles::RenderFamily::billboard;
                plan.blend_selector = 1; plan.program = "Engine/PrimAdditive.fx"; plan.technique = "t1";
                plan.blend = particles::Blend::additive; plan.phase = particles::DrawPhase::transparent;
                plan.depth_test = true; plan.depth_write = false; plan.drawable = true; plan.texture = look.texture;
                look.batch.resource = backend_->create_emitter(plan);
                if (look.batch.resource == 0) { failure_ = "hero beam " + look.texture + ": " + backend_->failure_cause(); return false; }
            }
            const V a = vec(origin), b = vec(endpoint);
            const auto direction = normalized(sub(b, a));
            if (!direction) continue;
            const auto axes = screen_axes(view, scale(add(a, b), 0.5F), *direction);
            // TBF-01/02: full world width; texture U crosses the line, V follows it.
            const V side = scale(axes.side, look.width * 0.5F);
            auto& stream = look.batch.stream;
            const auto base = static_cast<std::uint32_t>(stream.vertices.size());
            push(stream, add(a, side), look.colour, 0.0F, 1.0F);
            push(stream, sub(a, side), look.colour, 1.0F, 1.0F);
            push(stream, add(b, side), look.colour, 0.0F, 0.0F);
            push(stream, sub(b, side), look.colour, 1.0F, 0.0F);
            std::copy_n(stream.vertices.begin() + base, look.last_quad.size(), look.last_quad.begin());
            for (const auto index : {0U, 1U, 2U, 1U, 3U, 2U}) stream.indices.push_back(base + index);
            ++stream.quads;
            ++(beam == 0 ? energy_beams_drawn_ : tractor_beams_drawn_);
            if (beam == 1) {
                active_tractors.insert(instance.entity_id);
                auto [birth, inserted] = look.births.try_emplace(instance.entity_id,
                    std::pair{status.target, latest.completed_tick()});
                if (birth->second.first != status.target) {
                    birth->second = {status.target, latest.completed_tick()};
                    inserted = true;
                }
                const double elapsed = latest.completed_tick() >= birth->second.second
                    ? static_cast<double>(latest.completed_tick() - birth->second.second) : 0.0;
                // TBF-02/03: simple-line UVs do not scroll. Endpoint refresh and
                // animation service advance four small highlights toward the source.
                const double advances = 1.0 + elapsed * (look.frames != 0 ? 2.0 : 1.0);
                if (look.sparks.resource == 0) {
                    particles::EmitterRenderPlan plan;
                    plan.family = particles::RenderFamily::billboard;
                    plan.blend_selector = 1; plan.program = "Engine/PrimAdditive.fx"; plan.technique = "t1";
                    plan.blend = particles::Blend::additive; plan.phase = particles::DrawPhase::transparent;
                    plan.depth_test = true; plan.depth_write = false; plan.drawable = true;
                    plan.texture = "w_galaxy_dot.tga";
                    look.sparks.resource = backend_->create_emitter(plan);
                    if (look.sparks.resource == 0) { failure_ = "tractor highlights: " + backend_->failure_cause(); return false; }
                }
                const auto byte_tint = [](const float value) {
                    return static_cast<float>(static_cast<unsigned>(value * 0.7F * 255.0F)) / 255.0F;
                };
                const particles::Color tint{byte_tint(look.colour.x), byte_tint(look.colour.y),
                    byte_tint(look.colour.z), byte_tint(look.colour.w)};
                const V across = scale(axes.side, 1.75F);
                const V along = scale(*direction, 1.75F);
                for (const double start : {0.45, 0.70, 0.50, 0.89}) {
                    double fraction = std::fmod(start - advances / 50.0, 1.0);
                    if (fraction < 0.0) fraction += 1.0;
                    const V centre = add(a, scale(sub(b, a), static_cast<float>(fraction)));
                    auto& highlights = look.sparks.stream;
                    const auto first = static_cast<std::uint32_t>(highlights.vertices.size());
                    push(highlights, add(sub(centre, along), across), tint, 0.0F, 1.0F);
                    push(highlights, sub(sub(centre, along), across), tint, 1.0F, 1.0F);
                    push(highlights, add(add(centre, along), across), tint, 0.0F, 0.0F);
                    push(highlights, sub(add(centre, along), across), tint, 1.0F, 0.0F);
                    for (const auto index : {0U, 1U, 2U, 1U, 3U, 2U}) highlights.indices.push_back(first + index);
                    ++highlights.quads;
                    ++look.sparks_drawn;
                    if (start == 0.45) {
                        if (!inserted && look.last_spark_fraction != static_cast<float>(fraction)) ++look.moving_samples;
                        look.last_spark_fraction = static_cast<float>(fraction);
                    }
                }
            }
        }
    }
    for (auto active = energy_owner_effects_.begin(); active != energy_owner_effects_.end();) {
        if (active_energy.contains(active->first)) { ++active; continue; }
        for (auto& effect : effects_) if (effect.handle == active->second) effect.lifetime = effect.age;
        active = energy_owner_effects_.erase(active);
    }
    std::erase_if(hero_beams_[1].births, [&](const auto& entry) { return !active_tractors.contains(entry.first); });
    for (auto& look : hero_beams_) {
        if (look.batch.resource != 0) backend_->update_emitter(look.batch.resource, look.batch.stream);
        if (look.sparks.resource != 0) backend_->update_emitter(look.sparks.resource, look.sparks.stream);
    }
    max_kites_ = std::max<std::uint64_t>(max_kites_, kites_.stream.quads);
    max_beams_ = std::max<std::uint64_t>(max_beams_, beams_.stream.quads);
    timer.finish();
    for (Batch* target : {&kites_, &beams_}) {
        if (target->resource != 0) backend_->update_emitter(target->resource, target->stream);
    }
    return true;
}

void BattleEffects::note_ability_shots(const std::span<const platform::LiveTickEvents> reached,
                                       const tactical::TacticalSnapshot& latest, const SnapshotAt& snapshot_at) {
    for (const platform::LiveTickEvents& record : reached) {
        if (ability_noted_through_ && record.tick <= *ability_noted_through_) continue;
        ability_noted_through_ = record.tick;
        // #862 (AB-66): a shot a weapon fired as its ability shot is the newest projectile of that
        // shooter and weapon in its tick's snapshot (the tick launches it in event order).
        const auto launched = snapshot_at ? snapshot_at(record.tick) : nullptr;
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::weapon_fired
                || (event.outcome & (tactical::fired_ability_shot | tactical::fired_barrage_shot)) == 0U) {
                continue;
            }
            const auto list = launched ? launched->projectiles() : latest.projectiles();
            const tactical::Projectile* shot = nullptr;
            for (const tactical::Projectile& projectile : list) {
                if (projectile.shooter != event.shooter || projectile.weapon != event.weapon
                    || ability_projectiles_.contains(projectile.id) || barrage_projectiles_.contains(projectile.id)) {
                    continue;
                }
                if (shot == nullptr || projectile.id > shot->id) shot = &projectile;
            }
            if (shot == nullptr) continue;
            auto& selected = (event.outcome & tactical::fired_ability_shot) != 0U
                ? ability_projectiles_ : barrage_projectiles_;
            selected.emplace(shot->id, record.tick);
            const ProjectileLook* look = look_of(*shot);
            ++ability_shots_fired_[look == nullptr ? std::string("<unknown weapon>") : look->projectile];
        }
    }
    std::erase_if(ability_projectiles_, [&](const auto& entry) {
        return entry.second + ability_projectile_memory < latest.completed_tick();
    });
    std::erase_if(barrage_projectiles_, [&](const auto& entry) {
        return entry.second + ability_projectile_memory < latest.completed_tick();
    });
}

} // namespace eawr::presentation::godot_backend

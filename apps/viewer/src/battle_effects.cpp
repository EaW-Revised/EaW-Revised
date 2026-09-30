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

namespace eawr::presentation::godot_backend {
namespace {

namespace tactical = sim::tactical;

// Every live particle effect draws from its own capacity; the batch keeps at most this many
// effects alive at once and reports the rest as dropped.
constexpr std::size_t effect_capacity = 4096;
constexpr std::size_t max_live_effects = 256;
// A detached effect that never reports finished is released after this many further samples.
constexpr std::uint32_t drain_limit_frames = 300;
// The report's spawn log keeps this many rows.
constexpr std::size_t spawn_log_limit = 8192;
// The report keeps this many placed shield hits.
constexpr std::size_t shield_sample_limit = 64;
// GAMECONSTANTS.XML Laser_Beam_Z_Scale_Factor and Laser_Kite_Z_Scale_Factor (BP-04, BP-08).
constexpr float beam_z_scale = 8.0F;
constexpr float kite_z_scale = 1.2F;

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] std::vector<float> numbers(std::string_view text) {
    std::vector<float> result;
    std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        char* end = nullptr;
        const float value = std::strtof(token.c_str(), &end);
        if (end == token.c_str() + token.size() && std::isfinite(value)) result.push_back(value);
        token.clear();
    };
    for (const char character : text) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character))) flush();
        else token.push_back(character);
    }
    flush();
    return result;
}

[[nodiscard]] std::string json(const std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            result += ' ';
        } else {
            result += character;
        }
    }
    return result + "\"";
}

[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name) {
    const data::EffectiveValue* value = object.value(name);
    return value == nullptr ? std::string{} : trim(value->value.raw_text);
}

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

using V = particles::Vec3;
[[nodiscard]] V add(const V a, const V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] V sub(const V a, const V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] V scale(const V a, const float s) { return {a.x * s, a.y * s, a.z * s}; }
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
[[nodiscard]] V vec(const sim::math::Vec3& value) { return {to_float(value.x), to_float(value.y), to_float(value.z)}; }
[[nodiscard]] V vec(const assets::Vec3f& value) { return {value.x, value.y, value.z}; }

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

// The debug build's normalised view depth (BP-17): (view depth - near) / (far - near).
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
[[nodiscard]] ScreenAxes screen_axes(const View& view, const V direction) {
    const V across = sub(direction, scale(view.forward, dot(direction, view.forward)));
    ScreenAxes axes;
    axes.across = length(across);
    axes.along = normalized(across).value_or(view.up);
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

[[nodiscard]] V vec(const space::Vec3d& value) {
    return {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
}
[[nodiscard]] space::Vec3d dvec(const V value) { return {value.x, value.y, value.z}; }

[[nodiscard]] bool truthy(const std::string& text) {
    const std::string value = lower(text);
    return value == "1" || value == "yes" || value == "true";
}

// Every triangle of `mesh`'s submeshes, its corners placed by `place`.
template <typename Place>
void append_triangles(const assets::Mesh& mesh, const Place& place, std::vector<space::ShieldTriangle>& result) {
    for (const assets::Submesh& submesh : mesh.submeshes) {
        for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
            const std::array<std::uint16_t, 3> corner{submesh.indices[index], submesh.indices[index + 1],
                                                      submesh.indices[index + 2]};
            if (corner[0] >= submesh.vertices.size() || corner[1] >= submesh.vertices.size()
                || corner[2] >= submesh.vertices.size()) {
                continue;
            }
            result.push_back({place(submesh.vertices[corner[0]].position), place(submesh.vertices[corner[1]].position),
                              place(submesh.vertices[corner[2]].position)});
        }
    }
}

// Whether the model has a sub-object named SHIELD (BP-17, the debug build; the lookup ignores
// case).
[[nodiscard]] bool has_shield_mesh(const assets::Model& model) {
    return std::any_of(model.meshes.begin(), model.meshes.end(),
                       [](const assets::Mesh& mesh) { return lower(mesh.name) == "shield"; });
}

// What a projectile of a shielded unit can hit (BP-19): the collidable meshes and, while the
// shield is up, the SHIELD mesh, as model-space triangles in the bind pose, each mesh placed by
// its bone's bind frame.
[[nodiscard]] std::vector<space::ShieldTriangle> collision_triangles(const assets::Model& model) {
    std::vector<space::ShieldTriangle> result;
    const auto frames = units::bind_frames(model);
    for (const assets::Mesh& mesh : model.meshes) {
        if (!mesh.collidable && lower(mesh.name) != "shield") continue;
        std::array<std::array<double, 4>, 3> frame{{{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 1.0, 0.0}}};
        if (frames && mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < frames.value().size()) {
            const sim::math::Mat3x4& bone = frames.value()[static_cast<std::size_t>(mesh.bone)];
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) frame[row][column] = to_float(bone.rows[row][column]);
            }
        }
        const auto place = [&frame](const assets::Vec3f& point) {
            space::Vec3d placed{};
            for (std::size_t row = 0; row < 3; ++row) {
                placed[row] = frame[row][0] * point.x + frame[row][1] * point.y + frame[row][2] * point.z + frame[row][3];
            }
            return placed;
        };
        append_triangles(mesh, place, result);
    }
    return result;
}

// BP-63: every entry of a type-list tag: each of its tags (in XML order), split on commas and
// white space.
[[nodiscard]] std::vector<std::string> type_list(const data::EffectiveObject& object, const std::string_view name) {
    std::vector<std::string> result;
    for (const data::EffectiveValue& value : object.values) {
        if (lower(value.value.name) != lower(name)) continue;
        std::string token;
        const auto flush = [&] {
            if (!token.empty()) result.push_back(token);
            token.clear();
        };
        for (const char character : value.value.raw_text) {
            if (character == ',' || std::isspace(static_cast<unsigned char>(character))) flush();
            else token.push_back(character);
        }
        flush();
    }
    return result;
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

[[nodiscard]] particles::Basis3 yaw_basis(const double yaw_degrees) {
    const double radians = yaw_degrees * 3.14159265358979323846 / 180.0;
    const auto c = static_cast<float>(std::cos(radians));
    const auto s = static_cast<float>(std::sin(radians));
    return {{c, s, 0.0F}, {-s, c, 0.0F}, {0.0F, 0.0F, 1.0F}};
}

} // namespace

BattleEffects::BattleEffects(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog)
    : host_(&host), filesystem_(&filesystem), catalog_(&catalog),
      backend_(std::make_unique<GodotParticleBackend>(
          host, [this](const std::string_view name) { return resolve_texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {}

BattleEffects::~BattleEffects() { release(); }

const assets::Texture* BattleEffects::resolve_texture(const std::string_view authored) {
    const std::size_t slash = authored.find_last_of("/\\");
    const std::string name = lower(slash == std::string_view::npos ? authored : authored.substr(slash + 1));
    auto found = textures_.find(name);
    if (found == textures_.end()) {
        std::optional<assets::Texture> decoded;
        std::string stem = name;
        for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
            if (stem.ends_with(suffix)) stem.resize(stem.size() - suffix.size());
        }
        for (const std::string& candidate : {"data/art/textures/" + name, "data/art/textures/" + stem + ".tga",
                                             "data/art/textures/" + stem + ".dds"}) {
            if (!filesystem_->stat(candidate)) continue;
            if (auto loaded = assets::load_texture(*filesystem_, candidate)) {
                decoded = std::move(loaded.value());
                break;
            }
        }
        found = textures_.emplace(name, std::move(decoded)).first;
    }
    return found->second ? &*found->second : nullptr;
}

void BattleEffects::prepare(const units::UnitTables& tables, const tactical::CombatTable& combat) {
    const auto look_of = [&](const std::string& projectile) {
        ProjectileLook look;
        look.projectile = projectile;
        auto object = catalog_->resolve(projectile);
        if (!object) {
            unresolved_.push_back("projectile " + projectile + ": " + core::format_diagnostic(object.error()));
            return look;
        }
        const data::EffectiveObject& value = object.value();
        // BP-01: Projectile_Custom_Render 1 is the laser beam, 2 the laser kite; anything else
        // draws Space_Model_Name.
        const std::string custom = tag(value, "Projectile_Custom_Render");
        look.render = custom == "1" ? Render::beam : custom == "2" ? Render::kite
            : tag(value, "Space_Model_Name").empty() ? Render::none : Render::model;
        const auto width = numbers(tag(value, "Projectile_Width"));
        const auto length_value = numbers(tag(value, "Projectile_Length"));
        const auto slot = numbers(tag(value, "Projectile_Texture_Slot"));
        const auto colour = numbers(tag(value, "Projectile_Laser_Color"));
        if (!width.empty()) look.width = width.front();
        if (!length_value.empty()) look.length = length_value.front();
        if (slot.size() >= 2) look.slot = {slot[0], slot[1]};
        if (colour.size() >= 4) look.colour = {colour[0] / 255.0F, colour[1] / 255.0F, colour[2] / 255.0F, colour[3] / 255.0F};
        look.detonation = tag(value, "Projectile_Object_Detonation_Particle");
        look.armor_reduced = tag(value, "Projectile_Object_Armor_Reduced_Detonation_Particle");
        look.shield_absorbed = tag(value, "Projectile_Absorbed_By_Shields_Particle");
        return look;
    };
    for (const units::UnitType& type : tables.units) {
        TypeLooks looks;
        const tactical::CombatProfile* profile = combat.find(skirmish::type_id(type.id));
        // The shot's frame step (Max_Speed per frame), for where its flight meets a shield (BP-19).
        const auto step_of = [profile](const std::uint32_t slot) {
            if (profile == nullptr) return 0.0;
            for (const tactical::WeaponProfile& weapon : profile->weapons) {
                if (weapon.hardpoint == slot && weapon.shot) return static_cast<double>(to_float(weapon.shot->speed));
            }
            return 0.0;
        };
        for (std::size_t index = 0; index < type.hardpoints.size(); ++index) {
            const units::Hardpoint& hardpoint = type.hardpoints[index];
            if (hardpoint.weapon && !hardpoint.weapon->projectile.empty()) {
                ProjectileLook look = look_of(hardpoint.weapon->projectile);
                look.step_length = step_of(static_cast<std::uint32_t>(index));
                looks.weapons.emplace(static_cast<std::uint32_t>(index), std::move(look));
            }
            auto object = catalog_->resolve(hardpoint.id);
            looks.hardpoint_explosions.push_back(object ? tag(object.value(), "Death_Explosion_Particles") : std::string{});
        }
        if (type.weapon && !type.weapon->projectile.empty()) {
            ProjectileLook look = look_of(type.weapon->projectile);
            look.step_length = step_of(tactical::object_weapon);
            looks.weapons.emplace(tactical::object_weapon, std::move(look));
        }
        if (auto object = catalog_->resolve(type.id)) {
            // Death_Explosions may list alternatives; FoC's pick among them is not modelled, the
            // first is shown (fidelity list).
            const std::string explosions = tag(object.value(), "Death_Explosions");
            looks.death_explosion = trim(std::string_view(explosions).substr(0, explosions.find(',')));
            // #447 SP-03: the explosion a craft that spins away shows when it is killed; the
            // first of a list, as for Death_Explosions.
            const std::string spin = tag(object.value(), "Spin_Away_On_Death_Explosion");
            looks.spin_explosion = trim(std::string_view(spin).substr(0, spin.find(',')));
            looks.damage_hits = type_list(object.value(), "Damage_Hit_Particles");
            looks.shield_hits = type_list(object.value(), "Shield_Hit_Particles");
        }
        const tactical::TypeId id = skirmish::type_id(type.id);
        if (type.scale_factor) looks.scale = to_float(*type.scale_factor);
        // BP-17, BP-19: only a shield takes a hit whole, so only shielded types need their
        // collision meshes and whether they have a SHIELD sub-object.
        if (type.shielded && !type.model_path.empty()) {
            if (auto model = assets::load_model(*filesystem_, type.model_path)) {
                looks.shield_mesh = has_shield_mesh(model.value());
                looks.collision = space::make_shield_collision_mesh(collision_triangles(model.value()));
            } else {
                unresolved_.push_back("model " + type.model_path + ": " + core::format_diagnostic(model.error()));
            }
            shield_meshes_[type.id] = {looks.shield_mesh, looks.collision.triangles.size()};
        }
        looks.hardpoint_points.assign(type.hardpoints.size(), sim::math::Vec3{});
        if (profile != nullptr) {
            for (const tactical::TargetHardpoint& point : profile->hardpoints) {
                if (point.hardpoint < looks.hardpoint_points.size()) looks.hardpoint_points[point.hardpoint] = point.position;
            }
        }
        types_.emplace(id, std::move(looks));
    }
}

void BattleEffects::plan_projectile_models(std::vector<SpacePopulation::Options::PlacedShip>& placed_ships) {
    // Between the launch slots (max / 4 up) and the session's own entities.
    constexpr sim::EntityId first_slot_entity = std::numeric_limits<sim::EntityId>::max() / 8U;
    for (const auto& [type, looks] : types_) {
        for (const auto& [slot, look] : looks.weapons) {
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
    for (const tactical::TacticalInstance& instance : previous.instances()) entity_types_[instance.entity_id] = instance.type_id;
    for (const tactical::TacticalInstance& instance : latest.instances()) entity_types_[instance.entity_id] = instance.type_id;
}

const BattleEffects::ProjectileLook* BattleEffects::look_of(const tactical::Projectile& projectile) const {
    std::optional<tactical::TypeId> shooter_type;
    if (const auto known = projectile_shooters_.find(projectile.id); known != projectile_shooters_.end()) {
        shooter_type = known->second;
    } else if (const auto type = entity_types_.find(projectile.shooter); type != entity_types_.end()) {
        shooter_type = type->second;
    }
    if (!shooter_type) return nullptr;
    const auto type = types_.find(*shooter_type);
    if (type == types_.end()) return nullptr;
    const auto weapon = type->second.weapons.find(projectile.weapon);
    return weapon == type->second.weapons.end() ? nullptr : &weapon->second;
}

void BattleEffects::pose_projectile_models(const tactical::TacticalSnapshot& previous,
                                           const tactical::TacticalSnapshot& latest, const double alpha,
                                           const UnitLookup& units, std::vector<SpacePopulation::LivePose>& live) {
    modelled_now_.clear();
    if (released_ || model_pools_.empty()) return;
    note_types(previous, latest);
    // Per pool, the model projectiles the local player sees (BP-09), ascending ID.
    std::map<std::string, std::vector<const tactical::Projectile*>> flying;
    for (const tactical::Projectile& projectile : latest.projectiles()) {
        const ProjectileLook* look = look_of(projectile);
        if (look == nullptr || look->render != Render::model) continue;
        if (!units(projectile.shooter) && !units(projectile.target)) continue;
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
            const auto pose = space::interpolate_projectile(
                before != earlier.end() && before->id == projectile.id ? &*before : nullptr, projectile, alpha);
            if (!pose) continue;
            if (auto placed = live_pose(pool.ships[*slots[index]], *pose)) {
                live.push_back(std::move(*placed));
                modelled_now_.push_back(projectile.id);
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
    if (latest == nullptr) return std::nullopt;
    const auto pose = space::interpolate_projectile(before ? find(*before) : nullptr, *latest,
                                                    std::max(0.0, tick - static_cast<double>(base)));
    if (!pose) return std::nullopt;
    auto placed = live_pose(ship, *pose);
    if (placed) {
        auto& last = model_last_posed_tick_[*projectile];
        last = std::max(last, after->completed_tick());
    }
    return placed;
}

const std::string* BattleEffects::hit_pick(const std::vector<std::string>& list, const space::HitParticleList kind,
                                           const tactical::CombatEvent& event, const SnapshotAt& snapshot_at) {
    if (list.empty()) return nullptr;
    std::optional<std::uint64_t> projectile;
    if (snapshot_at && event.tick > 0) {
        if (const auto before = snapshot_at(event.tick - 1U)) projectile = space::hit_projectile(before->projectiles(), event);
    }
    ++(projectile ? hit_picks_by_projectile_ : hit_picks_by_event_);
    return &list[space::hit_particle_pick(projectile.value_or(space::hit_event_key(event)), kind, list.size())];
}

const BattleEffects::ParticleType* BattleEffects::particle_type(const std::string& name) {
    if (name.empty()) return nullptr;
    auto found = particle_types_.find(name);
    if (found != particle_types_.end()) return &found->second;
    ParticleType type;
    auto object = catalog_->resolve(name);
    if (!object) {
        type.cause = "not in the XML catalog";
    } else {
        const std::string model = tag(object.value(), "Space_Model_Name");
        const auto lifetime = numbers(tag(object.value(), "Particle_Lifetime_Frames"));
        type.lifetime_frames = lifetime.empty() ? 0U : static_cast<std::uint32_t>(std::max(0.0F, lifetime.front()));
        type.attached_to_collision = truthy(tag(object.value(), "Particle_Attach_To_Collision"));
        if (model.empty()) {
            type.cause = "no Space_Model_Name";
        } else {
            type.model_path = "data/art/models/" + lower(model);
            auto bytes = filesystem_->open(type.model_path);
            if (!bytes) {
                type.cause = core::format_diagnostic(bytes.error());
            } else if (auto system = particles::load_alo(bytes.value(), type.model_path); !system) {
                type.cause = core::format_diagnostic(system.error());
            } else if (system.value().emitters.empty()) {
                type.cause = "particle system declares no emitters";
            } else {
                type.system = std::move(system).value();
            }
        }
    }
    return &particle_types_.emplace(name, std::move(type)).first->second;
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
        // HIDE_WHEN_FOGGED, approximated: shown while the local player sees its shooter or target.
        if (!units(projectile.shooter) && !units(projectile.target)) {
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
        const ScreenAxes axes = screen_axes(view, backward);
        Batch* target = batch(look->render);
        if (target == nullptr) return false;
        particles::VertexStream& stream = target->stream;
        const auto base = static_cast<std::uint32_t>(stream.vertices.size());
        if (look->render == Render::kite) {
            // BP-02 to BP-04: a diamond about the projectile, its long point towards B2, i.e. trailing
            // the flight; the glow cell centres on the projectile, so the thick end leads.
            const float width = look->width * (kite_z_scale * normalized_view_z(view, centre) + 1.0F);
            const float reach = std::max(1.0F, look->length * axes.across);
            const float u0 = look->slot[0] / 8.0F;
            const float v0 = look->slot[1] / 8.0F;
            const particles::Color colour{look->colour[0], look->colour[1], look->colour[2], look->colour[3]};
            const V tail = add(centre, scale(axes.along, width * reach));
            const V head = sub(centre, scale(axes.along, width));
            push(stream, add(centre, scale(axes.side, width)), colour, u0, v0 + 0.125F);
            push(stream, tail, colour, u0, v0);
            push(stream, sub(centre, scale(axes.side, width)), colour, u0 + 0.125F, v0);
            push(stream, head, colour, u0 + 0.125F, v0 + 0.125F);
            if (dot(sub(head, tail), *direction) > 0.0F) ++kites_head_leading_;
            else ++kites_head_trailing_;
            for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U}) stream.indices.push_back(base + index);
        } else {
            // BP-06 to BP-09: the pill from B1 (front) to B2 with pointed ends, white.
            const V half = scale(backward, 0.5F * look->length);
            const V b1 = sub(centre, half);
            const V b2 = add(centre, half);
            const float width = look->width * (beam_z_scale * normalized_view_z(view, centre) + 1.0F);
            const V a = scale(axes.along, width);
            const V p = scale(axes.side, width);
            const float u0 = look->slot[0] / 4.0F;
            const float v0 = look->slot[1] / 4.0F;
            const particles::Color white{1.0F, 1.0F, 1.0F, 1.0F};
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
    }
    projectile_shooters_ = std::move(shooters);
    max_kites_ = std::max<std::uint64_t>(max_kites_, kites_.stream.quads);
    max_beams_ = std::max<std::uint64_t>(max_beams_, beams_.stream.quads);
    for (Batch* target : {&kites_, &beams_}) {
        if (target->resource != 0) backend_->update_emitter(target->resource, target->stream);
    }
    return true;
}

bool BattleEffects::step_effect(LiveEffect& effect, bool& gone) {
    gone = false;
    auto advanced = registry_->advance(effect.handle, 1.0F / 30.0F, camera_frame_);
    if (!advanced) {
        failure_ = "battle effect " + effect.particle + ": " + core::format_diagnostic(advanced.error());
        return false;
    }
    particles_ += advanced.value().particles;
    ++effect.age;
    if (!effect.detached && effect.age >= effect.lifetime) {
        // The particle object's lifetime ends: FoC detaches its system, which drains.
        auto detached = registry_->detach(effect.handle);
        effect.detached = true;
        gone = !detached || detached.value() == particles::EffectDetachState::released;
    } else if (effect.detached && (advanced.value().finished || effect.age >= effect.lifetime + drain_limit_frames)) {
        static_cast<void>(registry_->release(effect.handle));
        gone = true;
    }
    return true;
}

bool BattleEffects::advance_until(const std::uint64_t target) {
    for (; samples_ < target; ++samples_) {
        particles_ = 0;
        for (auto effect = effects_.begin(); effect != effects_.end();) {
            bool gone = false;
            if (effect->born <= samples_ && !step_effect(*effect, gone)) return false;
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
    // Events fire on the frame that first reaches their tick, oldest tick first; the effects
    // already live are aged up to each tick's birth before its effects are born.
    for (const platform::LiveTickEvents& record : reached) {
        birth_ = sample_of(static_cast<double>(record.tick) - 1.0);
        if (!advance_until(std::min(birth_, due))) return false;
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            // Shown when the local player sees the target: a lethal hit's target has left the
            // session by that tick, and its last visible frame stands for it (#370 review 1).
            if (!last_seen_.contains(event.target)) continue;
            ++hit_events_[record.tick];
            const auto shooter = entity_types_.find(event.shooter);
            const ProjectileLook* found = nullptr;
            if (shooter != entity_types_.end()) {
                if (const auto type = types_.find(shooter->second); type != types_.end()) {
                    if (const auto weapon = type->second.weapons.find(event.weapon); weapon != type->second.weapons.end()) {
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

void BattleEffects::release() {
    if (released_) return;
    released_ = true;
    for (const LiveEffect& effect : effects_) static_cast<void>(registry_->release(effect.handle));
    effects_.clear();
    for (Batch* target : {&kites_, &beams_}) {
        if (target->resource != 0) backend_->destroy_emitter(target->resource);
        target->resource = 0;
    }
}

void BattleEffects::write_report(std::ostream& output) const {
    output << "  \"battle_effects\": {\"frames\": " << frames_ << ", \"projectiles_drawn\": " << projectiles_drawn_
           << ", \"projectiles_hidden\": " << projectiles_hidden_ << ", \"max_kites\": " << max_kites_
           << ", \"kites_head_leading\": " << kites_head_leading_ << ", \"kites_head_trailing\": " << kites_head_trailing_
           << ", \"max_beams\": " << max_beams_ << ", \"projectile_models_drawn\": " << models_drawn_ << ", \"live_effects\": " << effects_.size()
           << ", \"max_live_effects\": " << max_live_effects_ << ", \"effects_dropped\": " << effects_dropped_;
    const auto counts = [&output](const char* name, const std::map<std::string, std::uint64_t>& values) {
        output << ", \"" << name << "\": {";
        bool first = true;
        for (const auto& [key, count] : values) {
            output << (first ? "" : ", ") << json(key) << ": " << count;
            first = false;
        }
        output << "}";
    };
    counts("spawned", spawned_);
    counts("spawn_failed", spawn_failed_);
    counts("expired", expired_);
    counts("projectiles_not_drawn", not_drawn_);
    // #456 BP-62: each model projectile pool: its slots, the most bound at once, the flights it
    // took and those it refused (every slot bound or resting).
    output << ", \"projectile_models\": {";
    for (auto pool = model_pools_.begin(); pool != model_pools_.end(); ++pool) {
        output << (pool == model_pools_.begin() ? "" : ", ") << json(pool->first) << ": {\"slots\": "
               << pool->second.slots.size() << ", \"max_bound\": " << pool->second.slots.max_bound()
               << ", \"bindings\": " << pool->second.slots.bindings() << ", \"refused\": "
               << pool->second.slots.refused() << "}";
    }
    output << "}, \"hit_picks\": {\"by_projectile\": " << hit_picks_by_projectile_ << ", \"by_event\": "
           << hit_picks_by_event_ << "}";
    output << ", \"shield_hits\": {\"on_mesh\": " << shield_on_mesh_ << ", \"no_mesh\": " << shield_no_mesh_
           << ", \"mesh_missed\": " << shield_mesh_missed_ << ", \"in_step\": " << shield_in_step_
           << ", \"ahead\": " << shield_ahead_ << ", \"casts\": {\"casts\": " << shield_casts_.casts
           << ", \"sphere_rejects\": " << shield_casts_.sphere_rejects << ", \"chunks_tested\": "
           << shield_casts_.chunks_tested << ", \"triangles_tested\": " << shield_casts_.triangles_tested
           << ", \"max_triangles_per_cast\": " << shield_casts_.max_triangles_per_cast << "}, \"meshes\": {";
    for (auto mesh = shield_meshes_.begin(); mesh != shield_meshes_.end(); ++mesh) {
        output << (mesh == shield_meshes_.begin() ? "" : ", ") << json(mesh->first) << ": {\"shield\": "
               << (mesh->second.first ? "true" : "false") << ", \"triangles\": " << mesh->second.second << "}";
    }
    const auto triple = [](const std::array<double, 3>& value) {
        char text[96];
        std::snprintf(text, sizeof(text), "[%.3f, %.3f, %.3f]", value[0], value[1], value[2]);
        return std::string(text);
    };
    output << "}, \"samples\": [";
    for (std::size_t index = 0; index < shield_samples_.size(); ++index) {
        const ShieldSample& sample = shield_samples_[index];
        output << (index ? ", " : "") << "{\"tick\": " << sample.tick << ", \"target\": " << sample.target
               << ", \"contact\": " << triple(sample.contact) << ", \"placed\": " << triple(sample.placed)
               << ", \"direction\": " << triple(sample.direction) << "}";
    }
    output << "]}";
    output << ", \"hit_events\": {";
    for (auto hit = hit_events_.begin(); hit != hit_events_.end(); ++hit) {
        output << (hit == hit_events_.begin() ? "" : ", ") << "\"" << hit->first << "\": " << hit->second;
    }
    output << "}, \"projectile_model_last_tick\": {";
    for (auto row = model_last_posed_tick_.begin(); row != model_last_posed_tick_.end(); ++row) {
        output << (row == model_last_posed_tick_.begin() ? "" : ", ") << "\"" << row->first << "\": " << row->second;
    }
    output << "}, \"spawn_log\": [";
    for (std::size_t index = 0; index < spawn_log_.size(); ++index) {
        const SpawnRow& row = spawn_log_[index];
        output << (index ? ", " : "") << "[" << row.tick << ", " << json(row.key) << ", "
               << (row.first_age ? std::to_string(*row.first_age) : std::string("null")) << "]";
    }
    output << "], \"spawn_log_full\": " << (spawn_log_.size() >= spawn_log_limit ? "true" : "false");
    output << ", \"particle_types\": {";
    bool first = true;
    for (const auto& [name, type] : particle_types_) {
        output << (first ? "" : ", ") << json(name) << ": {\"model\": " << json(type.model_path)
               << ", \"lifetime_frames\": " << type.lifetime_frames << ", \"ready\": " << (type.system ? "true" : "false")
               << ", \"cause\": " << json(type.cause) << "}";
        first = false;
    }
    output << "}, \"unresolved\": [";
    for (std::size_t index = 0; index < unresolved_.size(); ++index) {
        output << (index ? ", " : "") << json(unresolved_[index]);
    }
    output << "], \"failure\": " << (failure_.empty() ? std::string("null") : json(failure_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend

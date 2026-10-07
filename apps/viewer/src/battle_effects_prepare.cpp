#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"
#include "eawr/data/tag_trace.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/abilities.hpp"

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

[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name) {
    const data::EffectiveValue* value = object.value(name);
    return value == nullptr ? std::string{} : trim(value->value.raw_text);
}

[[nodiscard]] bool truthy(const std::string& text) {
    const std::string value = lower(text);
    return value == "1" || value == "yes" || value == "true";
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
[[nodiscard]] std::vector<space::ShieldTriangle> collision_triangles(const assets::Model& model,
    const bool shield_only = false, std::vector<std::uint32_t>* bones = nullptr) {
    std::vector<space::ShieldTriangle> result;
    const auto frames = units::bind_frames(model);
    for (const assets::Mesh& mesh : model.meshes) {
        if (shield_only && lower(mesh.name) != "shield") continue;
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
        const auto first = result.size();
        append_triangles(mesh, place, result);
        if (bones != nullptr) bones->insert(bones->end(), result.size() - first,
            mesh.bone < 0 ? 0U : static_cast<std::uint32_t>(mesh.bone));
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
} // namespace

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
    core::load_profile::Scope load_scope(core::load_profile::Phase::particles);
    laser_scales_ = presentation_constants::load_lasers(*filesystem_);
    if (auto constants = data::load_document(*filesystem_, "data/xml/gameconstants.xml")) {
        // OF-01/02: retain the authored effects and space scale, including mod overrides.
        constexpr std::array<std::string_view, 4> tags{"GUI_Move_Command_Ack_Effect",
            "GUI_Double_Click_Move_Command_Ack_Effect", "GUI_Attack_Move_Command_Ack_Effect",
            "GUI_Guard_Move_Command_Ack_Effect"};
        for (const auto& child : constants.value().root.children) {
            for (std::size_t index = 0; index < tags.size(); ++index) {
                if (lower(child.name) != lower(tags[index])) continue;
                data::tag_trace::used(child);
                move_particles_[index] = trim(child.raw_text);
            }
            if (lower(child.name) == "gui_move_acknowledge_scale_space") {
                data::tag_trace::used(child);
                const auto values = numbers(child.raw_text);
                if (!values.empty() && values.front() > 0.0F) move_scale_ = values.front();
            }
        }
        for (const auto& particle : move_particles_) {
            const auto* type = particle_type(particle);
            if (!type || !type->system) continue;
            for (const auto& emitter : type->system->emitters) {
                static_cast<void>(resolve_texture(emitter.color_texture));
                if (!emitter.normal_texture.empty()) static_cast<void>(resolve_texture(emitter.normal_texture));
            }
        }
        for (std::size_t index = 0; index < hero_beams_.size(); ++index) {
            auto& look = hero_beams_[index];
            const std::string prefix = index == 0 ? "Energy_Beam_" : "Tractor_Beam_";
            for (const auto& child : constants.value().root.children) {
                if (lower(child.name) == lower(prefix + "Texture")) {
                    data::tag_trace::used(child); look.texture = trim(child.raw_text);
                } else if (lower(child.name) == lower(prefix + "Width")) {
                    data::tag_trace::used(child);
                    const auto values = numbers(child.raw_text);
                    if (!values.empty()) look.width = std::max(0.0F, values.front());
                } else if (lower(child.name) == lower(prefix + "Color")) {
                    data::tag_trace::used(child);
                    const auto values = numbers(child.raw_text);
                    // TBF-01: these authored channels are bytes, not HDR multipliers.
                    const auto channel = [](const float value) { return std::clamp(value, 0.0F, 255.0F) / 255.0F; };
                    if (values.size() >= 3) look.colour = {channel(values[0]), channel(values[1]), channel(values[2]),
                        values.size() >= 4 ? channel(values[3]) : 1.0F};
                } else if (index == 1 && lower(child.name) == lower(prefix + "Frames")) {
                    data::tag_trace::used(child);
                    const std::string value = trim(child.raw_text);
                    std::int32_t frames{};
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
                    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) look.frames = frames;
                }
            }
        }
    }
    const auto look_of = [&](const std::string& projectile) {
        ProjectileLook look;
        look.projectile = projectile;
        auto object = catalog_->resolve(projectile, data::Category::game_object);
        if (!object) {
            unresolved_.push_back("projectile " + projectile + ": " + core::format_diagnostic(object.error()));
            return look;
        }
        const data::EffectiveObject& value = object.value();
        const auto behaviours = type_list(value, "Behavior");
        look.hide_when_fogged = std::any_of(behaviours.begin(), behaviours.end(),
            [](const auto& name) { return lower(name) == "hide_when_fogged"; });
        look.immediate_fog = truthy(tag(value, "Last_State_Visible_Under_FOW"));
        // BP-01: Projectile_Custom_Render 1 is the laser beam, 2 the laser kite; anything else
        // draws Space_Model_Name.
        const int custom = presentation_constants::custom_render(tag(value, "Projectile_Custom_Render"));
        look.render = custom == 1 ? Render::beam : custom == 2 ? Render::kite
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
        look.lifetime_detonation = tag(value, "Projectile_Lifetime_Detonation_Particle");
        const auto death_explosions = type_list(value, "Death_Explosions");
        if (!death_explosions.empty()) look.death_explosion = death_explosions.front();
        look.armor_reduced = tag(value, "Projectile_Object_Armor_Reduced_Detonation_Particle");
        look.shield_absorbed = tag(value, "Projectile_Absorbed_By_Shields_Particle");
        return look;
    };
    for (const auto& type : tables.units) for (const auto& ability : type.abilities) {
        if (ability.spawned_object.empty()) continue;
        const auto id = skirmish::type_id(ability.spawned_object);
        types_[id].weapons.emplace(tactical::object_weapon, look_of(ability.spawned_object));
        if (ability.spawned_projectile_index < tables.projectiles.size())
            weaken_particles_[id] = tables.projectiles[ability.spawned_projectile_index].weaken.status_effect;
    }
    // #862 (space-abilities AB-60, AB-66): the craft of a squadron whose team holds
    // ION_CANNON_SHOT fire its Projectile_Types_Override while it is on; craft type -> projectile.
    std::map<std::string, std::string> ability_projectiles;
    for (const units::UnitType& squadron : tables.units) {
        if (squadron.kind != units::UnitKind::squadron) continue;
        for (const units::Ability& ability : squadron.team_abilities) {
            if (ability.type != "ION_CANNON_SHOT" || ability.projectile_override.empty()) continue;
            for (const units::SquadronMember& member : squadron.members) {
                if (member.craft_index >= tables.units.size()) continue;
                ability_projectiles.emplace(tables.units[member.craft_index].id, ability.projectile_override);
            }
        }
    }
    for (const units::UnitType& type : tables.units) {
        TypeLooks looks;
        for (std::size_t index = 0; index < type.death_projectiles.size(); ++index) {
            looks.weapons.emplace(tactical::death_projectile_slot_flag | static_cast<std::uint32_t>(index),
                look_of(type.death_projectiles[index]));
        }
        for (const auto& ability : type.abilities)
            if (ability.type == "REPLENISH_WINGMEN") looks.replenish_particle = ability.replenish_particle;
        const tactical::CombatProfile* profile = combat.find(skirmish::type_id(type.id));
        const auto ability_projectile = ability_projectiles.find(type.id);
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
                // AB-66: a weapon the simulation gave an ability shot draws it with the override's look.
                const tactical::WeaponProfile* weapon = nullptr;
                if (profile != nullptr) {
                    for (const tactical::WeaponProfile& entry : profile->weapons) {
                        if (entry.hardpoint == index && entry.ability_shot) weapon = &entry;
                    }
                }
                if (weapon != nullptr && ability_projectile != ability_projectiles.end()) {
                    ProjectileLook ability = look_of(ability_projectile->second);
                    ability.step_length = static_cast<double>(to_float(weapon->ability_shot->speed));
                    looks.ability_weapons.emplace(static_cast<std::uint32_t>(index), std::move(ability));
                }
            }
            auto object = catalog_->resolve(hardpoint.id, data::Category::hardpoint);
            looks.hardpoint_explosions.push_back(object ? tag(object.value(), "Death_Explosion_Particles") : std::string{});
        }
        if (type.weapon && !type.weapon->projectile.empty()) {
            ProjectileLook look = look_of(type.weapon->projectile);
            look.step_length = step_of(tactical::object_weapon);
            looks.weapons.emplace(tactical::object_weapon, std::move(look));
        }
        for (const units::Ability& ability : type.abilities) {
            if (tactical::ability_kind(ability.type) != tactical::AbilityKind::barrage
                || ability.projectile_override.empty() || profile == nullptr) continue;
            for (const tactical::WeaponProfile& weapon : profile->weapons) {
                if (!weapon.barrage_shot) continue;
                ProjectileLook look = look_of(ability.projectile_override);
                look.step_length = static_cast<double>(to_float(weapon.barrage_shot->speed));
                looks.barrage_weapons.emplace(weapon.hardpoint, std::move(look));
            }
        }
        if (auto object = catalog_->resolve(type.id, data::Category::game_object)) {
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
            looks.asteroid_hits = type_list(object.value(), "Asteroid_Damage_Hit_Particles");
        }
        const tactical::TypeId id = skirmish::type_id(type.id);
        if (type.scale_factor) looks.scale = to_float(*type.scale_factor);
        // BP-17, BP-19: only a shield takes a hit whole, so only shielded types need their
        // collision meshes and whether they have a SHIELD sub-object.
        if (!type.model_path.empty()) {
            if (auto model = assets::load_model(*filesystem_, type.model_path)) {
                looks.shield_mesh = has_shield_mesh(model.value());
                if (type.shielded || !looks.asteroid_hits.empty()) {
                    looks.collision = space::make_shield_collision_mesh(collision_triangles(model.value(), false, &looks.collision_bones));
                }
                // PS-02: the event's damaged hardpoint names the contact mesh; absent mesh uses bone 0.
                for (const units::Hardpoint& hardpoint : type.hardpoints) {
                    const auto mesh = std::find_if(model.value().meshes.begin(), model.value().meshes.end(),
                        [&](const assets::Mesh& entry) { return lower(entry.name) == lower(hardpoint.collision_mesh); });
                    looks.hardpoint_contact_bones.push_back(mesh == model.value().meshes.end() || mesh->bone < 0
                        ? 0U : static_cast<std::uint32_t>(mesh->bone));
                }
                if (!looks.asteroid_hits.empty()) looks.asteroid_collision = space::make_shield_collision_mesh(
                    collision_triangles(model.value(), looks.shield_mesh));
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
        for (const auto& nested : type.special_abilities) {
            if (nested.kind != tactical::SpecialAbilityKind::energy_weapon && nested.kind != tactical::SpecialAbilityKind::tractor_beam) continue;
            const auto beam = nested.kind == tactical::SpecialAbilityKind::energy_weapon ? 0U : 1U;
            if (beam == 0) looks.energy_owner_particle = nested.beam_owner_particle;
            if (!nested.beam_owner_bone.empty() && !type.model_path.empty()) {
                if (auto model = assets::load_model(*filesystem_, type.model_path)) {
                    const auto frames = units::bind_frames(model.value());
                    if (frames) for (std::size_t bone = 0; bone < model.value().bones.size(); ++bone) {
                        if (lower(model.value().bones[bone].name) != lower(nested.beam_owner_bone)) continue;
                        const auto& frame = frames.value()[bone];
                        looks.beam_origins[beam] = {-to_float(frame.rows[1][3]) * looks.scale,
                            to_float(frame.rows[0][3]) * looks.scale, to_float(frame.rows[2][3]) * looks.scale};
                        break;
                    }
                }
            }
        }
        types_.emplace(id, std::move(looks));
    }
}

const BattleEffects::ParticleType* BattleEffects::particle_type(const std::string& name) {
    if (name.empty()) return nullptr;
    auto found = particle_types_.find(name);
    if (found != particle_types_.end()) return &found->second;
    ParticleType type;
    auto object = catalog_->resolve(name, data::Category::game_object);
    if (!object) {
        type.cause = "not in the XML catalog";
    } else {
        const std::string model = tag(object.value(), "Space_Model_Name");
        const auto lifetime = numbers(tag(object.value(), "Particle_Lifetime_Frames"));
        type.lifetime_frames = lifetime.empty() ? 0U : static_cast<std::uint32_t>(std::max(0.0F, lifetime.front()));
        type.attached_to_collision = truthy(tag(object.value(), "Particle_Attach_To_Collision"));
        type.decoration = truthy(tag(object.value(), "Is_Decoration"));
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

} // namespace eawr::presentation::godot_backend

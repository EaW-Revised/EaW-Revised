#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"
#include "eawr/data/tag_trace.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
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

void BattleEffects::prepare_effect_constants() {
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
}

template <typename LookOf>
void BattleEffects::prepare_unit_looks(const units::UnitTables& tables, const sim::tactical::CombatTable& combat, LookOf& look_of) {
    scene::VfsAssetCache model_cache(*filesystem_);
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
        looks.healing = type.special_abilities;
        for (const auto& handler : looks.healing) {
            if (handler.kind != tactical::SpecialAbilityKind::force_healing || handler.heal_lightning.empty()
                || healing_lightning_.contains(handler.heal_lightning)) continue;
            const auto document = data::load_document(*filesystem_, "data/xml/lightningeffecttypes.xml");
            if (!document) continue;
            for (const auto& effect : document.value().root.children) {
                const auto name = std::find_if(effect.attributes.begin(), effect.attributes.end(), [](const auto& attribute) {
                    return lower(attribute.name) == "name";
                });
                if (name == effect.attributes.end() || lower(name->value) != lower(handler.heal_lightning)) continue;
                HealLightning look;
                const auto read = [&](const std::string_view tag_name) {
                    for (const auto& child : effect.children) if (lower(child.name) == lower(tag_name)) {
                        data::tag_trace::used(child); return numbers(child.raw_text);
                    }
                    return std::vector<float>{};
                };
                const auto scalar = [&](const std::string_view tag_name) { const auto v = read(tag_name); return v.empty() ? 0.0F : v[0]; };
                for (const auto& child : effect.children) if (lower(child.name) == "texture_name") {
                    data::tag_trace::used(child); look.texture = trim(child.raw_text);
                }
                look.width_min = scalar("Width_Min"); look.width_max = scalar("Width_Max");
                look.fade_min = scalar("Fadeout_Time_Min"); look.fade_max = scalar("Fadeout_Time_Max");
                look.interval_min = scalar("Bolt_Creation_Interval_Min"); look.interval_max = scalar("Bolt_Creation_Interval_Max");
                look.detail = static_cast<std::uint32_t>(std::clamp(scalar("Detail"), 1.0F, 64.0F));
                look.bolts = static_cast<std::uint32_t>(std::clamp(scalar("Number_Bolts"), 0.0F, 128.0F));
                const auto displacement = read("Displace");
                for (std::size_t i = 0; i < std::min(displacement.size(), look.displacement.size()); ++i) look.displacement[i] = displacement[i];
                const auto colour = [&](const std::string_view tag_name) {
                    const auto v = read(tag_name);
                    return v.size() >= 4 ? particles::Color{v[0] / 255.0F, v[1] / 255.0F, v[2] / 255.0F, v[3] / 255.0F} : particles::Color{};
                };
                look.start = colour("Color_Start"); look.end = colour("Color_End");
                healing_lightning_.emplace(handler.heal_lightning, std::move(look));
            }
        }
        for (const auto& special : type.special_abilities) {
            if (special.kind != tactical::SpecialAbilityKind::corrupt_systems) continue;
            looks.corrupt_target_particle = special.corrupt_target_particle;
            looks.corrupt_owner_particle = special.corrupt_owner_particle;
        }
        for (std::size_t index = 0; index < type.death_projectiles.size(); ++index) {
            looks.weapons.emplace(tactical::death_projectile_slot_flag | static_cast<std::uint32_t>(index),
                look_of(type.death_projectiles[index]));
        }
        // RP-09: the station's retreat explosion is looked up by its own weapon slot.
        if (!type.retreat_self_destruct.empty())
            looks.weapons.emplace(tactical::retreat_explosion_slot_flag, look_of(type.retreat_self_destruct));
        for (const auto& ability : type.abilities)
            if (ability.type == "REPLENISH_WINGMEN") looks.replenish_particle = ability.replenish_particle;
            else if (ability.type == "LURE") looks.lure_particle = ability.lure_particle;
            else if (ability.type == "CONCENTRATE_FIRE") {
                looks.concentrate_particle = ability.concentrate_particle;
                if (const auto particle = catalog_->resolve(looks.concentrate_particle, data::Category::game_object); particle) {
                    const auto authored_scale = numbers(tag(particle.value(), "Scale_Factor"));
                    if (!authored_scale.empty()) looks.concentrate_scale = authored_scale.front();
                }
            }
        const tactical::CombatProfile* profile = combat.find(battle::type_id(type.id));
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
                if (!hardpoint.weapon->charged_projectile.empty()) {
                    ProjectileLook charged = look_of(hardpoint.weapon->charged_projectile);
                    if (profile) for (const auto& gun : profile->weapons)
                        if (gun.hardpoint == index && gun.charged_shot)
                            charged.step_length = static_cast<double>(to_float(gun.charged_shot->speed));
                    looks.charged_weapons.emplace(static_cast<std::uint32_t>(index), std::move(charged));
                }
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
        const tactical::TypeId id = battle::type_id(type.id);
        if (type.scale_factor) looks.scale = to_float(*type.scale_factor);
        // BP-17, BP-19: only a shield takes a hit whole, so only shielded types need their
        // collision meshes and whether they have a SHIELD sub-object.
        if (!type.model_path.empty()) {
            if (auto model = model_cache.shared_model(type.model_path)) {
                looks.shield_mesh = has_shield_mesh(*model.value());
                for (std::size_t bone = 0; bone < model.value()->bones.size(); ++bone)
                    looks.named_bones.emplace(lower(model.value()->bones[bone].name), static_cast<std::uint32_t>(bone));
                if (type.shielded || !looks.asteroid_hits.empty()) {
                    looks.collision = space::make_shield_collision_mesh(collision_triangles(*model.value(), false, &looks.collision_bones));
                }
                // PS-02: the event's damaged hardpoint names the contact mesh; absent mesh uses bone 0.
                for (const units::Hardpoint& hardpoint : type.hardpoints) {
                    const auto mesh = std::find_if(model.value()->meshes.begin(), model.value()->meshes.end(),
                        [&](const assets::Mesh& entry) { return lower(entry.name) == lower(hardpoint.collision_mesh); });
                    looks.hardpoint_contact_bones.push_back(mesh == model.value()->meshes.end() || mesh->bone < 0
                        ? 0U : static_cast<std::uint32_t>(mesh->bone));
                }
                if (!looks.asteroid_hits.empty()) looks.asteroid_collision = space::make_shield_collision_mesh(
                    collision_triangles(*model.value(), looks.shield_mesh));
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
            if (nested.kind == tactical::SpecialAbilityKind::blast && nested.charged_blast) {
                looks.blast_charging_particle = nested.charged_blast->charging_effect;
                looks.blast_charged_particle = nested.charged_blast->charged_effect;
            }
            if (nested.kind == tactical::SpecialAbilityKind::laser_defense) {
                // WLD-12: the beam's authored look; the origin is the bone nearest the shot at draw time.
                auto& defence = looks.defence;
                defence.texture = nested.defence_beam_texture;
                defence.width = std::max(0.0F, to_float(nested.defence_beam_width));
                const auto channel = [&](const std::size_t index) { return static_cast<float>(nested.defence_colour[index]) / 255.0F; };
                defence.colour = {channel(0), channel(1), channel(2), channel(3)};
                defence.frames = nested.defence_beam_frames;
                if (!type.model_path.empty()) {
                    if (auto model = model_cache.shared_model(type.model_path)) {
                        const auto frames = units::bind_frames(*model.value());
                        if (frames) for (const auto& name : nested.defence_bones) {
                            for (std::size_t bone = 0; bone < model.value()->bones.size(); ++bone) {
                                if (lower(model.value()->bones[bone].name) != lower(name)) continue;
                                const auto& frame = frames.value()[bone];
                                // Keep bind points in model space; the zap consumer applies the quarter turn.
                                defence.bones.push_back({to_float(frame.rows[0][3]) * looks.scale,
                                    to_float(frame.rows[1][3]) * looks.scale, to_float(frame.rows[2][3]) * looks.scale});
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (nested.kind != tactical::SpecialAbilityKind::energy_weapon && nested.kind != tactical::SpecialAbilityKind::tractor_beam
                && nested.kind != tactical::SpecialAbilityKind::leech_shields) continue;
            const auto beam = nested.kind == tactical::SpecialAbilityKind::energy_weapon ? 0U
                : nested.kind == tactical::SpecialAbilityKind::leech_shields ? 2U : 1U;
            if (beam == 2) {
                auto& look = leech_beams_[id];
                look.texture = nested.leech_texture;
                look.width = to_float(nested.leech_beam_width);
                look.frames = static_cast<std::int32_t>(nested.leech_beam_frames);
                const auto& c = nested.leech_colour;
                look.colour = {c[0] / 255.0F, c[1] / 255.0F, c[2] / 255.0F, c[3] / 255.0F};
                looks.leech_effect = nested.leech_effect;
            }
            if (beam == 0) looks.energy_owner_particle = nested.beam_owner_particle;
            // TBF-05: tractor uses the conventional model socket, not Beam_Bone_Name.
            const auto origin_bone = beam == 1 ? std::string{"HP_trac_bone"} : nested.beam_owner_bone;
            if (!origin_bone.empty() && !type.model_path.empty()) {
                if (auto model = model_cache.shared_model(type.model_path)) {
                    const auto frames = units::bind_frames(*model.value());
                    if (frames) for (std::size_t bone = 0; bone < model.value()->bones.size(); ++bone) {
                        if (lower(model.value()->bones[bone].name) != lower(origin_bone)) continue;
                        const auto& frame = frames.value()[bone];
                        looks.beam_bones[beam] = static_cast<std::uint32_t>(bone);
                        // live_model_point applies the model quarter-turn; keep this fallback in asset space.
                        looks.beam_origins[beam] = {to_float(frame.rows[0][3]) * looks.scale,
                            to_float(frame.rows[1][3]) * looks.scale, to_float(frame.rows[2][3]) * looks.scale};
                        break;
                    }
                }
            }
            if (beam != 0) {
                auto& lightning = (beam == 2 ? leech_beams_[id] : hero_beams_[beam]).lightning;
                const std::string effect_name = beam == 1 ? "Tractor_Beam" : nested.leech_lightning;
                if (effect_name.empty() || !lightning.name.empty()) continue;
                const auto document = data::load_document(*filesystem_, "data/xml/lightningeffecttypes.xml");
                if (!document) continue;
                for (const auto& effect : document.value().root.children) {
                    const auto name = std::find_if(effect.attributes.begin(), effect.attributes.end(), [](const auto& attribute) {
                        return lower(attribute.name) == "name";
                    });
                    if (name == effect.attributes.end() || lower(name->value) != lower(effect_name)) continue;
                    lightning.name = effect_name;
                    const auto text = [&](const std::string_view key) {
                        for (const auto& child : effect.children) if (lower(child.name) == lower(key)) {
                            data::tag_trace::used(child); return trim(child.raw_text);
                        }
                        return std::string{};
                    };
                    const auto scalar = [&](const std::string_view key, const float fallback) {
                        const auto values = numbers(text(key)); return values.empty() ? fallback : values.front();
                    };
                    // TBF-07: defaults are engine defaults, not bounds inferred from retail data.
                    lightning.texture = text("Texture_Name");
                    if (lightning.texture.empty()) lightning.texture = "lightning_default.tga";
                    lightning.width_min = scalar("Width_Min", 20.0F); lightning.width_max = scalar("Width_Max", 5.0F);
                    lightning.fade_min = scalar("Fadeout_Time_Min", 3.0F); lightning.fade_max = scalar("Fadeout_Time_Max", 1.0F);
                    lightning.interval_min = scalar("Bolt_Creation_Interval_Min", 0.05F);
                    lightning.interval_max = scalar("Bolt_Creation_Interval_Max", 0.3F);
                    lightning.detail = scalar("Detail", 10.0F);
                    lightning.count = static_cast<std::uint32_t>(std::clamp(static_cast<double>(scalar("Number_Bolts", 10.0F)),
                        0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
                    lightning.cycles = scalar("Cycles_Per_Distance", 0.0F);
                    lightning.rotations = scalar("Rotations_Per_Second", 0.0F);
                    lightning.repeat = scalar("Texture_Repeat", 0.0F); lightning.scroll = scalar("Texture_Scroll", 0.0F);
                    lightning.update_all = truthy(text("Update_All")); lightning.replace_faded = truthy(text("Replace_Faded"));
                    const auto displacement = numbers(text("Displace"));
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        lightning.displacement[axis] = std::clamp(axis < displacement.size() ? displacement[axis] : 0.4F, 0.0F, 1.0F);
                    const auto colour = [&](const std::string_view key, const particles::Color fallback) {
                        const auto v = numbers(text(key));
                        return v.size() >= 4 ? particles::Color{v[0] / 255.0F, v[1] / 255.0F, v[2] / 255.0F, v[3] / 255.0F} : fallback;
                    };
                    lightning.start = colour("Color_Start", {100.0F / 255.0F, 100.0F / 255.0F, 1.0F, 1.0F});
                    lightning.end = colour("Color_End", {0.0F, 0.0F, 0.0F, 0.0F});
                    break;
                }
            }
        }
        types_.emplace(id, std::move(looks));
    }
}

void BattleEffects::prewarm_effect_programs() {
    // Warm programs from the references this battle's loaded looks actually use.
    // Resource holds are bounded by the backend; definitions decoded only for
    // this pass are released so the lazy CPU effect cache does not grow here.
    const auto warm_start = std::chrono::steady_clock::now();
    std::set<std::string_view> seen;
    const auto warm = [&](const std::string& name) {
        if (name.empty() || !seen.emplace(name).second) return;
        if (!backend_->begin_prewarm_reference()) return;
        const bool cached = particle_types_.contains(name);
        const auto lookup_start = std::chrono::steady_clock::now();
        const auto* type = particle_type(name);
        backend_->record_prewarm_lookup(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - lookup_start).count(), !cached && type != nullptr && bool(type->system));
        if (type != nullptr && type->system) {
            for (const auto& plan : particles::plan_system(*type->system)) {
                if (plan.drawable) static_cast<void>(backend_->prewarm_program(plan));
            }
        }
        if (!cached) particle_types_.erase(name);
    };
    for (const auto& name : move_particles_) warm(name);
    for (const auto& entry : weaken_particles_) warm(entry.second);
    for (const auto& entry : types_) {
        const auto& looks = entry.second;
        for (const auto* weapons : {&looks.weapons, &looks.ability_weapons,
                &looks.barrage_weapons, &looks.charged_weapons}) {
            for (const auto& weapon : *weapons) {
                const auto& look = weapon.second;
                for (const auto* name : {&look.detonation, &look.lifetime_detonation,
                        &look.death_explosion, &look.armor_reduced, &look.shield_absorbed}) warm(*name);
            }
        }
        for (const auto* name : {&looks.death_explosion, &looks.spin_explosion, &looks.leech_effect,
                &looks.energy_owner_particle, &looks.replenish_particle, &looks.corrupt_target_particle,
                &looks.corrupt_owner_particle, &looks.lure_particle, &looks.concentrate_particle, &looks.blast_charging_particle,
                &looks.blast_charged_particle}) warm(*name);
        for (const auto* names : {&looks.hardpoint_explosions, &looks.damage_hits,
                &looks.shield_hits, &looks.asteroid_hits}) {
            for (const auto& name : *names) warm(name);
        }
        for (const auto& handler : looks.healing) warm(handler.heal_particle);
    }
    backend_->finish_program_prewarm(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - warm_start).count());
}

void BattleEffects::prepare(const units::UnitTables& tables, const tactical::CombatTable& combat) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::particles);
    prepare_effect_constants();
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
        const auto visible_to_enemy = tag(value, "Model_Visible_To_Enemy");
        look.visible_to_enemy = visible_to_enemy.empty() || truthy(visible_to_enemy);
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
        const auto id = battle::type_id(ability.spawned_object);
        types_[id].weapons.emplace(tactical::object_weapon, look_of(ability.spawned_object));
        if (ability.spawned_projectile_index < tables.projectiles.size())
            weaken_particles_[id] = tables.projectiles[ability.spawned_projectile_index].weaken.status_effect;
    }
    // #862 (space-abilities AB-60, AB-66): the craft of a squadron whose team holds
    // ION_CANNON_SHOT fire its Projectile_Types_Override while it is on; craft type -> projectile.
    for (const units::UnitType& type : tables.units) {
        if (!type.cluster_bomb) continue;
        // WCB-01: the real bomb authors object death effects for either terminal path.
        auto look = look_of(type.cluster_bomb->bomb_type);
        if (look.detonation.empty()) look.detonation = look.death_explosion;
        if (look.lifetime_detonation.empty()) look.lifetime_detonation = look.death_explosion;
        types_[battle::type_id(type.cluster_bomb->bomb_type)].weapons.emplace(tactical::object_weapon,
            std::move(look));
    }
    prepare_unit_looks(tables, combat, look_of);
    prewarm_effect_programs();
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
                type.system = std::make_shared<const particles::SystemDefinition>(std::move(system).value());
            }
        }
    }
    return &particle_types_.emplace(name, std::move(type)).first->second;
}

} // namespace eawr::presentation::godot_backend

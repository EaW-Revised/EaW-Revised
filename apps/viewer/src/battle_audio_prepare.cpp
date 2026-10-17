#include "eawr/core/load_profile.hpp"
#include "battle_audio.hpp"
#include "audio_output.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/tag_trace.hpp"

#include <godot_cpp/classes/audio_effect_hard_limiter.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "battle_audio_internal.hpp"
#include "eawr/presentation/audio/hardpoint_loss.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_audio_detail;

namespace battle_audio_detail {

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] bool same(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name) {
    // ML-24 (#3631): a blank or TBD variant value inherits from its parent.
    return object.single_text(name);
}

ui::RetreatFactionPresentation read_retreat_faction(const data::Catalog& catalog, const std::string& faction) {
    ui::RetreatFactionPresentation out;
    if (const auto resolved = catalog.resolve(faction, data::Category::faction)) {
        const auto read = [&](const std::string_view key) {
            const auto* field = resolved.value().value(key);
            if (!field) return std::string{};
            data::tag_trace::used(field->value);
            return trim(field->value.raw_text);
        };
        out.begin = read("Space_Retreat_Begin_SFXEvent");
        out.enemy_begin = read("Space_Retreat_Enemy_Begin_SFXEvent");
        out.cancel = read("Space_Retreat_Cancel_SFXEvent");
        out.not_allowed = read("Space_Retreat_Not_Allowed_SFXEvent");
        out.reason = {read("Space_Retreat_Not_Allowed_Reason_1_SFXEvent"), read("Space_Retreat_Not_Allowed_Reason_2_SFXEvent"),
            read("Space_Retreat_Not_Allowed_Reason_3_SFXEvent")};
        out.countdown_text_id = read("Space_Retreat_Countdown_Text_ID");
        // RP-08: four comma separated 0..255 channels; a malformed list keeps opaque white.
        const auto colour = read("Space_Retreat_Countdown_Color_RGBA");
        std::array<int, 4> channels{255, 255, 255, 255};
        std::size_t index = 0;
        bool valid = !colour.empty();
        for (std::size_t at = 0; at < colour.size() && valid;) {
            std::size_t used = 0;
            if (index == 4) { valid = false; break; }
            try { channels[index++] = std::stoi(colour.substr(at), &used); } catch (...) { valid = false; break; }
            at += used;
            while (at < colour.size() && (colour[at] == ',' || std::isspace(static_cast<unsigned char>(colour[at])) != 0)) ++at;
        }
        if (valid && index == 4)
            for (std::size_t i = 0; i < 4; ++i) out.colour[i] = static_cast<std::uint8_t>(std::clamp(channels[i], 0, 255));
    }
    return out;
}
} // namespace battle_audio_detail

namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] std::string child(const data::XmlNode& node, const std::string_view name) {
    for (const data::XmlNode& item : node.children) {
        if (same(item.name, name)) return trim(item.raw_text);
    }
    return {};
}

[[nodiscard]] std::vector<audio::Field> fields_of(const data::XmlNode& node) {
    std::vector<audio::Field> fields;
    fields.reserve(node.children.size());
    for (const data::XmlNode& item : node.children) fields.emplace_back(item.name, trim(item.raw_text));
    return fields;
}

[[nodiscard]] std::string attribute(const data::XmlNode& node, const std::string_view name) {
    for (const data::XmlAttribute& item : node.attributes) {
        if (same(item.name, name)) return trim(item.value);
    }
    return {};
}

[[nodiscard]] std::optional<double> number(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return std::nullopt;
    return value;
}

// "Space, Space_Map_Rebel_Ambient_Music_Event": the event of a map environment entry.
[[nodiscard]] std::optional<std::string> environment_entry(const std::string& value, const std::string_view environment) {
    const auto items = audio::split_list(value);
    if (items.size() < 2 || !same(items[0], environment)) return std::nullopt;
    return items[1];
}
} // namespace

const audio::SfxEvent* BattleAudio::event(const std::string_view name, const std::string& where) {
    const std::string text = trim(name);
    if (text.empty()) return nullptr;
    const audio::SfxEvent* found = registry_.find(text);
    if (found == nullptr) missing_events_.insert(where + ": " + text);
    return found;
}

void BattleAudio::prepare(const units::UnitTables& tables, const tactical::CombatTable& combat,
                          const std::string_view local_faction, std::set<tactical::TypeId> first_types) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::audio);
    local_faction_ = std::string(local_faction);
    // Follow automatic spawners and squadron composition from the actual force.
    // The rest of the table still binds all sounds, but no longer delays Begin.
    bool changed = true;
    while (changed) {
        const auto count = first_types.size();
        const auto add_index = [&](const std::uint32_t index) {
            if (index < tables.units.size()) first_types.insert(skirmish::type_id(tables.units[index].id));
        };
        for (const auto& type : tables.units) {
            if (!first_types.contains(skirmish::type_id(type.id))) continue;
            for (const auto& member : type.members) add_index(member.craft_index);
            if (type.spawner) {
                for (const auto& entry : type.spawner->starting) if (entry.count > 0) add_index(entry.squadron_index);
                for (const auto& list : type.spawner->campaign_starting)
                    for (const auto& entry : list) if (entry.count > 0) add_index(entry.squadron_index);
            }
        }
        changed = first_types.size() != count;
    }
    std::set<std::string> immediate_roots;
    bool binding_unit = false, first_type = true;
    const auto event = [this, &immediate_roots, &binding_unit, &first_type](
            const std::string_view name, const std::string& where) {
        const audio::SfxEvent* cue = this->event(name, where);
        if (cue && (!binding_unit || (first_type && cue->is_3d))) immediate_roots.insert(cue->name);
        return cue;
    };
    // BA-01: the SFXEvents in SFXEventFiles order (presets first), each definition in file order.
    std::vector<const data::Definition*> definitions;
    for (const data::Definition& definition : catalog_->definitions()) {
        if (definition.category == data::Category::sfx && definition.active) definitions.push_back(&definition);
    }
    std::stable_sort(definitions.begin(), definitions.end(), [](const data::Definition* a, const data::Definition* b) {
        return std::pair(a->registry_order, a->definition_order) < std::pair(b->registry_order, b->definition_order);
    });
    for (const data::Definition* definition : definitions) {
        registry_.add(definition->id, fields_of(definition->root), problems_);
    }
    // BA-10: audio.xml's space factors and the peace time.
    double peace_seconds = 15.0;
    std::array<std::string, 2> summary_names;
    if (auto document = data::load_document(*filesystem_, "data/xml/audio.xml")) {
        const data::XmlNode& root = document.value().root;
        command_cues_[static_cast<std::size_t>(audio::CommandCue::attack)] = event(child(root, "SFXEvent_Command_Bar_Attack"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::attack_move)] = event(child(root, "SFXEvent_Command_Bar_Attack_Move"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::guard)] = event(child(root, "SFXEvent_Command_Bar_Guard"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::move)] = event(child(root, "SFXEvent_Command_Bar_Move"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::stop)] = event(child(root, "SFXEvent_Command_Bar_Stop"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::negative)] = event(child(root, "SFXEvent_GUI_Negative_Feedback"), "Audio");
        command_cues_[static_cast<std::size_t>(audio::CommandCue::waypoint)] = event(child(root, "SFXEvent_Command_Bar_Waypoint"), "Audio");
        options_.admission.negative_feedback = command_cues_[static_cast<std::size_t>(audio::CommandCue::negative)];
        for (const auto& item : root.children) {
            const int side = same(item.name, "Music_Event_Battle_End_Summary_Screen_Win") ? 0
                : same(item.name, "Music_Event_Battle_End_Summary_Screen_Lose") ? 1 : -1;
            if (side < 0) continue;
            data::tag_trace::used(item);
            summary_names[static_cast<std::size_t>(side)] = trim(item.raw_text);
        }
        const auto spatial = audio::space_3d(fields_of(root), problems_);
        spatial_audio_available_ = spatial.has_value();
        if (spatial) space_ = *spatial;
        if (const auto value = number(child(root, "Music_Space_Battle_To_Ambient_Peace_Seconds"))) peace_seconds = *value;
        for (const auto& item : root.children) {
            if (!same(item.name, "Delay_Between_Space_Base_Attack_Announcement_Seconds")) continue;
            data::tag_trace::used(item);
            if (const auto value = number(trim(item.raw_text)); value && *value >= 0.0
                && *value <= static_cast<double>(std::numeric_limits<int>::max()) / 30.0) {
                base_warning_delay_frames_ = static_cast<std::uint64_t>(*value * 30.0);
            }
        }
    } else {
        problems_.push_back("data/xml/audio.xml: " + core::format_diagnostic(document.error()));
        problems_.push_back("EAWR-AUDIO-0301: audio.xml unavailable; spatial playback uses 2D");
    }
    // BA-20: the command rankings.
    if (auto document = data::load_document(*filesystem_, "data/xml/gameconstants.xml")) {
        rankings_ = audio::split_list(child(document.value().root, "Unit_Command_Rankings_By_Category"));
        if (const auto value = number(child(document.value().root, "SpaceIdleMovementSpeed"))) engine_idle_speed_ = *value;
    } else {
        problems_.push_back("data/xml/gameconstants.xml: " + core::format_diagnostic(document.error()));
    }
    // BA-40: the music events and the local faction's space lists.
    if (auto document = data::load_document(*filesystem_, "data/xml/musicevents.xml")) {
        for (const data::XmlNode& node : document.value().root.children) {
            if (!same(node.name, "MusicEvent")) continue;
            music_events_.push_back(audio::parse_music_event(attribute(node, "Name"), fields_of(node)));
        }
    } else {
        problems_.push_back("data/xml/musicevents.xml: " + core::format_diagnostic(document.error()));
    }
    std::vector<const audio::MusicEvent*> ambient;
    for (std::size_t side = 0; side < summary_names.size(); ++side) {
        if (summary_names[side].empty()) continue;
        const auto found = std::find_if(music_events_.begin(), music_events_.end(),
            [&](const auto& music) { return same(music.name, summary_names[side]); });
        if (found != music_events_.end() && !found->files.empty()) summary_events_[side] = &*found;
        else problems_.push_back("summary music event " + summary_names[side] + " did not resolve to playable files");
    }
    std::vector<const audio::MusicEvent*> battle;
    // WBF-38: the local faction chooses the space victory/defeat HUD sound.
    if (auto faction = catalog_->resolve(local_faction, data::Category::faction)) {
        constexpr std::array special_keys{"SFXEvent_HUD_Special_Weapon_Ready", "SFXEvent_HUD_Special_Weapon_Firing",
            "SFXEvent_HUD_Enemy_Special_Weapon_Ready", "SFXEvent_HUD_Enemy_Special_Weapon_Firing"};
        // WSW-33/34: derived lists replace their inherited list as a whole, including empty entries.
        for (auto layer = faction.value().chain.rbegin(); layer != faction.value().chain.rend(); ++layer) {
            const auto* definition = catalog_->find(*layer, data::Category::faction);
            if (!definition) continue;
            for (std::size_t index = 0; index < special_keys.size(); ++index) {
                std::vector<const audio::SfxEvent*> cues;
                for (const auto& item : definition->root.children) if (same(item.name, special_keys[index])) {
                    data::tag_trace::used(item);
                    cues.push_back(event(trim(item.raw_text), std::string(local_faction)));
                }
                if (!cues.empty()) special_speech_[index] = std::move(cues);
            }
        }
        // SND-40: a faction WAV request, independent of production speech streams.
        if (const auto* field = faction.value().value("SFXEvent_Space_Base_Under_Attack_Announcement")) {
            data::tag_trace::used(field->value);
            base_under_attack_ = event(trim(field->value.raw_text), std::string(local_faction));
        }
        constexpr std::array keys{"Reinforcements_Selection_SFXEvent", "Reinforcements_Pick_Landing_Zone_SFXEvent", "Reinforcements_Enroute_SFXEvent", "Reinforcements_Cancelled_SFXEvent"};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            reinforcement_sounds_[index] = event(tag(faction.value(), keys[index]), std::string(local_faction));
        }
        for (std::size_t side = 0; side < outcome_events_.size(); ++side) {
            const std::string_view key = side == 0 ? "SFXEvent_HUD_Won_Space_Battle" : "SFXEvent_HUD_Lost_Space_Battle";
            if (const auto* value = faction.value().value(key)) {
                data::tag_trace::used(value->value);
                outcome_events_[side] = event(trim(value->value.raw_text), std::string(local_faction));
            }
        }
    }
    // The lists repeat their tag, which the effective object merges to one value: read the
    // faction's own definition.
    if (const data::Definition* faction = catalog_->find(local_faction, data::Category::faction); faction != nullptr) {
        // SND-54: retain repeated overrides in source order, including blank event entries.
        tactical_music_fields_ = fields_of(faction->root);
        if (const auto resolved = catalog_->resolve(local_faction, data::Category::faction)) {
            for (const auto key : {"Music_Event_Tactical_Win", "Music_Event_Tactical_Lose"}) {
                tactical_music_fields_.emplace_back(key, tag(resolved.value(), key));
            }
        }
        const auto find_music = [this](const std::string& name) -> const audio::MusicEvent* {
            for (const audio::MusicEvent& music : music_events_) {
                if (same(music.name, name)) return &music;
            }
            problems_.push_back("music event " + name + " is not in musicevents.xml");
            return nullptr;
        };
        for (const data::XmlNode& item : faction->root.children) {
            std::vector<const audio::MusicEvent*>* list = same(item.name, "Music_Event_List_Ambient") ? &ambient
                : same(item.name, "Music_Event_List_Battle") ? &battle : nullptr;
            if (list == nullptr) continue;
            if (const auto name = environment_entry(trim(item.raw_text), "Space")) {
                if (const audio::MusicEvent* music = find_music(*name)) list->push_back(music);
            }
        }
        if (ambient.empty() || battle.empty()) problems_.push_back("faction " + std::string(local_faction) + " names no space music");
    } else {
        problems_.push_back("faction '" + std::string(local_faction) + "' is not in the XML catalog");
    }
    // MAT-18, SND-71: the demo battle starts no tactical music; the menu's music is the front end's.
    if (!options_.attract_silent) {
        music_ = std::make_unique<audio::MusicDirector>(
            std::move(ambient), std::move(battle),
            static_cast<std::uint64_t>(std::llround(peace_seconds * tactical::logical_frames_per_second)));
    }

    // SND-44: speech is a distinct registry, not a WAV event or a music playlist.
    if (auto document = data::load_document(*filesystem_, "data/xml/speechevents.xml")) {
        for (const data::XmlNode& node : document.value().root.children) {
            if (!same(node.name, "SpeechEvent")) continue;
            const auto parsed = audio::parse_music_event(attribute(node, "Name"), fields_of(node));
            SpeechEvent speech;
            speech.name = parsed.name;
            speech.files = parsed.files;
            speech.volume = parsed.volume;
            speech_events_.try_emplace(lower(speech.name), std::move(speech));
        }
    } else {
        problems_.push_back("speech registry: " + core::format_diagnostic(document.error()));
    }
    std::map<tactical::TypeId, const audio::SfxEvent*> spotted;
    const audio::SfxEvent* generic_spotted = nullptr;
    if (auto faction = catalog_->resolve(local_faction, data::Category::faction)) {
        if (const auto* value = faction.value().value("SFXEvent_Generic_Unit_Spotted")) {
            data::tag_trace::used(value->value);
            generic_spotted = event(trim(value->value.raw_text), std::string(local_faction));
        }
        if (const auto* value = faction.value().value("SFXEvent_Enemy_Spotted")) {
            data::tag_trace::used(value->value);
            enemy_spotted_ = event(trim(value->value.raw_text), std::string(local_faction));
        }
        for (const auto& layer : faction.value().chain) {
            const auto* definition = catalog_->find(layer, data::Category::faction);
            if (!definition) continue;
            for (const auto& item : definition->root.children) {
                if (!same(item.name, "SFXEvent_Unit_Type_Spotted")) continue;
                data::tag_trace::used(item);
                const auto comma = item.raw_text.find(',');
                if (comma != std::string::npos) {
                    // BA-70: retain a matching blank override; it must not turn
                    // into the generic fallback by dropping an empty token.
                    const auto name = trim(std::string_view(item.raw_text).substr(0, comma));
                    if (!name.empty()) spotted.try_emplace(skirmish::type_id(name),
                        event(trim(std::string_view(item.raw_text).substr(comma + 1)), std::string(local_faction)));
                } else {
                    const auto pair = audio::split_list(item.raw_text);
                    if (pair.size() == 2) spotted.try_emplace(skirmish::type_id(pair[0]), event(pair[1], std::string(local_faction)));
                }
            }
        }
    }

    for (const units::Projectile& projectile : tables.projectiles) {
        if (auto object = catalog_->resolve(projectile.id, data::Category::game_object)) {
            projectile_detonations_.emplace(skirmish::type_id(projectile.id),
                event(tag(object.value(), "Death_SFXEvent_Start_Die"), projectile.id));
            terminal_detonations_.emplace(skirmish::type_id(projectile.id),
                event(tag(object.value(), "Projectile_SFXEvent_Detonate"), projectile.id));
        }
    }
    // WCB-01: real bomb terminals use their authored object death cue.
    for (const units::UnitType& type : tables.units) {
        if (!type.cluster_bomb) continue;
        const auto id = skirmish::type_id(type.cluster_bomb->bomb_type);
        const auto cue = projectile_detonations_.find(id);
        if (cue == projectile_detonations_.end()) continue;
        if (terminal_detonations_[id] == nullptr) terminal_detonations_[id] = cue->second;
        TypeSounds sounds;
        sounds.name = type.cluster_bomb->bomb_type;
        sounds.detonate[tactical::object_weapon] = cue->second;
        types_.try_emplace(id, std::move(sounds));
    }
    std::map<std::string, std::string> ability_projectiles;
    for (const units::UnitType& squadron : tables.units) {
        if (squadron.kind != units::UnitKind::squadron) continue;
        for (const units::Ability& ability : squadron.team_abilities) {
            if (ability.type != "ION_CANNON_SHOT" || ability.projectile_override.empty()) continue;
            for (const units::SquadronMember& member : squadron.members) {
                if (member.craft_index < tables.units.size())
                    ability_projectiles.emplace(tables.units[member.craft_index].id, ability.projectile_override);
            }
        }
    }
    binding_unit = true;
    for (const units::UnitType& type : tables.units) {
        first_type = first_types.contains(skirmish::type_id(type.id));
        // WAB-75: the droid object's arming cue comes from the ability that creates it.
        for (const auto* buzz : {&type.buzz_droids, &type.team_buzz_droids}) {
            if (!*buzz || (*buzz)->object_type.empty()) continue;
            const auto seconds = (*buzz)->activation_seconds.value_or(sim::math::Fixed{});
            const auto frames = seconds.raw() > 0
                ? static_cast<std::uint64_t>(seconds.raw()) * tactical::logical_frames_per_second
                    / static_cast<std::uint64_t>(sim::math::Fixed::scale)
                : 0U;
            droid_arming_.try_emplace(skirmish::type_id((*buzz)->object_type),
                DroidArming{event((*buzz)->activate_sfx, type.id), frames});
        }
        TypeSounds sounds;
        sounds.name = type.id;
        sounds.squadron = type.kind == units::UnitKind::squadron;
        sounds.categories = type.category_mask;
        auto object = catalog_->resolve(type.id, data::Category::game_object);
        if (!object) {
            problems_.push_back("unit " + type.id + ": " + core::format_diagnostic(object.error()));
            types_.emplace(skirmish::type_id(type.id), std::move(sounds));
            continue;
        }
        const data::EffectiveObject& value = object.value();
        const std::string where = type.id;
        // SND-40: base eligibility follows behaviour, never category or station size.
        const auto base_behavior = [](const std::string& name) {
            return same(name, "DUMMY_STAR_BASE") || same(name, "DUMMY_ORBITAL_STRUCTURE");
        };
        const auto& behavior = type.footprint.hazard;
        sounds.base = std::any_of(behavior.behavior.begin(), behavior.behavior.end(), base_behavior)
            || std::any_of(behavior.space_behavior.begin(), behavior.space_behavior.end(), base_behavior);
        sounds.community_property = type.community_property || type.station_community_property;
        if (const auto sighting = value.single("Play_SFXEvent_On_Sighting")) {
            data::tag_trace::used(sighting->value);
            sounds.announce_sighting = same(trim(sighting->value.raw_text), "true")
                || same(trim(sighting->value.raw_text), "yes") || trim(sighting->value.raw_text) == "1";
        }
        const auto spotted_type = spotted.find(skirmish::type_id(type.id));
        sounds.spotted = spotted_type == spotted.end() ? generic_spotted : spotted_type->second;
        const auto production_speech = [this, &value](const std::string_view key) {
            const auto field = value.single(key);
            if (!field) return static_cast<const SpeechEvent*>(nullptr);
            return speech_event(trim(field->value.raw_text));
        };
        sounds.build_underway_speech = production_speech("Build_Speech_Underway");
        sounds.build_completed_speech = production_speech("Build_Speech_Completed");
        sounds.build_stopped_speech = production_speech("Build_Speech_Stopped");
        const audio::SfxEvent* unit_fire = event(tag(value, "SFXEvent_Fire"), where);
        sounds.special_ready = event(tag(value, "SFXEvent_Special_Weapon_Ready"), where);
        sounds.death = event(tag(value, "Death_SFXEvent_Start_Die"), where);
        sounds.spin_death = event(tag(value, "Spin_Away_On_Death_SFXEvent_Start_Die"), where);
        sounds.asteroid_damage = event(tag(value, "SFXEvent_Damaged_By_Asteroid"), where);
        sounds.all_weapons_lost = event(tag(value, "SFXEvent_Hardpoint_All_Weapons_Destroyed"), where);
        sounds.ambient_moving = event(tag(value, "SFXEvent_Ambient_Moving"), where);
        sounds.engine_idle = event(tag(value, "SFXEvent_Engine_Idle_Loop"), where);
        sounds.engine_moving = event(tag(value, "SFXEvent_Engine_Moving_Loop"), where);
        sounds.fleet_move = event(tag(value, "SFXEvent_Fleet_Move"), where);
        sounds.self_destruct_start = event(tag(value, "Selt_Destruct_SFXEvent_Start_Die"), where); // WSD-11
        // WR-44, RP-02/RP-03: the type cues that take precedence over the faction retreat cues.
        sounds.retreat_start = event(tag(value, "SFXEvent_Retreat_Start"), where);
        sounds.retreat_cancel = event(tag(value, "SFXEvent_Retreat_Cancel"), where);
        for (auto* sound : {&sounds.engine_idle, &sounds.engine_moving}) {
            if (*sound && (!(*sound)->is_3d || (*sound)->play_count != -1)) {
                problems_.push_back(where + ": engine event must be a spatial loop");
                *sound = nullptr;
            }
        }
        // SND-46: integer seconds, positive and ordered; defaults are five and ten seconds.
        if (sounds.ambient_moving) {
            const auto delays = audio::ambient_moving_delays(tag(value, "SFXEvent_Ambient_Moving_Min_Delay_Seconds"),
                tag(value, "SFXEvent_Ambient_Moving_Max_Delay_Seconds"), where, problems_);
            if (delays) {
                sounds.ambient_min_delay = (*delays)[0];
                sounds.ambient_max_delay = (*delays)[1];
            } else sounds.ambient_moving = nullptr;
        }
        // WBP-20: local tactical lines use the generic build line only as fallback.
        sounds.build_started = event(tag(value, "SFXEvent_Tactical_Build_Started"), where);
        if (sounds.build_started == nullptr) sounds.build_started = event(tag(value, "SFXEvent_Build_Started"), where);
        sounds.build_complete = event(tag(value, "SFXEvent_Tactical_Build_Complete"), where);
        if (sounds.build_complete == nullptr) sounds.build_complete = event(tag(value, "SFXEvent_Build_Complete"), where);
        // WPR-31: explicit tactical cancellation uses the same tactical/generic fallback.
        sounds.build_cancelled = event(tag(value, "SFXEvent_Tactical_Build_Cancelled"), where);
        if (sounds.build_cancelled == nullptr) sounds.build_cancelled = event(tag(value, "SFXEvent_Build_Cancelled"), where);
        sounds.sold = event(tag(value, "SFXEvent_Sold_Tactical"), where);
        sounds.select = event(tag(value, "SFXEvent_Select"), where);
        sounds.move = event(tag(value, "SFXEvent_Move"), where);
        sounds.attack = event(tag(value, "SFXEvent_Attack"), where);
        sounds.group_move = event(tag(value, "SFXEvent_Group_Move"), where);
        sounds.group_attack = event(tag(value, "SFXEvent_Group_Attack"), where);
        sounds.assist_move = event(tag(value, "SFXEvent_Assist_Move"), where);
        sounds.assist_attack = event(tag(value, "SFXEvent_Assist_Attack"), where);
        sounds.move_asteroid = event(tag(value, "SFXEvent_Move_Into_Asteroid_Field"), where);
        sounds.move_nebula = event(tag(value, "SFXEvent_Move_Into_Nebula"), where);
        sounds.stop = event(tag(value, "SFXEvent_Stop"), where);
        sounds.guard = event(tag(value, "SFXEvent_Guard"), where);
        // BA-25: repeated hardpoint-kind/event pairs, with derived entries replacing that kind.
        for (auto layer = value.chain.rbegin(); layer != value.chain.rend(); ++layer) {
            const data::Definition* definition = catalog_->find(*layer, data::Category::game_object);
            if (definition == nullptr) continue;
            for (const data::XmlNode& item : definition->root.children) {
                if (same(item.name, "SFXEvent_Hardpoint_Destroyed")) {
                    data::tag_trace::used(item);
                    audio::read_hardpoint_loss(item.raw_text, sounds.hardpoint_loss,
                        [&event, &where](const std::string& name) { return event(name, where); });
                }
                if (!same(item.name, "SFXEvent_Attack_Hardpoint")) continue;
                data::tag_trace::used(item);
                const auto pair = audio::split_list(item.raw_text);
                if (pair.size() == 2) sounds.attack_hardpoint[lower(pair[0])] = event(pair[1], where);
            }
        }
        if (const auto ranking = audio::leading_integer(tag(value, "Ranking_In_Category"))) sounds.ranking = *ranking;
        // BA-52: each modelled ability's voice lines, ION_CANNON_SHOT (#561) included. The ability kinds
        // the simulation does not have play nothing.
        if (const data::EffectiveValue* abilities = value.value("Unit_Abilities_Data")) {
            for (const data::XmlNode& ability : abilities->value.children) {
                if (!same(ability.name, "Unit_Ability")) continue;
                if (same(child(ability, "Type"), "EJECT_VEHICLE_THIEF")) sounds.vehicle_thief = true;
                const tactical::AbilityKind kind = tactical::ability_kind(child(ability, "Type"));
                if (kind == tactical::AbilityKind::none) continue;
                sounds.ability_voices[kind] = {event(child(ability, "SFXEvent_GUI_Unit_Ability_Activated"), where),
                                               event(child(ability, "SFXEvent_GUI_Unit_Ability_Deactivated"), where)};
                // BA-53/54: targeting acknowledgement and successful spawn use the source object.
                sounds.ability_targets[kind] = event(child(ability, "SFXEvent_Target_Ability"), where);
                if (kind == tactical::AbilityKind::leech_shields)
                    sounds.leech_loop = event(child(ability, "SFXEvent_Special_Ability_Loop"), where);
            }
        }
        // WCB-02: the named handler's launch cue is distinct from the button voice.
        if (type.cluster_bomb) {
            if (const auto* handlers = value.value("Abilities")) for (const auto& handler : handlers->value.children) {
                if (!same(handler.name, "Cluster_Bomb_Ability")
                    || !same(attribute(handler, "Name"), type.cluster_bomb->name)) continue;
                for (const auto& field : handler.children)
                    if (same(field.name, "SFXEvent_Activate")) data::tag_trace::used(field);
                sounds.ability_targets[tactical::AbilityKind::cluster_bomb] = event(child(handler, "SFXEvent_Activate"), where);
            }
        }
        for (const auto& nested : type.special_abilities) {
            if (nested.kind == tactical::SpecialAbilityKind::blast && nested.charged_blast)
                sounds.blast_activate = event(nested.charged_blast->activate_sfx, where);
            sounds.heal_pulses.push_back(nested.kind == tactical::SpecialAbilityKind::force_healing
                ? event(nested.heal_sfx, where) : nullptr);
            if (nested.kind == tactical::SpecialAbilityKind::corrupt_systems && !nested.corrupt_activate_sfx.empty())
                sounds.corrupt_activate = event(nested.corrupt_activate_sfx, where);
            if (nested.kind == tactical::SpecialAbilityKind::laser_defense && !nested.defence_zap_sfx.empty())
                sounds.point_defence_zap = event(nested.defence_zap_sfx, where);
            if (nested.kind == tactical::SpecialAbilityKind::redirect_blaster && !nested.redirect_activate_sfx.empty()
                && sounds.redirect_block == nullptr)
                sounds.redirect_block = event(nested.redirect_activate_sfx, where);
        }
        const auto projectile_events = [&](const std::string& projectile, const std::uint32_t slot,
                                            const tactical::ProjectileProfileKind kind = tactical::ProjectileProfileKind::ordinary) {
            auto shot = catalog_->resolve(projectile, data::Category::game_object);
            if (!shot) {
                problems_.push_back("projectile " + projectile + ": " + core::format_diagnostic(shot.error()));
                return;
            }
            auto& plain = kind == tactical::ProjectileProfileKind::ability ? sounds.ability_detonate
                : kind == tactical::ProjectileProfileKind::barrage ? sounds.barrage_detonate
                : kind == tactical::ProjectileProfileKind::charged_blast ? sounds.charged_detonate : sounds.detonate;
            auto& armor = kind == tactical::ProjectileProfileKind::ability ? sounds.ability_detonate_armor
                : kind == tactical::ProjectileProfileKind::barrage ? sounds.barrage_detonate_armor
                : kind == tactical::ProjectileProfileKind::charged_blast ? sounds.charged_detonate_armor : sounds.detonate_armor;
            plain[slot] = event(tag(shot.value(), "Projectile_SFXEvent_Detonate"), projectile);
            armor[slot] = event(tag(shot.value(), "Projectile_SFXEvent_Detonate_Reduced_By_Armor"), projectile);
        };
        for (std::size_t index = 0; index < type.hardpoints.size(); ++index) {
            const units::Hardpoint& hardpoint = type.hardpoints[index];
            sounds.hardpoint_types.push_back(lower(hardpoint.type_name));
            const auto slot = static_cast<std::uint32_t>(index);
            auto point = catalog_->resolve(hardpoint.id, data::Category::hardpoint);
            const audio::SfxEvent* death = nullptr;
            if (point) {
                // BA-14: the hardpoint's Fire_SFXEvent, else its parent's SFXEvent_Fire.
                if (hardpoint.weapon && !hardpoint.weapon->projectile.empty()) {
                    const audio::SfxEvent* own = event(tag(point.value(), "Fire_SFXEvent"), hardpoint.id);
                    sounds.fire[slot] = own != nullptr ? own : unit_fire;
                    projectile_events(hardpoint.weapon->projectile, slot);
                    if (!hardpoint.weapon->charged_projectile.empty())
                        projectile_events(hardpoint.weapon->charged_projectile, slot, tactical::ProjectileProfileKind::charged_blast);
                }
                // BA-17: Death_Explosion_SFXEvent, else the breakoff prop's Death_SFXEvent_Start_Die.
                death = event(tag(point.value(), "Death_Explosion_SFXEvent"), hardpoint.id);
                const std::string prop = tag(point.value(), "Death_Breakoff_Prop");
                if (death == nullptr && !prop.empty()) {
                    if (auto prop_object = catalog_->resolve(prop, data::Category::game_object)) {
                        death = event(tag(prop_object.value(), "Death_SFXEvent_Start_Die"), prop);
                    }
                }
            } else {
                problems_.push_back("hardpoint " + hardpoint.id + ": " + core::format_diagnostic(point.error()));
            }
            sounds.hardpoint_deaths.push_back(death);
        }
        if (type.weapon && !type.weapon->projectile.empty()) {
            sounds.fire[tactical::object_weapon] = unit_fire;
            projectile_events(type.weapon->projectile, tactical::object_weapon);
        }
        for (std::size_t index = 0; index < type.death_projectiles.size(); ++index)
            projectile_events(type.death_projectiles[index],
                tactical::death_projectile_slot_flag | static_cast<std::uint32_t>(index));
        if (!type.retreat_self_destruct.empty()) projectile_events(type.retreat_self_destruct, tactical::retreat_explosion_slot_flag);
        sounds.hardpoint_points.assign(type.hardpoints.size(), sim::math::Vec3{});
        if (const tactical::CombatProfile* profile = combat.find(skirmish::type_id(type.id))) {
            for (const tactical::WeaponProfile& weapon : profile->weapons) {
                if (weapon.ability_shot) {
                    if (const auto override = ability_projectiles.find(type.id); override != ability_projectiles.end())
                        projectile_events(override->second, weapon.hardpoint, tactical::ProjectileProfileKind::ability);
                }
                if (weapon.barrage_shot) for (const units::Ability& ability : type.abilities) {
                    if (tactical::ability_kind(ability.type) == tactical::AbilityKind::barrage && !ability.projectile_override.empty())
                        projectile_events(ability.projectile_override, weapon.hardpoint, tactical::ProjectileProfileKind::barrage);
                }
            }
            for (const tactical::TargetHardpoint& point : profile->hardpoints) {
                if (point.hardpoint < sounds.hardpoint_points.size()) sounds.hardpoint_points[point.hardpoint] = point.position;
            }
        }
        types_.emplace(skirmish::type_id(type.id), std::move(sounds));
    }
    start_rows_.reserve(256);
    binding_unit = false;
    // WST-14: the debug build's stealth service starts this event by name.
    stealth_reveal_ = event("SFX_Stealth_Reveal", "stealth reveal");
    static_cast<void>(event("GUI_Text_Hint_SFX", "local Begin"));
    std::set<std::string> visited, scheduled_samples;
    const auto warm = [&](std::set<std::string> roots) {
        while (!roots.empty()) {
            const std::string name = *roots.begin();
            roots.erase(roots.begin());
            const audio::SfxEvent* cue = registry_.find(name);
            if (!cue || !visited.insert(audio::upper(cue->name)).second) continue;
            for (const auto* stage : {&cue->pre_samples, &cue->samples, &cue->post_samples}) {
                for (const auto& file : *stage) {
                    if (!scheduled_samples.insert(file).second) continue;
                    static_cast<void>(sample(file));
                }
            }
            if (!cue->chained.empty()) roots.insert(cue->chained);
        }
    };
    // Warm only global and initial-force spatial cues. Other sounds retain
    // synchronous first-use loading rather than decoding unused PCM.
    warm(std::move(immediate_roots));
    for (const auto& [name, cached] : samples_) {
        static_cast<void>(cached);
        preloaded_samples_.insert(name);
    }
    samples_prepared_ = true;
}

namespace {

[[nodiscard]] godot::Ref<godot::AudioStreamWAV> wav_stream(const audio::WavPcm& pcm) {
    godot::PackedByteArray data;
    data.resize(static_cast<std::int64_t>(pcm.data.size()));
    std::memcpy(data.ptrw(), pcm.data.data(), pcm.data.size());
    godot::Ref<godot::AudioStreamWAV> stream;
    stream.instantiate();
    stream->set_format(godot::AudioStreamWAV::FORMAT_16_BITS);
    stream->set_mix_rate(static_cast<std::int32_t>(pcm.sample_rate));
    stream->set_stereo(pcm.channels == 2);
    stream->set_data(data);
    return stream;
}

[[nodiscard]] godot::Ref<godot::AudioStreamMP3> mp3_stream(const std::span<const std::byte> bytes) {
    godot::PackedByteArray data;
    data.resize(static_cast<std::int64_t>(bytes.size()));
    std::memcpy(data.ptrw(), bytes.data(), bytes.size());
    godot::Ref<godot::AudioStreamMP3> stream;
    stream.instantiate();
    stream->set_data(data);
    stream->set_loop(false);
    return stream;
}

} // namespace

// Speech and music streams: the original adds a fixed directory to the event's file name and
// lets its decoder tell WAVE from MP3 by content (#1708), so a WAVE named `.mp3` plays too.
godot::Ref<godot::AudioStream> BattleAudio::open_stream(const std::string& path, const std::string& key,
                                                        const std::string_view kind) {
    godot::Ref<godot::AudioStream> result;
    auto bytes = filesystem_->open(path);
    if (!bytes) {
        missing_samples_[key] = "missing: " + std::string(kind) + " " + core::format_diagnostic(bytes.error());
        return result;
    }
    std::string error;
    result = audio::decode_stream(bytes.value(), error,
        [](const audio::WavPcm& pcm) -> godot::Ref<godot::AudioStream> { return wav_stream(pcm); },
        [](const std::span<const std::byte> data) -> godot::Ref<godot::AudioStream> {
            auto stream = mp3_stream(data);
            return stream->get_length() > 0.0 ? godot::Ref<godot::AudioStream>(stream) : godot::Ref<godot::AudioStream>{};
        });
    if (result.is_null()) missing_samples_[key] = "undecodable: "
        + (error.empty() ? "Godot's MP3 decoder read no audio" : error);
    return result;
}

const BattleAudio::SpeechEvent* BattleAudio::speech_event(const std::string_view name) {
    if (name.empty()) return nullptr;
    const auto found = speech_events_.find(lower(name));
    if (found == speech_events_.end()) {
        missing_events_.insert("speech: " + std::string(name));
        return nullptr;
    }
    auto& speech = found->second;
    if (!speech.streams.empty()) return &speech;
    // Preload only reachable speech at preparation. The current viewer language
    // is English; SND-62 requires an explicit language route before adding others.
    speech.streams.reserve(speech.files.size());
    for (const auto& file : speech.files) {
        speech.streams.push_back(open_stream(audio::speech_sample_path("english", file), file, "speech"));
    }
    return &speech;
}

BattleAudio::DecodedSample BattleAudio::decode_sample(const std::string& name) const {
    DecodedSample decoded;
    // The original opens the name as written (#1708); see audio::sfx_sample_candidates.
    const auto bytes = audio::open_sfx_sample(*filesystem_, name);
    if (!bytes) {
        decoded.cause = "missing: " + core::format_diagnostic(bytes.error());
        return decoded;
    }
    std::string error;
    const auto pcm = audio::parse_wav(bytes.value(), error);
    if (!pcm) {
        decoded.cause = "undecodable: " + error;
        return decoded;
    }
    decoded.rate = pcm->sample_rate;
    decoded.channels = pcm->channels;
    decoded.pcm.assign(pcm->data.begin(), pcm->data.end());
    return decoded;
}

const BattleAudio::Sample& BattleAudio::sample(const std::string& name) {
    core::load_profile::Scope sample_scope(core::load_profile::Phase::audio);
    const auto found = samples_.find(name);
    if (found != samples_.end()) return found->second;
    if (samples_prepared_) ++synchronous_runtime_loads_;
    auto decoded = decode_sample(name);
    Sample sample;
    sample.cause = std::move(decoded.cause);
    if (sample.cause.empty()) {
        audio::WavPcm pcm(decoded.rate, decoded.channels);
        pcm.converted = std::move(decoded.pcm);
        pcm.data = pcm.converted;
        sample.stream = wav_stream(pcm);
    }
    if (!sample.stream.is_valid()) missing_samples_[name] = sample.cause;
    return samples_.emplace(name, std::move(sample)).first->second;
}

godot::Ref<godot::AudioStream> BattleAudio::music_stream(const std::string& file) {
    core::load_profile::Scope music_scope(core::load_profile::Phase::audio);
    const auto found = music_streams_.find(file);
    if (found != music_streams_.end()) return found->second;
    auto result = open_stream(audio::music_sample_path(file), file, "music");
    music_streams_.emplace(file, result);
    return result;
}

} // namespace eawr::presentation::godot_backend

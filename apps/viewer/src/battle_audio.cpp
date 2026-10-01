#include "battle_audio.hpp"
#include "audio_output.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/skirmish/start.hpp"

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
#include <utility>

namespace eawr::presentation::godot_backend {
namespace {

namespace tactical = sim::tactical;

// The report keeps this many music and response rows.
constexpr std::size_t log_limit = 256;
// How long a voice asked to play counts as playing before its player reports it (a few physics steps).
constexpr double start_grace_seconds = 0.1;
// BA-51: a timed ability whose last read had at most this many frames left ended by itself.
constexpr std::uint32_t natural_end_frames = 2;

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

[[nodiscard]] bool same(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
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

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] audio::Vec3 vec(const sim::math::Vec3& value) {
    return {to_double(value.x), to_double(value.y), to_double(value.z)};
}

// Source (Z up) to Godot (Y up): (x, z, -y), the inverse of space::source_from_render.
[[nodiscard]] godot::Vector3 render(const audio::Vec3& source) {
    return {static_cast<float>(source[0]), static_cast<float>(source[2]), static_cast<float>(-source[1])};
}

[[nodiscard]] float decibels(const double linear) {
    return linear <= 1.0e-4 ? -80.0F : static_cast<float>(20.0 * std::log10(linear));
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

BattleAudio::BattleAudio(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog,
                         const Options options)
    : host_(&host), filesystem_(&filesystem), catalog_(&catalog), options_(options) {
    // #443: Godot's 3D players hear only through a Camera3D of their world, and the renderer draws
    // with a bare camera of its own, so the listener lives in a never-drawn viewport on the same
    // world with a camera that makes it a listener (without one every 3D sound mixed to silence).
    listener_viewport_ = memnew(godot::SubViewport);
    listener_viewport_->set_size(godot::Vector2i(2, 2));
    listener_viewport_->set_update_mode(godot::SubViewport::UPDATE_DISABLED);
    listener_viewport_->set_as_audio_listener_3d(true);
    listener_camera_ = memnew(godot::Camera3D);
    listener_viewport_->add_child(listener_camera_);
    listener_ = memnew(godot::AudioListener3D);
    listener_viewport_->add_child(listener_);
    host_->add_child(listener_viewport_);
    // BA-45: FoC's volume sliders at their defaults, 0.75 each, a channel's level its slider times
    // the master's. The SFX (3D), the unit responses (2D) and the music play on their own buses
    // at the channel slider into a mix bus at the master slider, which feeds Master; the report
    // meters each (a muted Master still meters the buses that feed it).
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    buses_ = {BusLevel{.name = "EAWR_SFX"}, BusLevel{.name = "EAWR_Voice"}, BusLevel{.name = "EAWR_Music"},
              BusLevel{.name = "EAWR_Mix"}};
    const auto open_bus = [server](BusLevel& bus, const char* send, const bool limit) {
        bus.index = server->get_bus_index(bus.name);
        if (bus.index < 0) {
            bus.index = server->get_bus_count();
            server->add_bus();
            server->set_bus_name(bus.index, bus.name);
        }
        server->set_bus_send(bus.index, send);
        server->set_bus_volume_db(bus.index, decibels(audio::default_volume_slider));
        while (server->get_bus_effect_count(bus.index) > 0) server->remove_bus_effect(bus.index, 0);
        if (limit) {
            // A viewer choice, not FoC's (BA-45): a hard limiter just under full scale keeps the
            // rare peaks of many shots over loud music from clipping the output.
            godot::Ref<godot::AudioEffectHardLimiter> limiter;
            limiter.instantiate();
            limiter->set_ceiling_db(-0.3F);
            server->add_bus_effect(bus.index, limiter);
        }
        // The report's RMS: the bus's signal after its effects, before its own slider.
        bus.capture.instantiate();
        bus.capture->set_buffer_length(0.5F);
        server->add_bus_effect(bus.index, bus.capture);
    };
    // A bus sends only to a bus before it: the mix first.
    open_bus(buses_[3], "Master", true);
    for (std::size_t index = 0; index < 3; ++index) open_bus(buses_[index], buses_[3].name, false);
    for (std::size_t index = 0; index < audio::Voices::voices_3d; ++index) {
        auto* player = memnew(godot::AudioStreamPlayer3D);
        // BA-11: the falloff is FoC's, applied as the player's volume; Godot only pans.
        player->set_attenuation_model(godot::AudioStreamPlayer3D::ATTENUATION_DISABLED);
        player->set_max_distance(0.0F);
        player->set_attenuation_filter_cutoff_hz(20500.0F);
        player->set_attenuation_filter_db(0.0F);
        player->set_doppler_tracking(godot::AudioStreamPlayer3D::DOPPLER_TRACKING_DISABLED);
        player->set_bus(buses_[0].name);
        host_->add_child(player);
        players_3d_.push_back(player);
    }
    for (std::size_t index = 0; index < audio::Voices::voices_2d; ++index) {
        auto* player = memnew(godot::AudioStreamPlayer);
        player->set_bus(buses_[1].name);
        host_->add_child(player);
        players_2d_.push_back(player);
    }
    for (MusicTrack& track : music_tracks_) {
        track.player = memnew(godot::AudioStreamPlayer);
        track.player->set_bus(buses_[2].name);
        host_->add_child(track.player);
    }
    // The viewer has no other sound: muting the master bus mutes the battle.
    options_.muted = options_.muted || audio_output_muted();
    godot::AudioServer::get_singleton()->set_bus_mute(0, options_.muted);
}

BattleAudio::~BattleAudio() { release(); }

const audio::SfxEvent* BattleAudio::event(const std::string_view name, const std::string& where) {
    const std::string text = trim(name);
    if (text.empty()) return nullptr;
    const audio::SfxEvent* found = registry_.find(text);
    if (found == nullptr) missing_events_.insert(where + ": " + text);
    return found;
}

void BattleAudio::prepare(const units::UnitTables& tables, const tactical::CombatTable& combat,
                          const std::string_view local_faction) {
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
    if (auto document = data::load_document(*filesystem_, "data/xml/audio.xml")) {
        const data::XmlNode& root = document.value().root;
        if (const auto value = number(child(root, "Audio_Space_3D_Saturation_Distance_Mod"))) space_.saturation_factor = *value;
        if (const auto value = number(child(root, "Audio_Space_3D_Rolloff_Distance_Mod"))) space_.rolloff_factor = *value;
        if (const auto value = number(child(root, "Audio_Space_3D_Listener_Z_Pullback_Dist"))) space_.listener_z = *value;
        if (const auto value = number(child(root, "Music_Space_Battle_To_Ambient_Peace_Seconds"))) peace_seconds = *value;
    } else {
        problems_.push_back("data/xml/audio.xml: " + core::format_diagnostic(document.error()));
    }
    // BA-20: the command rankings.
    if (auto document = data::load_document(*filesystem_, "data/xml/gameconstants.xml")) {
        rankings_ = audio::split_list(child(document.value().root, "Unit_Command_Rankings_By_Category"));
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
    std::vector<const audio::MusicEvent*> battle;
    // The lists repeat their tag, which the effective object merges to one value: read the
    // faction's own definition.
    if (const data::Definition* faction = catalog_->find(local_faction); faction != nullptr) {
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
    music_ = std::make_unique<audio::MusicDirector>(
        std::move(ambient), std::move(battle),
        static_cast<std::uint64_t>(std::llround(peace_seconds * tactical::logical_frames_per_second)));

    for (const units::UnitType& type : tables.units) {
        TypeSounds sounds;
        sounds.name = type.id;
        sounds.categories = type.category_mask;
        auto object = catalog_->resolve(type.id);
        if (!object) {
            problems_.push_back("unit " + type.id + ": " + core::format_diagnostic(object.error()));
            types_.emplace(skirmish::type_id(type.id), std::move(sounds));
            continue;
        }
        const data::EffectiveObject& value = object.value();
        const std::string where = type.id;
        const audio::SfxEvent* unit_fire = event(tag(value, "SFXEvent_Fire"), where);
        sounds.death = event(tag(value, "Death_SFXEvent_Start_Die"), where);
        sounds.select = event(tag(value, "SFXEvent_Select"), where);
        sounds.move = event(tag(value, "SFXEvent_Move"), where);
        sounds.attack = event(tag(value, "SFXEvent_Attack"), where);
        sounds.group_move = event(tag(value, "SFXEvent_Group_Move"), where);
        sounds.group_attack = event(tag(value, "SFXEvent_Group_Attack"), where);
        if (const auto ranking = audio::leading_integer(tag(value, "Ranking_In_Category"))) sounds.ranking = *ranking;
        // BA-52: each modelled ability's voice lines, ION_CANNON_SHOT (#561) included. The ability kinds
        // the simulation does not have (HUNT, AB-03) play nothing.
        if (const data::EffectiveValue* abilities = value.value("Unit_Abilities_Data")) {
            for (const data::XmlNode& ability : abilities->value.children) {
                if (!same(ability.name, "Unit_Ability")) continue;
                const tactical::AbilityKind kind = tactical::ability_kind(child(ability, "Type"));
                if (kind == tactical::AbilityKind::none) continue;
                sounds.ability_voices[kind] = {event(child(ability, "SFXEvent_GUI_Unit_Ability_Activated"), where),
                                               event(child(ability, "SFXEvent_GUI_Unit_Ability_Deactivated"), where)};
            }
        }
        const auto projectile_events = [&](const std::string& projectile, const std::uint32_t slot) {
            auto shot = catalog_->resolve(projectile);
            if (!shot) {
                problems_.push_back("projectile " + projectile + ": " + core::format_diagnostic(shot.error()));
                return;
            }
            sounds.detonate[slot] = event(tag(shot.value(), "Projectile_SFXEvent_Detonate"), projectile);
            sounds.detonate_armor[slot] = event(tag(shot.value(), "Projectile_SFXEvent_Detonate_Reduced_By_Armor"), projectile);
        };
        for (std::size_t index = 0; index < type.hardpoints.size(); ++index) {
            const units::Hardpoint& hardpoint = type.hardpoints[index];
            const auto slot = static_cast<std::uint32_t>(index);
            auto point = catalog_->resolve(hardpoint.id);
            const audio::SfxEvent* death = nullptr;
            if (point) {
                // BA-14: the hardpoint's Fire_SFXEvent, else its parent's SFXEvent_Fire.
                if (hardpoint.weapon && !hardpoint.weapon->projectile.empty()) {
                    const audio::SfxEvent* own = event(tag(point.value(), "Fire_SFXEvent"), hardpoint.id);
                    sounds.fire[slot] = own != nullptr ? own : unit_fire;
                    projectile_events(hardpoint.weapon->projectile, slot);
                }
                // BA-17: Death_Explosion_SFXEvent, else the breakoff prop's Death_SFXEvent_Start_Die.
                death = event(tag(point.value(), "Death_Explosion_SFXEvent"), hardpoint.id);
                const std::string prop = tag(point.value(), "Death_Breakoff_Prop");
                if (death == nullptr && !prop.empty()) {
                    if (auto prop_object = catalog_->resolve(prop)) {
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
        sounds.hardpoint_points.assign(type.hardpoints.size(), sim::math::Vec3{});
        if (const tactical::CombatProfile* profile = combat.find(skirmish::type_id(type.id))) {
            for (const tactical::TargetHardpoint& point : profile->hardpoints) {
                if (point.hardpoint < sounds.hardpoint_points.size()) sounds.hardpoint_points[point.hardpoint] = point.position;
            }
        }
        types_.emplace(skirmish::type_id(type.id), std::move(sounds));
    }
}

const BattleAudio::Sample& BattleAudio::sample(const std::string& name) {
    auto found = samples_.find(name);
    if (found != samples_.end()) return found->second;
    Sample sample;
    // BA-30: every FoC SFX sample is a 16-bit PCM WAVE under Data/Audio/SFX.
    const std::string path = "data/audio/sfx/" + lower(name);
    auto bytes = filesystem_->open(path);
    if (!bytes) {
        sample.cause = "missing: " + core::format_diagnostic(bytes.error());
    } else {
        std::string error;
        const auto pcm = audio::parse_wav(bytes.value(), error);
        if (!pcm) {
            sample.cause = "undecodable: " + error;
        } else {
            godot::PackedByteArray data;
            data.resize(static_cast<std::int64_t>(pcm->data.size()));
            std::memcpy(data.ptrw(), pcm->data.data(), pcm->data.size());
            godot::Ref<godot::AudioStreamWAV> stream;
            stream.instantiate();
            stream->set_format(godot::AudioStreamWAV::FORMAT_16_BITS);
            stream->set_mix_rate(static_cast<std::int32_t>(pcm->sample_rate));
            stream->set_stereo(pcm->channels == 2);
            stream->set_data(data);
            sample.stream = stream;
        }
    }
    if (!sample.stream.is_valid()) missing_samples_[name] = sample.cause;
    return samples_.emplace(name, std::move(sample)).first->second;
}

void BattleAudio::play(const audio::SfxEvent* sfx, const std::optional<audio::Vec3> position, const bool hidden,
                       const std::string& reason) {
    if (sfx == nullptr) return;
    ++requested_[reason + ":" + sfx->name];
    const audio::Vec3 listener = listener_position_;
    const audio::Start start = voices_.start({sfx, position, hidden}, listener, random_);
    ++results_[reason][std::string(audio::to_string(start.result))];
    if (start.result != audio::Start::Result::playing) return;
    if (start.stopped) {
        ++stolen_;
        if (audio::Voices::is_3d_voice(*start.stopped)) players_3d_[*start.stopped]->stop();
        else players_2d_[*start.stopped - audio::Voices::voices_3d]->stop();
        end_voice(*start.stopped);
    }
    const Sample& loaded = sample(start.sample);
    if (!loaded.stream.is_valid()) {
        // Listed under missing_samples; the voice is free again at once.
        ++results_[reason]["sample_missing"];
        voices_.finished(start.voice);
        return;
    }
    ++played_samples_[start.sample];
    if (reason.starts_with("ability_") && start_rows_.size() < log_limit) {
        start_rows_.push_back({reason, sfx->name, start.sample, presented_tick_, start.volume, start.pitch});
    }
    VoiceState& state = voice_states_[start.voice];
    state = {start.volume, position.value_or(audio::Vec3{}), true, frames_, clock_};
    if (audio::Voices::is_3d_voice(start.voice)) {
        godot::AudioStreamPlayer3D* player = players_3d_[start.voice];
        player->set_stream(loaded.stream);
        player->set_position(render(state.position));
        player->set_pitch_scale(static_cast<float>(start.pitch));
        const double distance = std::sqrt(std::pow(state.position[0] - listener[0], 2.0)
                                          + std::pow(state.position[1] - listener[1], 2.0)
                                          + std::pow(state.position[2] - listener[2], 2.0));
        const float gain_db = decibels(state.volume * audio::falloff_gain(distance, sfx->saturation_distance, space_));
        start_distance_[reason].add(distance);
        start_gain_db_[reason].add(gain_db);
        player->set_volume_db(gain_db);
        player->play();
    } else {
        godot::AudioStreamPlayer* player = players_2d_[start.voice - audio::Voices::voices_3d];
        player->set_stream(loaded.stream);
        player->set_pitch_scale(static_cast<float>(start.pitch));
        player->set_volume_db(decibels(state.volume));
        player->play();
    }
    max_voices_ = std::max<std::uint64_t>(max_voices_, voices_.playing_count());
}

void BattleAudio::update_voices(const audio::Vec3& listener) {
    for (std::size_t voice = 0; voice < voice_states_.size(); ++voice) {
        VoiceState& state = voice_states_[voice];
        if (!state.started) continue;
        const bool three_d = audio::Voices::is_3d_voice(voice);
        const bool playing = three_d ? players_3d_[voice]->is_playing()
                                     : players_2d_[voice - audio::Voices::voices_3d]->is_playing();
        // Godot starts a player's stream on its next physics step, so a player asked to play is
        // not playing yet for a frame or two (#443: freeing its voice then let the next sound cut it).
        if (!playing && clock_ - state.started_at > start_grace_seconds) {
            voices_.finished(voice);
            end_voice(voice);
            continue;
        }
        if (!three_d) continue;
        const audio::SfxEvent* sfx = voices_.playing(voice);
        if (sfx == nullptr) continue;
        const double distance = std::sqrt(std::pow(state.position[0] - listener[0], 2.0)
                                          + std::pow(state.position[1] - listener[1], 2.0)
                                          + std::pow(state.position[2] - listener[2], 2.0));
        players_3d_[voice]->set_volume_db(decibels(state.volume * audio::falloff_gain(distance, sfx->saturation_distance, space_)));
    }
}

godot::Ref<godot::AudioStream> BattleAudio::music_stream(const std::string& file) {
    const auto found = music_streams_.find(file);
    if (found != music_streams_.end()) return found->second;
    godot::Ref<godot::AudioStream> result;
    const std::string path = "data/audio/music/" + lower(file);
    if (auto bytes = filesystem_->open(path)) {
        godot::PackedByteArray data;
        data.resize(static_cast<std::int64_t>(bytes.value().size()));
        std::memcpy(data.ptrw(), bytes.value().data(), bytes.value().size());
        godot::Ref<godot::AudioStreamMP3> stream;
        stream.instantiate();
        stream->set_data(data);
        stream->set_loop(false);
        if (stream->get_length() > 0.0) result = stream;
        else missing_samples_[file] = "undecodable: Godot's MP3 decoder read no audio";
    } else {
        missing_samples_[file] = "missing: " + core::format_diagnostic(bytes.error());
    }
    music_streams_.emplace(file, result);
    return result;
}

void BattleAudio::cue_music(const audio::MusicDirector::Cue& cue) {
    const char* mode = cue.mode == audio::MusicDirector::Mode::battle ? "battle" : "ambient";
    if (music_log_.size() < log_limit) {
        music_log_.push_back(std::to_string(music_tick_) + " " + mode + " " + cue.event->name + " " + cue.file);
    }
    const godot::Ref<godot::AudioStream> stream = music_stream(cue.file);
    if (!stream.is_valid()) return;
    // BA-44: the previous track fades out over the new event's Fade_Out_Previous_Seconds while the
    // new one fades in over its Fade_In_Seconds.
    MusicTrack& previous = music_tracks_[music_current_];
    if (previous.playing) {
        previous.fading_out = true;
        previous.fade_seconds = cue.event->fade_out_previous_seconds;
    }
    music_current_ = 1 - music_current_;
    MusicTrack& next = music_tracks_[music_current_];
    next.player->stop();
    next.volume = cue.event->volume;
    next.fade_seconds = cue.event->fade_in_seconds;
    next.level = next.fade_seconds > 0.0 ? 0.0 : 1.0;
    next.fading_out = false;
    next.playing = true;
    next.player->set_stream(stream);
    next.player->set_volume_db(decibels(next.volume * next.level));
    next.player->play();
}

void BattleAudio::update_music(const double delta) {
    for (std::size_t index = 0; index < music_tracks_.size(); ++index) {
        MusicTrack& track = music_tracks_[index];
        if (!track.playing) continue;
        const double step = track.fade_seconds > 0.0 ? delta / track.fade_seconds : 1.0;
        if (track.fading_out) {
            track.level = std::max(0.0, track.level - step);
            if (track.level <= 0.0) {
                track.player->stop();
                track.playing = false;
                continue;
            }
        } else {
            track.level = std::min(1.0, track.level + step);
        }
        track.player->set_volume_db(decibels(track.volume * track.level));
        if (index == music_current_ && !track.fading_out && !track.player->is_playing()) {
            track.playing = false;
            if (music_) {
                if (const auto cue = music_->track_ended()) cue_music(*cue);
            }
        }
    }
}

void BattleAudio::respond(const BattleInput::Acknowledgement& acknowledgement, const LiveSessionView& live) {
    // BA-20: the highest ranking selected unit speaks for the selection. A squadron's team
    // container has no response sounds or ranking fields of its own (BA-24): FoC resolves it to
    // its leading live craft, in roster order, for both the ranking category and the sound played
    // (debug build). A craft that has since died is skipped: it stays in roster order (BA-24 does
    // not track FoC's own leader reassignment), but `live_entities_` excludes it once it leaves the
    // tactical snapshot, ahead of the destruction event that would otherwise age it out of
    // `last_seen_` a tick or more later.
    std::vector<audio::RankedUnit> ranked;
    std::vector<const TypeSounds*> sounds;
    for (const sim::EntityId entity : acknowledgement.units) {
        const auto seen = last_seen_.find(entity);
        if (seen == last_seen_.end() || seen->second.owner != live.local_player()) continue;
        sim::tactical::TypeId type_id = seen->second.type;
        const auto members = live.squadron_members().find(entity);
        if (members != live.squadron_members().end()) {
            for (const sim::EntityId craft : members->second) {
                if (!live_entities_.contains(craft)) continue;
                const auto craft_type = entity_types_.find(craft);
                if (craft_type != entity_types_.end()) {
                    type_id = craft_type->second;
                    break;
                }
            }
        }
        const auto type = types_.find(type_id);
        if (type == types_.end()) continue;
        ranked.push_back({sounds.size(), type->second.categories, type->second.ranking});
        sounds.push_back(&type->second);
    }
    const auto chosen = audio::speaker(ranked, rankings_);
    if (!chosen) return;
    const TypeSounds& speaker = *sounds[*chosen];
    const bool group = acknowledgement.units.size() > 1;
    const audio::SfxEvent* line = nullptr;
    const char* kind = "select";
    switch (acknowledgement.kind) {
    case BattleInput::Acknowledgement::Kind::select:
        line = speaker.select;
        break;
    case BattleInput::Acknowledgement::Kind::move:
        // BA-22: Group_Move for more than one selected unit when the type has one.
        line = group && speaker.group_move != nullptr ? speaker.group_move : speaker.move;
        kind = "move";
        break;
    case BattleInput::Acknowledgement::Kind::attack:
        line = group && speaker.group_attack != nullptr ? speaker.group_attack : speaker.attack;
        kind = "attack";
        break;
    }
    if (response_log_.size() < log_limit) {
        response_log_.push_back(std::string(kind) + " " + speaker.name + " " + (line ? line->name : std::string("<none>")));
    }
    play(line, std::nullopt, false, std::string("response_") + kind);
}

const std::map<tactical::AbilityKind, BattleAudio::Toggle>& BattleAudio::toggles_of(const std::string& faction) {
    const auto found = toggles_.find(faction);
    if (found != toggles_.end()) return found->second;
    std::map<tactical::AbilityKind, Toggle> table;
    // FACTIONS.XML lists one "<ability>, <event>" entry per ability, the tag repeated; the effective
    // object merges the repeats to one value, so the faction's own definition is read (as the music is).
    if (const data::Definition* definition = catalog_->find(faction)) {
        for (const data::XmlNode& item : definition->root.children) {
            const audio::SfxEvent* Toggle::*slot = same(item.name, "SFXEvent_GUI_Toggle_Non_Hero_Ability_On") ? &Toggle::on
                : same(item.name, "SFXEvent_GUI_Toggle_Non_Hero_Ability_Off") ? &Toggle::off
                : same(item.name, "SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_On") ? &Toggle::enemy_on
                : same(item.name, "SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_Off") ? &Toggle::enemy_off : nullptr;
            if (slot == nullptr) continue;
            const std::string text = trim(item.raw_text);
            const std::size_t comma = text.find(',');
            const tactical::AbilityKind kind = tactical::ability_kind(trim(text.substr(0, comma)));
            if (kind == tactical::AbilityKind::none || comma == std::string::npos) continue;
            table[kind].*slot = event(text.substr(comma + 1), "faction " + faction);
        }
    } else {
        problems_.push_back("faction '" + faction + "' is not in the XML catalog");
    }
    return toggles_.emplace(faction, std::move(table)).first->second;
}

void BattleAudio::toggle_abilities(const tactical::TacticalSnapshot& snapshot, const LiveSessionView& live) {
    const tactical::PlayerId local = live.local_player();
    const auto enemy_of_local = [&](const tactical::PlayerId owner) {
        const auto owner_team = live.team_of(owner);
        const auto local_team = live.team_of(local);
        return owner_team && local_team ? *owner_team != *local_team : owner != local;
    };
    std::map<std::pair<sim::EntityId, tactical::AbilityKind>, AbilityMemory> next;
    std::set<std::tuple<tactical::PlayerId, tactical::AbilityKind, bool>> switched;
    for (const tactical::TacticalInstance& instance : snapshot.instances()) {
        for (const tactical::AbilityStatus& status : instance.abilities) {
            const auto key = std::make_pair(instance.entity_id, status.kind);
            const auto before = ability_memory_.find(key);
            // An ability is timed once it has read frames left while active; the read of its final tick is 0.
            const bool was_active = before != ability_memory_.end() && before->second.active;
            const bool timed = status.active && (status.remaining_frames > 0U || (was_active && before->second.timed));
            next[key] = {status.active, timed, status.active ? status.remaining_frames : 0U};
            if (!ability_memory_started_ || before == ability_memory_.end() || before->second.active == status.active) continue;
            // BA-51: a timed ability that ran out ends without a sound (its last read is 0 frames left); a
            // switch-off (or a lost engine or shield) plays the off sound, and so does an untimed one's end.
            if (!status.active && before->second.timed && before->second.remaining <= natural_end_frames) {
                if (ability_log_.size() < log_limit) {
                    ability_log_.push_back(std::string("expired ") + std::string(tactical::to_string(status.kind)));
                }
                continue;
            }
            switched.emplace(instance.owner, status.kind, status.active);
        }
    }
    ability_memory_ = std::move(next);
    ability_memory_started_ = true;
    // BA-50: one sound per switch, whatever the number of units it carried (a squadron's craft, a group).
    for (const auto& [owner, kind, on] : switched) {
        std::string faction = live.player_faction(owner);
        if (faction.empty() && owner == local) faction = live.local_faction();
        if (faction.empty()) continue;
        const auto& table = toggles_of(faction);
        const auto toggle = table.find(kind);
        if (toggle == table.end()) continue;
        const audio::SfxEvent* plain = on ? toggle->second.on : toggle->second.off;
        if (plain == nullptr) continue;
        const bool enemy = enemy_of_local(owner);
        const audio::SfxEvent* sfx = enemy ? (on ? toggle->second.enemy_on : toggle->second.enemy_off) : plain;
        if (ability_log_.size() < log_limit) {
            ability_log_.push_back(std::string("toggle ") + faction + " " + std::string(tactical::to_string(kind))
                                   + (on ? " on " : " off ") + (enemy ? "enemy " : "") + (sfx ? sfx->name : std::string("<none>")));
        }
        play(sfx, std::nullopt, false, "ability_toggle");
    }
}

void BattleAudio::voice_ability(const LiveSessionView::AbilityClick& click, const LiveSessionView& live) {
    // BA-52: the first pressed unit that has the ability speaks (a squadron as its leading live craft, BA-24).
    for (const sim::EntityId entity : click.units) {
        const auto seen = last_seen_.find(entity);
        if (seen == last_seen_.end() || seen->second.owner != live.local_player()) continue;
        tactical::TypeId type_id = seen->second.type;
        const auto members = live.squadron_members().find(entity);
        if (members != live.squadron_members().end()) {
            for (const sim::EntityId craft : members->second) {
                if (!live_entities_.contains(craft)) continue;
                const auto craft_type = entity_types_.find(craft);
                if (craft_type != entity_types_.end()) {
                    type_id = craft_type->second;
                    break;
                }
            }
        }
        const auto type = types_.find(type_id);
        if (type == types_.end()) continue;
        const auto voice = type->second.ability_voices.find(click.ability);
        if (voice == type->second.ability_voices.end()) continue;
        const audio::SfxEvent* line = click.activate ? voice->second.activated : voice->second.deactivated;
        if (ability_log_.size() < log_limit) {
            ability_log_.push_back(std::string("voice ") + type->second.name + " " + std::string(tactical::to_string(click.ability))
                                   + (click.activate ? " on " : " off ") + (line ? line->name : std::string("<none>")));
        }
        play(line, std::nullopt, false, "ability_voice");
        return;
    }
}

void BattleAudio::frame(const LiveSessionView& live, std::vector<BattleInput::Acknowledgement> acknowledgements,
                        std::vector<LiveSessionView::AbilityClick> ability_clicks, const FixedCamera& camera, const double delta) {
    if (released_) return;
    ++frames_;
    clock_ += delta;
    const LiveSessionView::BattleFrame& battle = live.battle_frame();
    presented_tick_ = battle.presented_tick;
    // BA-12: the listener stands where the camera looks, on the plane z = 60.
    const assets::Vec3f eye = space::source_from_render(camera.eye);
    const assets::Vec3f target = space::source_from_render(camera.target);
    const audio::Listener listener = audio::listener_for_camera(
        {eye.x, eye.y, eye.z}, {target.x - eye.x, target.y - eye.y, target.z - eye.z}, space_);
    // The camera would otherwise be the listener; the node joins the tree with the host.
    if (listener_->is_inside_tree() && !listener_->is_current()) listener_->make_current();
    const godot::Vector3 at = render(listener.position);
    const godot::Vector3 ahead = render(listener.ahead);
    // The players stand in the host's space; the listener's viewport is no 3D node, so its pose is
    // the host's transform of the listener's.
    const godot::Transform3D pose = godot::Transform3D().looking_at(ahead, godot::Vector3(0.0F, 1.0F, 0.0F)).translated(at);
    const godot::Transform3D placed = host_->is_inside_tree() ? host_->get_global_transform() * pose : pose;
    listener_->set_transform(placed);
    listener_camera_->set_transform(placed);
    char text[128];
    std::snprintf(text, sizeof(text), "%.1f,%.1f,%.1f", listener.position[0], listener.position[1], listener.position[2]);
    listener_text_ = text;
    listener_position_ = listener.position;
    update_voices(listener.position);
    update_music(delta);
    meter_buses();
    if (!battle.latest) return;
    const tactical::PlayerId local = live.local_player();

    // Where each unit the player sees stands (the last visible frame of one that left).
    if (metadata_snapshot_ != battle.latest) {
        if (battle.previous) {
            for (const tactical::TacticalInstance& instance : battle.previous->instances()) {
                entity_types_[instance.entity_id] = instance.type_id;
            }
        }
        live_entities_.clear();
        for (const tactical::TacticalInstance& instance : battle.latest->instances()) {
            live_entities_.insert(instance.entity_id);
            owners_[instance.entity_id] = instance.owner;
            entity_types_[instance.entity_id] = instance.type_id;
        }
        metadata_snapshot_ = battle.latest;
    }
    // Positions still interpolate every frame; destruction events need the last visible pose.
    for (const tactical::TacticalInstance& instance : battle.latest->instances()) {
        if (const auto unit = live.unit_frame(instance.entity_id)) {
            last_seen_[instance.entity_id] = {unit->position, unit->yaw_degrees, instance.type_id, instance.owner};
        } else {
            last_seen_.erase(instance.entity_id);
        }
    }
    // The ticks this frame reaches: an event of tick t sounds once the frame presents t - 1 (as
    // the battle effects fire), up to the newest completed tick.
    const std::uint64_t latest = battle.latest->completed_tick();
    const auto reach = std::min<std::uint64_t>(latest, static_cast<std::uint64_t>(std::max(0.0, std::floor(battle.presented_tick))) + 1U);
    if (!music_begun_ && music_) {
        music_begun_ = true;
        music_tick_ = fired_through_;
        if (const auto cue = music_->begin(random_)) cue_music(*cue);
    }
    // BA-13, BA-14 (shots) and BA-42 (the attack notification): a projectile ID not seen before is
    // a shot fired that tick. IDs are in creation order.
    for (std::uint64_t tick = fired_through_ + 1; tick <= reach; ++tick) {
        std::shared_ptr<const tactical::TacticalSnapshot> snapshot = live.snapshot_at(tick);
        if (!snapshot && tick == latest) snapshot = battle.latest;
        bool attack = false;
        if (snapshot) {
            const std::vector<sim::EntityId> visible = snapshot->visible_entities(local);
            for (const tactical::TacticalInstance& instance : snapshot->instances()) {
                entity_types_[instance.entity_id] = instance.type_id;
                owners_[instance.entity_id] = instance.owner;
            }
            // A lagging historical tick can overwrite metadata from the newest snapshot.
            // Restore the newest metadata next frame even if its snapshot pointer holds.
            if (snapshot != battle.latest) metadata_snapshot_.reset();
            toggle_abilities(*snapshot, live);
            for (const tactical::Projectile& projectile : snapshot->projectiles()) {
                if (!projectiles_seen_.insert(projectile.id).second) continue;
                const auto* shooter = space::find_instance(*snapshot, projectile.shooter);
                const auto target_owner = owners_.find(projectile.target);
                if (projectile.owner == local || (target_owner != owners_.end() && target_owner->second == local)) attack = true;
                if (shooter == nullptr) continue;
                const auto type = types_.find(shooter->type_id);
                if (type == types_.end()) continue;
                const auto fire = type->second.fire.find(projectile.weapon);
                if (fire == type->second.fire.end()) continue;
                const bool hidden = !std::binary_search(visible.begin(), visible.end(), projectile.shooter);
                play(fire->second, vec(projectile.position), hidden, "fire");
            }
            // Projectiles leave the snapshots when they land; IDs below the oldest in flight are done.
            if (!snapshot->projectiles().empty()) {
                const std::uint64_t oldest = snapshot->projectiles().front().id;
                projectiles_seen_.erase(projectiles_seen_.begin(), projectiles_seen_.lower_bound(oldest));
            }
        }
        if (attack) ++attacks_;
        music_tick_ = tick;
        if (music_) {
            if (const auto cue = music_->tick(attack, random_)) cue_music(*cue);
        }
    }
    fired_through_ = std::max(fired_through_, reach);

    // BA-15 (hits), BA-16 (deaths) and BA-17 (hardpoint deaths) from the event log.
    for (const platform::LiveTickEvents& record : battle.reached) {
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            // Heard when the local player sees the target, as the impact effect is drawn.
            if (!last_seen_.contains(event.target)) continue;
            const auto shooter = entity_types_.find(event.shooter);
            const tactical::TypeId shooter_type = shooter == entity_types_.end() ? tactical::TypeId{} : shooter->second;
            const auto type = types_.find(shooter_type);
            if (type == types_.end()) {
                ++results_["hit"]["unknown_shooter"];
                continue;
            }
            const audio::SfxEvent* sound = nullptr;
            if ((event.outcome & tactical::hit_outcome_armor_reduced) != 0U) {
                if (const auto armor = type->second.detonate_armor.find(event.weapon); armor != type->second.detonate_armor.end()) {
                    sound = armor->second;
                }
            }
            if (sound == nullptr) {
                if (const auto plain = type->second.detonate.find(event.weapon); plain != type->second.detonate.end()) {
                    sound = plain->second;
                }
            }
            play(sound, vec(event.aim), false, "hit");
        }
        for (const tactical::Event& event : record.events) {
            if (event.kind != tactical::EventKind::unit_destroyed && event.kind != tactical::EventKind::hardpoint_destroyed) {
                continue;
            }
            const auto seen = last_seen_.find(event.unit);
            if (seen == last_seen_.end()) continue;
            const auto type = types_.find(seen->second.type);
            if (type == types_.end()) continue;
            if (event.kind == tactical::EventKind::unit_destroyed) {
                play(type->second.death, seen->second.position, false, "death");
                last_seen_.erase(seen);
                continue;
            }
            if (event.hardpoint >= type->second.hardpoint_deaths.size()) continue;
            const double yaw = seen->second.yaw_degrees * 3.14159265358979323846 / 180.0;
            const audio::Vec3 local_point = vec(type->second.hardpoint_points[event.hardpoint]);
            const audio::Vec3 world{
                seen->second.position[0] + std::cos(yaw) * local_point[0] - std::sin(yaw) * local_point[1],
                seen->second.position[1] + std::sin(yaw) * local_point[0] + std::cos(yaw) * local_point[1],
                seen->second.position[2] + local_point[2]};
            play(type->second.hardpoint_deaths[event.hardpoint], world, false, "hardpoint");
        }
    }
    // BA-20 to BA-22: the unit responses to this frame's gestures.
    for (const BattleInput::Acknowledgement& acknowledgement : acknowledgements) respond(acknowledgement, live);
    // BA-52: the unit voices of this frame's ability presses.
    for (const LiveSessionView::AbilityClick& click : ability_clicks) voice_ability(click, live);
}

void BattleAudio::Spread::add(const double value) {
    low = count == 0 ? value : std::min(low, value);
    high = count == 0 ? value : std::max(high, value);
    sum += value;
    ++count;
}

void BattleAudio::end_voice(const std::size_t voice) {
    VoiceState& state = voice_states_[voice];
    if (state.started) {
        const std::uint64_t lived = frames_ - state.frame;
        ++lifetimes_[lived < 2 ? "0-1" : lived < 4 ? "2-3" : lived < 10 ? "4-9" : lived < 30 ? "10-29" : "30+"];
    }
    state = {};
}

void BattleAudio::meter_buses() {
    const godot::AudioServer* server = godot::AudioServer::get_singleton();
    for (BusLevel& bus : buses_) {
        if (bus.index < 0 || bus.index >= server->get_bus_count()) continue;
        if (bus.capture.is_valid()) {
            const std::int32_t available = bus.capture->get_frames_available();
            if (available > 0) {
                const godot::PackedVector2Array frames = bus.capture->get_buffer(available);
                const godot::Vector2* data = frames.ptr();
                for (std::int64_t index = 0; index < frames.size(); ++index) {
                    bus.energy += static_cast<double>(data[index].x) * data[index].x
                                  + static_cast<double>(data[index].y) * data[index].y;
                }
                bus.samples += static_cast<std::uint64_t>(frames.size());
            }
        }
        const double peak_db = std::max(server->get_bus_peak_volume_left_db(bus.index, 0),
                                        server->get_bus_peak_volume_right_db(bus.index, 0));
        if (peak_db <= -60.0) continue;
        const double peak = std::pow(10.0, peak_db / 20.0);
        ++bus.frames;
        bus.power += peak * peak;
        bus.peak = std::max(bus.peak, peak);
        if (peak_db >= -0.1) ++bus.clipped;
    }
}

void BattleAudio::release() {
    if (released_) return;
    released_ = true;
    for (godot::AudioStreamPlayer3D* player : players_3d_) {
        player->stop();
        player->queue_free();
    }
    for (godot::AudioStreamPlayer* player : players_2d_) {
        player->stop();
        player->queue_free();
    }
    for (MusicTrack& track : music_tracks_) {
        track.player->stop();
        track.player->queue_free();
    }
    listener_viewport_->queue_free();
    players_3d_.clear();
    players_2d_.clear();
}

void BattleAudio::write_report(std::ostream& output) const {
    const auto counts = [&output](const char* name, const std::map<std::string, std::uint64_t>& values) {
        output << ", \"" << name << "\": {";
        bool first = true;
        for (const auto& [key, count] : values) {
            output << (first ? "" : ", ") << json(key) << ": " << count;
            first = false;
        }
        output << "}";
    };
    const auto strings = [&output](const char* name, const auto& values) {
        output << ", \"" << name << "\": [";
        bool first = true;
        for (const auto& value : values) {
            output << (first ? "" : ", ") << json(value);
            first = false;
        }
        output << "]";
    };
    output << "  \"battle_audio\": {\"muted\": " << (options_.muted ? "true" : "false") << ", \"frames\": " << frames_
           << ", \"sfx_events\": " << registry_.size() << ", \"space_3d\": {\"saturation_factor\": "
           << space_.saturation_factor << ", \"rolloff_factor\": " << space_.rolloff_factor << ", \"listener_z\": "
           << space_.listener_z << ", \"max_distance\": " << space_.max_distance << "}, \"listener\": "
           << json(listener_text_) << ", \"voices_3d\": " << audio::Voices::voices_3d << ", \"max_voices\": " << max_voices_
           << ", \"stolen\": " << stolen_ << ", \"attacks\": " << attacks_;
    counts("requested", requested_);
    output << ", \"results\": {";
    bool first = true;
    for (const auto& [reason, values] : results_) {
        output << (first ? "" : ", ") << json(reason) << ": {";
        bool inner = true;
        for (const auto& [result, count] : values) {
            output << (inner ? "" : ", ") << json(result) << ": " << count;
            inner = false;
        }
        output << "}";
        first = false;
    }
    output << "}";
    counts("played_samples", played_samples_);
    output << ", \"missing_samples\": {";
    first = true;
    for (const auto& [name, cause] : missing_samples_) {
        output << (first ? "" : ", ") << json(name) << ": " << json(cause);
        first = false;
    }
    output << "}";
    strings("missing_events", missing_events_);
    output << ", \"types\": {";
    first = true;
    for (const auto& [id, sounds] : types_) {
        static_cast<void>(id);
        const auto name = [](const audio::SfxEvent* sfx) { return json(sfx ? sfx->name : std::string{}); };
        output << (first ? "" : ", ") << json(sounds.name) << ": {\"death\": " << name(sounds.death)
               << ", \"select\": " << name(sounds.select) << ", \"move\": " << name(sounds.move)
               << ", \"attack\": " << name(sounds.attack) << ", \"fire\": {";
        bool inner = true;
        for (const auto& [slot, sfx] : sounds.fire) {
            output << (inner ? "" : ", ") << json(std::to_string(slot)) << ": " << name(sfx);
            inner = false;
        }
        output << "}, \"detonate\": {";
        inner = true;
        for (const auto& [slot, sfx] : sounds.detonate) {
            output << (inner ? "" : ", ") << json(std::to_string(slot)) << ": " << name(sfx);
            inner = false;
        }
        output << "}, \"hardpoint_deaths\": [";
        inner = true;
        for (const audio::SfxEvent* sfx : sounds.hardpoint_deaths) {
            output << (inner ? "" : ", ") << name(sfx);
            inner = false;
        }
        output << "], \"ability_voices\": {";
        inner = true;
        for (const auto& [kind, voice] : sounds.ability_voices) {
            output << (inner ? "" : ", ") << json(tactical::to_string(kind)) << ": [" << name(voice.activated) << ", "
                   << name(voice.deactivated) << "]";
            inner = false;
        }
        output << "}}";
        first = false;
    }
    output << "}";
    output << ", \"music_mode\": "
           << json(!music_ ? "none" : music_->mode() == audio::MusicDirector::Mode::battle ? "battle"
                   : music_->mode() == audio::MusicDirector::Mode::ambient ? "ambient" : "none");
    strings("music", music_log_);
    strings("responses", response_log_);
    strings("abilities", ability_log_);
    output << ", \"ability_starts\": [";
    bool first_row = true;
    for (const StartRow& row : start_rows_) {
        output << (first_row ? "" : ", ") << "{\"reason\": " << json(row.reason) << ", \"event\": " << json(row.event)
               << ", \"sample\": " << json(row.sample) << ", \"tick\": " << row.tick << ", \"volume\": " << row.volume
               << ", \"pitch\": " << row.pitch << "}";
        first_row = false;
    }
    output << "]";
    strings("problems", problems_);
    const auto spreads = [&output](const char* name, const std::map<std::string, Spread>& values) {
        output << ", \"" << name << "\": {";
        bool comma = false;
        for (const auto& [key, spread] : values) {
            output << (comma ? ", " : "") << json(key) << ": {\"count\": " << spread.count << ", \"min\": " << spread.low
                   << ", \"mean\": " << (spread.count == 0 ? 0.0 : spread.sum / static_cast<double>(spread.count))
                   << ", \"max\": " << spread.high << "}";
            comma = true;
        }
        output << "}";
    };
    spreads("start_distance", start_distance_);
    spreads("start_gain_db", start_gain_db_);
    counts("voice_lifetimes_frames", lifetimes_);
    // #443: each bus's RMS over the whole run (its signal before its own slider; the mix's after
    // the channel sliders), and over the frames it carried sound the RMS of the per-frame peaks and
    // the loudest peak, in dBFS.
    output << ", \"levels\": {";
    first = true;
    for (const BusLevel& bus : buses_) {
        const double mean = bus.frames == 0 ? 0.0 : bus.power / static_cast<double>(bus.frames);
        const double rms = bus.samples == 0 ? 0.0 : std::sqrt(bus.energy / (2.0 * static_cast<double>(bus.samples)));
        output << (first ? "" : ", ") << json(bus.name) << ": {\"rms_db\": " << (rms <= 0.0 ? -120.0 : 20.0 * std::log10(rms))
               << ", \"frames\": " << bus.frames << ", \"peak_rms_db\": "
               << (bus.frames == 0 ? -120.0 : 10.0 * std::log10(mean)) << ", \"max_peak_db\": "
               << (bus.peak <= 0.0 ? -120.0 : 20.0 * std::log10(bus.peak)) << ", \"clipped_frames\": " << bus.clipped << "}";
        first = false;
    }
    output << "}";
    output << "},\n";
}

} // namespace eawr::presentation::godot_backend

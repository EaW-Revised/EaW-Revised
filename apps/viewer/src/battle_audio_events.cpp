#include "eawr/core/load_profile.hpp"
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

#include "battle_audio_internal.hpp"

namespace eawr::presentation::godot_backend {
void BattleAudio::loading_complete() {
    // WBF-10: resolved through the ordinary SFX event/voice path, including mute policy.
    if (!released_) play(event("GUI_Text_Hint_SFX", "local Begin"), std::nullopt, false, "loading_complete");
}
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_audio_detail;

namespace battle_audio_detail {

[[nodiscard]] float decibels(const double linear) {
    return linear <= 1.0e-4 ? -80.0F : static_cast<float>(20.0 * std::log10(linear));
}
} // namespace battle_audio_detail

namespace {

// The report keeps this many music and response rows.
constexpr std::size_t log_limit = 256;
// How long a voice asked to play counts as playing before its player reports it (a few physics steps).
constexpr double start_grace_seconds = 0.1;
// BA-51: a timed ability whose last read had at most this many frames left ended by itself.
constexpr std::uint32_t natural_end_frames = 2;

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
} // namespace

void BattleAudio::play(const audio::SfxEvent* sfx, const std::optional<audio::Vec3> position, const bool hidden,
                       const std::string& reason, const sim::EntityId loop_child, const sim::EntityId attached) {
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
    const double gain = audio::speech_sfx_gain(start.volume, speech_stream_.active(), gui_dialog_);
    if (gain < start.volume) ++ducked_starts_;
    if ((reason.starts_with("ability_") || reason.starts_with("battle_") || reason.starts_with("economy_")
         || reason == "hyperspace_arrival" || reason == "response_attack_hardpoint"
         || reason == "response_stop" || reason == "response_guard" || reason == "ambient_moving"
         || reason.starts_with("sighting_")) && start_rows_.size() < log_limit) {
        start_rows_.push_back({reason, sfx->name, start.sample, presented_tick_, gain, start.pitch});
    }
    VoiceState& state = voice_states_[start.voice];
    state = {start.volume, position.value_or(audio::Vec3{}), true, frames_, clock_, attached, gain};
    state.ambient = reason == "ambient_moving";
    if (audio::Voices::is_3d_voice(start.voice)) {
        godot::AudioStreamPlayer3D* player = players_3d_[start.voice];
        player->set_stream(loaded.stream);
        if (loop_child != sim::invalid_entity_id) {
            // WBP-20: a private loop stream, stopped when this construction child leaves.
            godot::Ref<godot::AudioStreamWAV> loop = loaded.stream->duplicate();
            if (loop.is_valid()) {
                loop->set_loop_mode(godot::AudioStreamWAV::LOOP_FORWARD);
                loop->set_loop_begin(0);
                loop->set_loop_end(static_cast<std::int32_t>(loop->get_data().size() / (loop->is_stereo() ? 4 : 2)));
                player->set_stream(loop);
                pad_loops_[loop_child] = {start.voice, frames_};
            }
        }
        player->set_position(render(state.position));
        player->set_pitch_scale(static_cast<float>(start.pitch));
        const double distance = std::sqrt(std::pow(state.position[0] - listener[0], 2.0)
                                          + std::pow(state.position[1] - listener[1], 2.0)
                                          + std::pow(state.position[2] - listener[2], 2.0));
        const float gain_db = decibels(gain * audio::falloff_gain(distance, sfx->saturation_distance, space_));
        start_distance_[reason].add(distance);
        start_gain_db_[reason].add(gain_db);
        player->set_volume_db(gain_db);
        player->play();
    } else {
        godot::AudioStreamPlayer* player = players_2d_[start.voice - audio::Voices::voices_3d];
        player->set_stream(loaded.stream);
        player->set_pitch_scale(static_cast<float>(start.pitch));
        player->set_volume_db(decibels(gain));
        player->play();
    }
    max_voices_ = std::max<std::uint64_t>(max_voices_, voices_.playing_count());
}

const BattleAudio::BuildSounds& BattleAudio::build_sounds(const std::string& faction) {
    if (const auto found = build_sounds_.find(faction); found != build_sounds_.end()) return found->second;
    BuildSounds sounds;
    if (auto object = catalog_->resolve(faction, data::Category::faction)) {
        sounds.started = event(tag(object.value(), "SFXEvent_Tactical_Object_Building_Started"), faction);
        sounds.loop = event(tag(object.value(), "SFXEvent_Tactical_Object_Building_Loop"), faction);
        sounds.complete = event(tag(object.value(), "SFXEvent_Tactical_Object_Building_Complete"), faction);
        sounds.captured = event(tag(object.value(), "SFXEvent_HUD_Build_Pad_Captured"), faction);
        sounds.lost = event(tag(object.value(), "SFXEvent_HUD_Build_Pad_Lost"), faction);
        sounds.sold = event(tag(object.value(), "SFXEvent_Tactical_Object_Sold"), faction);
    }
    return build_sounds_.emplace(faction, sounds).first->second;
}

void BattleAudio::update_voices(const audio::Vec3& listener, const LiveSessionView& live) {
    const auto latest = live.battle_frame().latest;
    std::uint64_t local_bit = 0;
    if (latest) {
        const auto players = latest->players();
        for (std::size_t index = 0; index < players.size(); ++index) {
            if (players[index].player_id == live.local_player()) local_bit = std::uint64_t{1} << index;
        }
    }
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
        // SND-61: ordinary ongoing 2D and attached 3D gain writes are capped;
        // fixed-position spatial voices retain the gain chosen at their start.
        const double gain = audio::speech_sfx_gain(state.volume, speech_stream_.active(), gui_dialog_);
        if (!three_d) {
            if (gain < state.volume) ++ducked_updates_;
            players_2d_[voice - audio::Voices::voices_3d]->set_volume_db(decibels(gain));
            continue;
        }
        bool hidden = false;
        if (state.ambient) {
            const auto* instance = latest ? space::find_instance(*latest, state.attached) : nullptr;
            if (!instance) {
                // SND-11: removal detaches the live-object cue; fixed voice slots bound this work.
                players_3d_[voice]->stop();
                voices_.finished(voice);
                end_voice(voice);
                ++ambient_detached_;
                continue;
            }
            hidden = (instance->visible_to & local_bit) == 0U
                || (instance->arrival && *instance->arrival < tactical::arrival_visible_frame)
                || !live.unit_frame(state.attached);
            ++ambient_attached_updates_;
            if (hidden) ++ambient_hidden_updates_;
        }
        // SP-03: the spinning sound follows the same interpolated pose as the dead copy.
        // A missing pose keeps the last position until SP-08 stops the attached sound.
        if (state.attached != sim::invalid_entity_id) {
            if (const auto unit = live.unit_frame(state.attached)) {
                if (state.position != unit->position) ++attached_moves_;
                state.position = unit->position;
                players_3d_[voice]->set_position(render(state.position));
                voices_.set_position(voice, state.position);
            }
        }
        const audio::SfxEvent* sfx = voices_.playing(voice);
        if (sfx == nullptr) continue;
        const double distance = std::sqrt(std::pow(state.position[0] - listener[0], 2.0)
                                          + std::pow(state.position[1] - listener[1], 2.0)
                                          + std::pow(state.position[2] - listener[2], 2.0));
        const double current_gain = hidden ? 0.0
            : state.attached == sim::invalid_entity_id ? state.positional_gain : gain;
        if (state.attached != sim::invalid_entity_id && gain < state.volume) ++ducked_updates_;
        players_3d_[voice]->set_volume_db(decibels(current_gain * audio::falloff_gain(distance, sfx->saturation_distance, space_)));
    }
}

void BattleAudio::ambient_moving(const tactical::TacticalSnapshot& snapshot, const LiveSessionView& live,
                               const std::span<const sim::EntityId> visible) {
    // SND-46: service once per logical frame; the free delay generator never feeds simulation.
    const auto stamp = snapshot.completed_tick();
    const auto now = stamp == 0 ? 0 : stamp - 1;
    const auto roster = snapshot.squadrons();
    bool refresh_members = !std::equal(roster.begin(), roster.end(), ambient_roster_.begin(), ambient_roster_.end());
    if (!refresh_members) {
        for (const auto& squadron : roster) {
            if (squadron.members.empty()) continue;
            if (!space::find_instance(snapshot, squadron.container)) continue;
            const auto cached = ambient_members_.find(squadron.members.front());
            if (cached == ambient_members_.end() || !space::find_instance(snapshot, cached->second.leader)) {
                refresh_members = true;
                break;
            }
        }
    }
    if (refresh_members) {
        ambient_roster_.assign(roster.begin(), roster.end());
        ambient_members_.clear();
        for (const auto& squadron : roster) {
            if (squadron.members.empty()) continue;
            // BA-24/SND-46: the retained roster includes dead craft; resolve its first live member.
            sim::EntityId leader = sim::invalid_entity_id;
            for (const auto member : squadron.members) {
                if (space::find_instance(snapshot, member)) { leader = member; break; }
            }
            for (const auto member : squadron.members) {
                ambient_members_.emplace(member, AmbientMember{squadron.container, leader});
            }
        }
    }
    for (const auto& instance : snapshot.instances()) {
        ++ambient_visits_;
        const auto type = types_.find(instance.type_id);
        if (type == types_.end() || type->second.squadron || !type->second.ambient_moving) continue;
        const auto& sounds = type->second;
        auto timer = ambient_timers_.find(instance.entity_id);
        if (timer == ambient_timers_.end()) {
            const auto delay = ambient_random_.between(sounds.ambient_min_delay, sounds.ambient_max_delay);
            timer = ambient_timers_.emplace(instance.entity_id, AmbientTimer{now + static_cast<std::uint64_t>(delay), stamp}).first;
            ++ambient_initialized_;
        }
        timer->second.seen = stamp;
        const auto member = ambient_members_.find(instance.entity_id);
        if (member != ambient_members_.end() && member->second.leader != instance.entity_id) {
            ++ambient_follower_skips_;
            continue;
        }
        if (timer->second.next > now) continue;
        ++ambient_due_;
        const auto* path = member == ambient_members_.end() ? &instance
            : space::find_instance(snapshot, member->second.container);
        const bool moving = path && path->has_movement_path;
        const bool hidden = !std::binary_search(visible.begin(), visible.end(), instance.entity_id);
        if (ambient_rows_.size() < log_limit) ambient_rows_.push_back({now, instance.entity_id, moving, hidden});
        if (moving) {
            const auto pose = live.unit_frame(instance.entity_id);
            const audio::Vec3 position = pose ? pose->position : audio::Vec3{
                to_double(instance.fixed_transform.rows[0][3]), to_double(instance.fixed_transform.rows[1][3]),
                to_double(instance.fixed_transform.rows[2][3])};
            play(sounds.ambient_moving, position, hidden, "ambient_moving", sim::invalid_entity_id, instance.entity_id);
        } else ++ambient_stationary_;
        // Reschedule even a stationary, hidden, missing-sample or voice-refused request.
        timer->second.next = now + static_cast<std::uint64_t>(ambient_random_.between(
            sounds.ambient_min_delay, sounds.ambient_max_delay));
    }
    for (auto timer = ambient_timers_.begin(); timer != ambient_timers_.end();) {
        if (timer->second.seen == stamp) { ++timer; continue; }
        timer = ambient_timers_.erase(timer);
        ++ambient_retired_;
    }
    ambient_peak_ = std::max<std::uint64_t>(ambient_peak_, ambient_timers_.size());
}

void BattleAudio::production_cue(const SpeechEvent* speech, const audio::SfxEvent* fallback,
                                 const std::string_view reason) {
    if (!speech) {
        play(fallback, std::nullopt, false, std::string(reason));
        return;
    }
    // SND-35/38: an authored speech event wins even if its MP3 fails to open.
    if (speech_queue_.push(speech, reason)) ++speech_queued_;
    else ++speech_overflow_;
}

void BattleAudio::update_speech(const bool paused) {
    speech_player_->set_stream_paused(paused);
    if (paused) return;
    // SND-48: cleanup follows SFX service; paused streams retain their handle.
    if (speech_stream_.active() && !speech_player_->is_playing()
        && clock_ - speech_started_at_ > start_grace_seconds) speech_stream_.finished(false);
    const auto* front = speech_queue_.front();
    if (!front) return;
    if (queued_speech_started_) {
        if (speech_stream_.contains(queued_speech_handle_)) return;
        ++speech_completed_;
        speech_queue_.pop();
        queued_speech_started_ = false;
        return;
    }
    const SpeechEvent& speech = *front->event;
    // SND-44/48: normal priority, centered pan and zero fade; FIFO service
    // attempts one front per frame, with no priority ordering or cooldown.
    if (!speech_stream_.admit(3)) {
        ++speech_failed_;
        speech_queue_.pop();
        return;
    }
    speech_player_->stop();
    if (speech.streams.empty()) {
        ++speech_failed_;
        speech_queue_.pop();
        return;
    }
    const auto index = static_cast<std::size_t>(random_.between(0, static_cast<int>(speech.streams.size()) - 1));
    if (!speech.streams[index].is_valid()) {
        ++speech_failed_;
        speech_queue_.pop();
        return;
    }
    speech_player_->set_stream(speech.streams[index]);
    speech_player_->set_volume_db(decibels(speech.volume));
    speech_player_->set_pitch_scale(1.0F);
    speech_player_->play();
    speech_stream_.opened();
    queued_speech_handle_ = speech_stream_.handle();
    queued_speech_started_ = true;
    speech_started_at_ = clock_;
    if (start_rows_.size() < log_limit) {
        start_rows_.push_back({"speech_" + std::string(front->reason), speech.name, speech.files[index], presented_tick_, speech.volume, 1.0});
    }
}

void BattleAudio::cue_music(const audio::MusicDirector::Cue& cue) {
    const char* mode = cue.mode == audio::MusicDirector::Mode::battle ? "battle"
        : cue.mode == audio::MusicDirector::Mode::none && summary_event_ ? "summary" : "ambient";
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
            if (summary_event_) {
                if (summary_event_->loop || summary_next_file_ < summary_event_->files.size()) {
                    const auto file = summary_event_->files[summary_next_file_++ % summary_event_->files.size()];
                    cue_music({summary_event_, file, audio::MusicDirector::Mode::none});
                }
            } else if (music_) {
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
        if (const auto target = entity_types_.find(acknowledgement.target); target != entity_types_.end()) {
            const auto type = types_.find(target->second);
            if (type != types_.end() && acknowledgement.hardpoint < type->second.hardpoint_types.size()) {
                const auto cue = speaker.attack_hardpoint.find(type->second.hardpoint_types[acknowledgement.hardpoint]);
                if (cue != speaker.attack_hardpoint.end() && cue->second != nullptr) {
                    line = cue->second;
                    kind = "attack_hardpoint";
                }
            }
        }
        break;
    case BattleInput::Acknowledgement::Kind::stop:
        line = speaker.stop;
        kind = "stop";
        break;
    case BattleInput::Acknowledgement::Kind::guard:
        line = speaker.guard;
        kind = "guard";
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
    if (const data::Definition* definition = catalog_->find(faction, data::Category::faction)) {
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
            const bool timed = status.active && status.total_frames > 0U;
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
    gui_dialog_ = live.time().paused(); // the viewer's pause banner is its modal dialog
    clock_ += delta;
    const LiveSessionView::BattleFrame& battle = live.battle_frame();
    if (!summary_shown_ && live.battle_end() && live.battle_end()->ended_frame) {
        summary_shown_ = true;
        summary_event_ = summary_events_[live.battle_end()->result == ui::BattleResult::victory ? 0 : 1];
        // WBF-47: an unresolved pointer does not start an invented cue or stop the current track.
        if (summary_event_) {
            summary_next_file_ = 1;
            cue_music({summary_event_, summary_event_->files.front(), audio::MusicDirector::Mode::none});
        }
    }
    presented_tick_ = battle.presented_tick;
    if (!outcome_shown_ && live.battle_end()) {
        outcome_shown_ = true;
        const bool won = live.battle_end()->result == ui::BattleResult::victory;
        play(outcome_events_[won ? 0 : 1], std::nullopt, false, won ? "battle_victory" : "battle_defeat");
    }
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
    update_voices(listener.position, live);
    update_music(delta);
    meter_buses();
    if (!battle.latest) { update_speech(live.time().paused()); return; }
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
    // SP-03/SP-08: keep the visible dead copy's metadata and final pose until its end event.
    for (const tactical::SpinningCraft& craft : battle.latest->spinning()) {
        if (const auto unit = live.unit_frame(craft.entity_id)) {
            last_seen_[craft.entity_id] = {unit->position, unit->yaw_degrees, craft.type_id, craft.owner};
        } else {
            last_seen_.erase(craft.entity_id);
        }
    }
    // The ticks this frame reaches: an event of tick t sounds once the frame presents t - 1 (as
    // the battle effects fire), up to the newest completed tick.
    const std::uint64_t latest = battle.latest->completed_tick();
    const auto reach = std::min<std::uint64_t>(latest, static_cast<std::uint64_t>(std::max(0.0, std::floor(battle.presented_tick))) + 1U);
    if (!ambient_begun_) {
        auto initial = live.snapshot_at(0);
        if (!initial) initial = battle.previous ? battle.previous : battle.latest;
        const auto visible = initial->visible_entities(local);
        ambient_moving(*initial, live, visible);
        ambient_begun_ = true;
    }
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
            // SND-42/64, WSU-03: use logical snapshot visibility, never the
            // model fade, ghost or camera/radar visibility. Stable fog service
            // is once per 30 logical frames; the presentation observer shares
            // one phase for the published roster, without changing simulation.
            if (tick > sighting_tick_) {
                sighting_tick_ = tick + 29;
                for (const auto& instance : snapshot->instances()) {
                    if (instance.arrival && *instance.arrival < tactical::arrival_visible_frame) continue;
                    if (!std::binary_search(visible.begin(), visible.end(), instance.entity_id)) continue;
                    auto type = types_.find(instance.type_id);
                    if (type == types_.end() || type->second.squadron) continue;
                    bool busy = false;
                    for (std::size_t voice = audio::Voices::voices_3d; voice < voice_states_.size(); ++voice) {
                        busy = busy || voices_.playing(voice) != nullptr;
                    }
                    // The retained setup distinguishes battle parties from map owners.
                    const bool participant = live.battle_participant(instance.owner);
                    auto& sounds = type->second;
                    const auto cue = sightings_.observe(sounds.type_sighted, sounds.announce_sighting, busy,
                        sounds.spotted != nullptr, participant && !live.is_ally_of_local(instance.owner));
                    if (cue == audio::SightingAnnouncements::Cue::type) play(sounds.spotted, std::nullopt, false, "sighting_type");
                    else if (cue == audio::SightingAnnouncements::Cue::enemy) play(enemy_spotted_, std::nullopt, false, "sighting_enemy");
                }
            }
            // A lagging historical tick can overwrite metadata from the newest snapshot.
            // Restore the newest metadata next frame even if its snapshot pointer holds.
            if (snapshot != battle.latest) metadata_snapshot_.reset();
            toggle_abilities(*snapshot, live);
            ambient_moving(*snapshot, live, visible);
            // WPR-22/30/31: sparse snapshot notifications retain accepted type identity;
            // no queue-length inference, so duplicate buys and same-tick cancels remain distinct.
            const auto cues = snapshot->economy_cues();
            while (economy_cues_delivered_ < cues.size()) {
                const auto& cue = cues[economy_cues_delivered_++];
                if (cue.kind == tactical::BattleEconomyCue::Kind::arrival) {
                    // WR-37: per-object spatial start for human and AI craft/ships; containers are silent.
                    const auto type = types_.find(cue.type);
                    if (type == types_.end() || type->second.squadron) continue;
                    const std::string faction = live.player_faction(cue.owner);
                    auto sound = arrival_sounds_.find(faction);
                    if (sound == arrival_sounds_.end()) {
                        const auto object = catalog_->resolve(faction, data::Category::faction);
                        sound = arrival_sounds_.emplace(faction, object
                            ? event(tag(object.value(), "SFXEvent_Arrive_From_Hyperspace"), faction) : nullptr).first;
                    }
                    const auto players = snapshot->players();
                    const auto player = std::find_if(players.begin(), players.end(), [local](const auto& entry) { return entry.player_id == local; });
                    const bool hidden = player == players.end() || (cue.visible_to & (std::uint64_t{1} << static_cast<unsigned>(player - players.begin()))) == 0U;
                    play(sound->second, vec(cue.position), hidden, "hyperspace_arrival");
                    continue;
                }
                if (cue.owner != local) continue;
                if (cue.kind == tactical::BattleEconomyCue::Kind::unit_cap) {
                    const auto faction = catalog_->resolve(live.local_faction(), data::Category::faction);
                    if (faction) play(event(tag(faction.value(), "SFXEvent_Tactical_Unit_Cap_Reached"), live.local_faction()),
                        std::nullopt, false, "economy_unit_cap");
                    continue;
                }
                const auto type = types_.find(cue.type);
                if (type == types_.end()) continue;
                const bool started = cue.kind == tactical::BattleEconomyCue::Kind::started;
                production_cue(started ? type->second.build_underway_speech : type->second.build_stopped_speech,
                    started ? type->second.build_started : type->second.build_cancelled,
                    started ? "economy_build_started" : "economy_build_cancelled");
            }
            const auto productions = snapshot->productions();
            while (productions_delivered_ < productions.size()) {
                const auto& production = productions[productions_delivered_++];
                if (production.owner != local) continue;
                // Build pads have their own local/spatial completion path below.
                const bool queued = std::any_of(live.economy().menus.begin(), live.economy().menus.end(), [&](const auto& menu) {
                    return menu.find(production.type) != nullptr;
                });
                const auto type = types_.find(production.type);
                if (queued && type != types_.end()) production_cue(type->second.build_completed_speech,
                    type->second.build_complete, "economy_build_complete");
            }
            for (const auto& economy : snapshot->economy()) {
                if (economy.player != local) continue;
                const bool full = economy.population >= economy.population_cap;
                if (full && !population_full_) {
                    const auto faction = catalog_->resolve(live.local_faction(), data::Category::faction);
                    if (faction) play(event(tag(faction.value(), "SFXEvent_Tactical_Pop_Cap_Reached"), live.local_faction()),
                        std::nullopt, false, "economy_population_cap");
                }
                population_full_ = full;
            }
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
        if (music_ && !summary_event_) {
            if (const auto cue = music_->tick(attack, random_)) cue_music(*cue);
        }
    }
    fired_through_ = std::max(fired_through_, reach);

    // BA-15 (hits), BA-16 (deaths) and BA-17 (hardpoint deaths) from the event log.
    // WBP-20: loop ownership includes the voice's start frame, so a stolen voice is never stopped.
    for (auto loop = pad_loops_.begin(); loop != pad_loops_.end();) {
        if (live.snapshot_index().instance(loop->first)) { ++loop; continue; }
        const auto [voice, started] = loop->second;
        if (voice < voice_states_.size() && voice_states_[voice].frame == started) {
            players_3d_[voice]->stop();
            voices_.finished(voice);
            end_voice(voice);
        }
        loop = pad_loops_.erase(loop);
    }
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
        for (const tactical::AsteroidImpact& impact : record.asteroid_impacts) {
            const auto seen = last_seen_.find(impact.target);
            if (seen == last_seen_.end()) continue;
            if (const auto type = types_.find(seen->second.type); type != types_.end()) {
                play(type->second.asteroid_damage, audio::Vec3{seen->second.position[0], seen->second.position[1], seen->second.position[2]}, false, "asteroid");
            }
        }
        for (const tactical::Event& event : record.events) {
            if (event.kind == tactical::EventKind::station_replaced) {
                // WPR-52 step 6: the local announcer's own/ally/enemy upgraded line.
                const auto faction = catalog_->resolve(live.local_faction(), data::Category::faction);
                if (faction) {
                    const auto field = event.player == live.local_player() ? "SFXEvent_Starbase_Upgraded"
                        : live.is_ally_of_local(event.player) ? "SFXEvent_Starbase_Ally_Upgraded" : "SFXEvent_Starbase_Enemy_Upgraded";
                    play(this->event(tag(faction.value(), field), live.local_faction()), std::nullopt, false, "station_upgraded");
                }
                continue;
            }
            if (event.kind == tactical::EventKind::pad_structure_sold) {
                const auto seen = last_seen_.find(event.unit);
                if (seen != last_seen_.end()) {
                    if (const auto type = types_.find(seen->second.type); type != types_.end() && event.player == local) {
                        play(type->second.sold, std::nullopt, false, "pad_sold");
                    }
                    play(build_sounds(live.player_faction(event.player)).sold, seen->second.position, false, "pad_spatial_sold");
                    last_seen_.erase(seen);
                }
                continue;
            }
            if (event.kind == tactical::EventKind::pad_captured) {
                const auto& sounds = build_sounds(live.player_faction(local));
                if (live.is_ally_of_local(event.player)) {
                    play(sounds.captured, std::nullopt, false, "pad_captured");
                } else if (const auto previous = live.snapshot_at(record.tick)) {
                    if (const auto* pad = space::find_instance(*previous, event.unit);
                        pad && live.is_ally_of_local(pad->owner)) {
                        play(sounds.lost, std::nullopt, false, "pad_lost");
                    }
                }
                continue;
            }
            if (event.kind == tactical::EventKind::pad_construction_started || event.kind == tactical::EventKind::pad_construction_completed) {
                const auto type_id = entity_types_.find(event.unit);
                const auto type = type_id == entity_types_.end() ? types_.end() : types_.find(type_id->second);
                const bool starting = event.kind == tactical::EventKind::pad_construction_started;
                const auto* builder = live.economy().player(event.player);
                if (type != types_.end() && event.player == local && builder && !builder->ai) {
                    play(starting ? type->second.build_started : type->second.build_complete, std::nullopt, false,
                        starting ? "pad_build_started" : "pad_build_complete");
                }
                const auto seen = last_seen_.find(event.unit);
                if (seen != last_seen_.end()) {
                    const auto& sounds = build_sounds(live.player_faction(event.player));
                    play(starting ? sounds.started : sounds.complete, seen->second.position, false,
                        starting ? "pad_spatial_started" : "pad_spatial_complete");
                    if (starting && live.snapshot_index().instance(event.unit)) {
                        play(sounds.loop, seen->second.position, false, "pad_construction_loop", event.unit);
                    }
                }
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_started) continue;
            if (event.kind != tactical::EventKind::unit_destroyed && event.kind != tactical::EventKind::hardpoint_destroyed
                && event.kind != tactical::EventKind::spin_away_ended) {
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_ended) {
                // SP-08/BA-16: remove attached sound even after the copy leaves the visible cache.
                for (std::size_t voice = 0; voice < voice_states_.size(); ++voice) {
                    const auto& state = voice_states_[voice];
                    if (state.attached != event.unit) continue;
                    if (audio::Voices::is_3d_voice(voice)) {
                        players_3d_[voice]->stop();
                    } else {
                        players_2d_[voice - audio::Voices::voices_3d]->stop();
                    }
                    voices_.finished(voice);
                    end_voice(voice);
                }
            }
            const auto seen = last_seen_.find(event.unit);
            if (seen == last_seen_.end()) continue;
            const auto type = types_.find(seen->second.type);
            if (type == types_.end()) continue;
            if (event.kind == tactical::EventKind::unit_destroyed) {
                // SP-03: a spin replaces the ordinary death cue; SP-08 plays that at the end.
                const bool spins = std::any_of(record.events.begin(), record.events.end(), [&event](const auto& other) {
                    return other.kind == tactical::EventKind::spin_away_started && other.unit == event.unit;
                });
                if (spins) {
                    play(type->second.spin_death, seen->second.position, false, "spin_death",
                         sim::invalid_entity_id, event.unit);
                    continue;
                }
                play(type->second.death, seen->second.position, false, "death");
                last_seen_.erase(seen);
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_ended) {
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
    update_speech(live.time().paused());
}

void BattleAudio::end_voice(const std::size_t voice) {
    VoiceState& state = voice_states_[voice];
    if (state.started) {
        const std::uint64_t lived = frames_ - state.frame;
        ++lifetimes_[lived < 2 ? "0-1" : lived < 4 ? "2-3" : lived < 10 ? "4-9" : lived < 30 ? "10-29" : "30+"];
    }
    state = {};
}

} // namespace eawr::presentation::godot_backend

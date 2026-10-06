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

std::optional<audio::EventQueue::Handle> BattleAudio::play(const audio::SfxEvent* sfx,
    const std::optional<audio::Vec3> position, const bool hidden, const std::string& reason,
    const sim::EntityId loop_child, const sim::EntityId attached, const bool engine_loop) {
    if (!sfx || released_) return std::nullopt;
    ++requested_[reason + ":" + sfx->name];
    auto& allocation = allocations_[reason];
    ++allocation.requested;
    const auto backend = event_backend();
    // SND-19: replacement detaches before any gate and cannot be undone by refusal.
    if (attached != sim::invalid_entity_id && sfx->kills_previous_object_sfx) events_.detach(attached, backend);
    auto admission = options_.admission;
    admission.speech_stream = speech_stream_.active();
    const auto accepted = events_.admit({{sfx, position, attached ? false : hidden, false, false, clock_},
                                       attached, loop_child != sim::invalid_entity_id}, random_, admission);
    if (accepted.handle) ++allocation.admitted;
    else ++allocation.refused;
    ++results_[reason][std::string(audio::to_string(accepted.result))];
    if (accepted.handle) {
        event_states_[*accepted.handle] = {reason, attached, loop_child, engine_loop, hidden, sfx};
        if (loop_child != sim::invalid_entity_id) pad_loops_[loop_child] = *accepted.handle;
    }
    return accepted.handle;
}

audio::EventQueue::Backend BattleAudio::event_backend() {
    return {
        [this](const audio::EventQueue::Sample& cue) { return start_sample(cue); },
        [this](const std::size_t voice) {
            const auto& state = voice_states_[voice];
            return state.started && (state.paused || clock_ - state.started_at <= start_grace_seconds
                || (audio::Voices::is_3d_voice(voice) ? players_3d_[voice]->is_playing()
                    : players_2d_[voice - audio::Voices::voices_3d]->is_playing()));
        },
        [this](const std::size_t voice) {
            if (audio::Voices::is_3d_voice(voice)) players_3d_[voice]->stop();
            else players_2d_[voice - audio::Voices::voices_3d]->stop();
            voices_.finished(voice);
            end_voice(voice);
        },
        [this](const std::size_t voice) { voices_.set_fading(voice); }
    };
}

audio::Start BattleAudio::start_sample(const audio::EventQueue::Sample& cue) {
    const auto& metadata = event_states_.at(cue.handle);
    const auto* sfx = cue.request.voice.event;
    const auto position = cue.request.voice.position;
    const auto& reason = metadata.reason;
    const auto attached = metadata.attached;
    const bool engine_loop = metadata.engine_loop;
    const bool hidden = metadata.hidden;
    const audio::Vec3 listener = listener_position_;
    auto& allocation = allocations_[reason];
    auto request = cue.request.voice;
    request.started_at = clock_;
    ++allocation.samples_requested;
    auto start = voices_.allocate(request, listener, cue.values);
    if (start.result != audio::Start::Result::playing) {
        ++allocation.samples_failed;
        ++results_[reason]["allocation_" + std::string(audio::to_string(start.result))];
        return start;
    }
    ++allocation.allocated;
    if (start.stopped) {
        ++stolen_;
        ++allocation.stolen;
        if (audio::Voices::is_3d_voice(*start.stopped)) players_3d_[*start.stopped]->stop();
        else players_2d_[*start.stopped - audio::Voices::voices_3d]->stop();
        end_voice(*start.stopped);
    }
    const Sample& loaded = sample(start.sample);
    if (!loaded.stream.is_valid()) {
        // Listed under missing_samples; the voice is free again at once.
        ++results_[reason]["sample_missing"];
        voices_.finished(start.voice);
        start.result = audio::Start::Result::no_samples;
        return start;
    }
    ++played_samples_[start.sample];
    ++allocation.audible;
    const double gain = audio::Voices::is_3d_voice(start.voice) && attached && hidden ? 0.0
        : audio::speech_sfx_gain(start.volume, speech_stream_.active(), gui_dialog_);
    if (gain < start.volume) ++ducked_starts_;
    if ((reason == "authored_chain" || reason.starts_with("ability_") || reason.starts_with("battle_") || reason.starts_with("economy_")
         || reason == "hyperspace_arrival" || reason == "response_attack_hardpoint" || reason == "lifetime_detonation"
         || reason == "response_stop" || reason == "response_guard" || reason == "ambient_moving" || reason == "base_under_attack"
         || reason.starts_with("reinforcement_") || reason.starts_with("engine_") || reason.starts_with("sighting_") || reason.starts_with("command_") || reason.starts_with("response_") || reason == "negative_feedback") && start_rows_.size() < log_limit) {
        start_rows_.push_back({reason, sfx->name, start.sample, presented_tick_, gain, start.pitch, position, attached, sfx->localized});
    }
    VoiceState& state = voice_states_[start.voice];
    state = {start.volume, position.value_or(audio::Vec3{}), true, frames_, clock_, attached, gain};
    state.ambient = reason == "ambient_moving" || engine_loop;
    state.engine_loop = engine_loop;
    state.fade_started = clock_;
    state.fade_seconds = engine_loop && cue.stage == audio::EventQueue::Stage::main ? sfx->loop_fade_in_seconds : 0.0;
    state.event_handle = cue.handle;
    state.paused = paused_ && audio::pauses_with_game(*sfx, audio::Voices::is_3d_voice(start.voice));
    if (audio::Voices::is_3d_voice(start.voice)) {
        godot::AudioStreamPlayer3D* player = players_3d_[start.voice];
        player->set_bus(buses_[sfx->localized ? 1 : 0].name);
        player->set_stream(loaded.stream);
        if (cue.continuous) {
            // WBP-20: a private loop stream, stopped when this construction child leaves.
            godot::Ref<godot::AudioStreamWAV> loop = loaded.stream->duplicate();
            if (loop.is_valid()) {
                loop->set_loop_mode(godot::AudioStreamWAV::LOOP_FORWARD);
                loop->set_loop_begin(0);
                loop->set_loop_end(static_cast<std::int32_t>(loop->get_data().size() / (loop->is_stereo() ? 4 : 2)));
                player->set_stream(loop);
            }
        }
        player->set_position(render(state.position));
        player->set_pitch_scale(static_cast<float>(start.pitch));
        const double distance = std::sqrt(std::pow(state.position[0] - listener[0], 2.0)
                                          + std::pow(state.position[1] - listener[1], 2.0)
                                          + std::pow(state.position[2] - listener[2], 2.0));
        const float gain_db = decibels((state.fade_seconds > 0.0 ? 0.0 : gain)
            * audio::falloff_gain(distance, sfx->saturation_distance, space_));
        start_distance_[reason].add(distance);
        start_gain_db_[reason].add(gain_db);
        player->set_volume_db(gain_db);
        player->play();
        player->set_stream_paused(state.paused);
    } else {
        godot::AudioStreamPlayer* player = players_2d_[start.voice - audio::Voices::voices_3d];
        const auto slot = start.voice - audio::Voices::voices_3d;
        panners_[slot]->set_pan(static_cast<float>(start.pan * 2.0 - 1.0));
        godot::AudioServer::get_singleton()->set_bus_send(pan_buses_[slot], buses_[sfx->localized ? 1 : 0].name);
        player->set_stream(loaded.stream);
        if (cue.continuous) {
            godot::Ref<godot::AudioStreamWAV> loop = loaded.stream->duplicate();
            if (loop.is_valid()) {
                loop->set_loop_mode(godot::AudioStreamWAV::LOOP_FORWARD);
                loop->set_loop_begin(0);
                loop->set_loop_end(static_cast<std::int32_t>(loop->get_data().size() / (loop->is_stereo() ? 4 : 2)));
                player->set_stream(loop);
            }
        }
        player->set_pitch_scale(static_cast<float>(start.pitch));
        player->set_volume_db(decibels(gain));
        player->play();
        player->set_stream_paused(state.paused);
    }
    max_voices_ = std::max<std::uint64_t>(max_voices_, voices_.playing_count());
    return start;
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

void BattleAudio::update_pause(const bool paused, const double delta) {
    const bool changed = paused_ != paused;
    if (changed) ++pause_transitions_;
    paused_ = paused;
    for (std::size_t voice = 0; voice < voice_states_.size(); ++voice) {
        auto& state = voice_states_[voice];
        const auto* sfx = voices_.playing(voice);
        if (!state.started || !sfx) continue;
        const bool spatial = audio::Voices::is_3d_voice(voice);
        const bool hold = paused && audio::pauses_with_game(*sfx, spatial);
        double position{};
        if (spatial) {
            auto* player = players_3d_[voice];
            player->set_stream_paused(hold);
            position = player->get_playback_position();
        } else {
            auto* player = players_2d_[voice - audio::Voices::voices_3d];
            player->set_stream_paused(hold);
            position = player->get_playback_position();
        }
        if (hold) {
            if (state.paused) pause_position_drift_ = std::max(pause_position_drift_, std::abs(position - state.paused_position));
            else if (spatial) ++pause_3d_;
            else ++pause_2d_loops_;
            state.paused_position = position;
            ++paused_voice_frames_;
            // Preserve fade progress and the backend-start grace across a long pause.
            state.started_at += std::max(0.0, delta);
            state.fade_started += std::max(0.0, delta);
        }
        state.paused = hold;
    }
    speech_player_->set_stream_paused(paused);
    if (paused && speech_stream_.active()) {
        const double position = speech_player_->get_playback_position();
        if (changed) ++pause_speech_;
        else pause_position_drift_ = std::max(pause_position_drift_, std::abs(position - speech_paused_position_));
        speech_paused_position_ = position;
        speech_started_at_ += std::max(0.0, delta);
        ++paused_speech_frames_;
    }
}

void BattleAudio::update_voices(const audio::Vec3& listener, const LiveSessionView& live) {
    const auto backend = event_backend();
    const auto latest = live.battle_frame().latest;
    // SND-19: a chain's source is independent of the primary's attachment. Check the
    // current snapshot before natural completion can release a removed source's assist.
    events_.cancel_missing_chains([&latest](const std::uint64_t source) {
        return latest && space::find_instance(*latest, source);
    });
    std::uint64_t local_bit = 0;
    if (latest) {
        const auto players = latest->players();
        for (std::size_t index = 0; index < players.size(); ++index) {
            if (players[index].player_id == live.local_player()) local_bit = std::uint64_t{1} << index;
        }
    }
    // SND-11/18: refresh queued attachments too; deletion cannot leave a delayed event behind.
    for (auto& [handle, state] : event_states_) {
        if (!events_.active(handle)) continue;
        if (state.loop_child != sim::invalid_entity_id && !live.snapshot_index().instance(state.loop_child)) {
            events_.stop(handle, backend);
            continue;
        }
        if (state.attached == sim::invalid_entity_id) continue;
        const auto* instance = latest ? space::find_instance(*latest, state.attached) : nullptr;
        const auto unit = live.unit_frame(state.attached);
        const bool dead_copy = state.reason == "spin_death";
        if (!instance && !(dead_copy && unit)) {
            events_.detach(state.attached, backend);
            if (state.engine_loop) ++engine_detached_;
            ++ambient_detached_;
            continue;
        }
        const bool fogged = instance && (instance->visible_to & local_bit) == 0U;
        const bool model_hidden = (!unit && !fogged) || (instance && instance->arrival
            && *instance->arrival < tactical::arrival_visible_frame);
        state.hidden = audio::attached_gain(silent_sources_.contains(state.attached), model_hidden,
                                             fogged, options_.admission.story_cinematic) == 0.0;
        if (unit) events_.set_position(handle, unit->position);
        else if (instance) events_.set_position(handle, {to_double(instance->fixed_transform.rows[0][3]),
            to_double(instance->fixed_transform.rows[1][3]), to_double(instance->fixed_transform.rows[2][3])});
    }
    for (std::size_t voice = 0; voice < voice_states_.size(); ++voice) {
        VoiceState& state = voice_states_[voice];
        if (!state.started) continue;
        if (state.paused) continue; // SND-13: retain the slot, chain and fade while playback is held.
        const bool three_d = audio::Voices::is_3d_voice(voice);
        // SND-61: ordinary ongoing 2D and attached 3D gain writes are capped;
        // fixed-position spatial voices retain the gain chosen at their start.
        const double gain = audio::speech_sfx_gain(state.volume, speech_stream_.active(), gui_dialog_);
        if (!three_d) {
            if (gain < state.volume) ++ducked_updates_;
            players_2d_[voice - audio::Voices::voices_3d]->set_volume_db(decibels(gain * events_.fade(state.event_handle)));
            continue;
        }
        const auto metadata = event_states_.find(state.event_handle);
        const bool hidden = metadata != event_states_.end() && metadata->second.hidden && state.attached != sim::invalid_entity_id;
        if (state.attached != sim::invalid_entity_id) {
            ++ambient_attached_updates_;
            if (hidden) ++ambient_hidden_updates_;
            if (hidden && state.engine_loop) ++engine_hidden_updates_;
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
        double fade = 1.0;
        if (state.engine_loop) {
            const double progress = state.fade_seconds > 0.0
                ? std::clamp((clock_ - state.fade_started) / state.fade_seconds, 0.0, 1.0) : 1.0;
            fade = progress;
            if (progress < 1.0) ++engine_fade_updates_;
        }
        const double current_gain = hidden ? 0.0
            : state.attached == sim::invalid_entity_id ? state.positional_gain : gain;
        if (state.attached != sim::invalid_entity_id && gain < state.volume) ++ducked_updates_;
        players_3d_[voice]->set_volume_db(decibels(current_gain * fade * events_.fade(state.event_handle) * audio::falloff_gain(distance, sfx->saturation_distance, space_)));
    }
    const auto chains = events_.service(event_delta_ms_, paused_, listener, random_, registry_, backend);
    std::erase_if(event_states_, [this](const auto& row) { return !events_.active(row.first); });
    for (const auto& chain : chains) {
        play(chain.event, std::nullopt, false,
            chain.runtime ? (chain.attack ? "response_assist_attack" : "response_assist_move") : "authored_chain",
            sim::invalid_entity_id, chain.attachment);
    }
}

void BattleAudio::stop_engine_loop(const sim::EntityId source, const audio::SfxEvent* event) {
    if (!event) return;
    const auto backend = event_backend();
    for (const auto& [handle, state] : event_states_) {
        if (!state.engine_loop || state.attached != source || state.event != event || !events_.active(handle)) continue;
        events_.stop(handle, backend, event->loop_fade_out_seconds);
        ++engine_stops_;
    }
}

void BattleAudio::engine_loops(const tactical::TacticalSnapshot& snapshot, const std::span<const sim::EntityId> visible) {
    const auto now = snapshot.completed_tick();
    const auto stamp = now + 1;
    for (const auto& instance : snapshot.instances()) {
        ++engine_visits_;
        const auto type = types_.find(instance.type_id);
        if (type == types_.end() || type->second.squadron || !instance.locomotor_speed_per_frame
            || (!type->second.engine_idle && !type->second.engine_moving)) continue;
        auto& state = engine_states_[instance.entity_id];
        state.seen = stamp;
        const auto& sounds = type->second;
        const bool online = !instance.durability || instance.durability->engines_online;
        const bool hidden = !std::binary_search(visible.begin(), visible.end(), instance.entity_id)
            || (instance.arrival && *instance.arrival < tactical::arrival_visible_frame);
        const auto point = audio::Vec3{to_double(instance.fixed_transform.rows[0][3]),
            to_double(instance.fixed_transform.rows[1][3]), to_double(instance.fixed_transform.rows[2][3])};
        const auto set = [&](bool& active, const bool enabled, const audio::SfxEvent* sound, const char* reason) {
            if (active == enabled) return;
            active = enabled; // SND-45: refusal/missing authoring does not restore the transition.
            ++engine_transitions_;
            if (enabled) play(sound, point, hidden, reason, sim::invalid_entity_id, instance.entity_id, true);
            else stop_engine_loop(instance.entity_id, sound);
        };
        if (!online) {
            set(state.moving, false, sounds.engine_moving, "engine_moving");
            set(state.idle, false, sounds.engine_idle, "engine_idle");
        } else {
            if (!state.online) set(state.idle, true, sounds.engine_idle, "engine_idle");
            // SND-67: ordinary stopped service updates zero speed too. Hyperspace uses a separate service.
            if (now > 0 && !instance.arrival) {
                const bool moving = to_double(*instance.locomotor_speed_per_frame) >= engine_idle_speed_ * 1.1;
                set(state.moving, moving, sounds.engine_moving, "engine_moving");
                set(state.idle, !moving, sounds.engine_idle, "engine_idle");
            }
        }
        state.online = online;
    }
    for (auto state = engine_states_.begin(); state != engine_states_.end();) {
        if (state->second.seen == stamp) { ++state; continue; }
        // SND-11: update_voices immediately detaches removed objects; retire their presentation state.
        state = engine_states_.erase(state);
        ++engine_retired_;
    }
    engine_peak_ = std::max<std::uint64_t>(engine_peak_, engine_states_.size());
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
        : cue.mode == audio::MusicDirector::Mode::victory ? "victory"
        : cue.mode == audio::MusicDirector::Mode::defeat ? "defeat"
        : cue.mode == audio::MusicDirector::Mode::none && summary_event_ ? "summary" : "ambient";
    if (music_log_.size() < log_limit) {
        music_log_.push_back(std::to_string(music_tick_) + " " + mode + " " + cue.event->name + " " + cue.file);
    }
    const auto record = [this](const std::string& message) {
        if (music_stream_log_.size() < log_limit) music_stream_log_.push_back(std::to_string(clock_) + " " + message);
    };
    // SND-72: retire before opening. A failed replacement cannot revive the old active stream.
    MusicTrack& previous = music_tracks_[music_current_];
    if (previous.player->get_stream().is_valid()) {
        MusicTrack& older = music_tracks_[1 - music_current_];
        if (older.player->get_stream().is_valid()) {
            record("close " + older.file);
            older.player->stop();
            older.player->set_stream({});
            older.playing = false;
        }
        record("retire " + previous.file + " level=" + std::to_string(previous.fade.level)
            + " seconds=" + std::to_string(cue.event->fade_out_previous_seconds));
        previous.fade.retire(cue.event->fade_out_previous_seconds);
        previous.playing = true; // an allocated, naturally ended stream still retires
        previous.player->set_volume_db(decibels(previous.volume * previous.fade.level));
        music_current_ = 1 - music_current_;
    }
    MusicTrack& next = music_tracks_[music_current_];
    const godot::Ref<godot::AudioStream> stream = music_stream(cue.file);
    if (!stream.is_valid()) {
        record("open_failed " + cue.file);
        return;
    }
    next.volume = cue.event->volume;
    next.fade.begin(cue.event->fade_in_seconds);
    next.file = cue.file;
    next.playing = true;
    next.player->set_stream(stream);
    next.player->set_volume_db(decibels(next.volume * next.fade.level));
    next.player->play();
    record("start " + cue.file);
}

void BattleAudio::update_music(const double delta) {
    for (std::size_t index = 0; index < music_tracks_.size(); ++index) {
        MusicTrack& track = music_tracks_[index];
        if (!track.playing) continue;
        const double previous_level = track.fade.level;
        track.fade.advance(delta);
        track.player->set_volume_db(decibels(track.volume * track.fade.level));
        // SND-72: retain the silent ending stream until replacement or teardown.
        if (previous_level > 0.0 && track.fade.ending && track.fade.level == 0.0 && music_stream_log_.size() < log_limit) {
            music_stream_log_.push_back(std::to_string(clock_) + " silent " + track.file);
        }
        if (index == music_current_ && !track.fade.ending && !track.player->is_playing()) {
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
    if (acknowledgement.cue != audio::CommandCue::none) {
        constexpr std::array<const char*, static_cast<std::size_t>(audio::CommandCue::count)> reasons{
            "", "command_attack", "command_attack_move", "command_guard", "command_move", "command_stop", "negative_feedback"};
        const auto index = static_cast<std::size_t>(acknowledgement.cue);
        if (index < command_cues_.size()) play(command_cues_[index], std::nullopt, false, reasons[index]);
        return;
    }
    // BA-20: the highest ranking selected unit speaks for the selection. A squadron's team
    // container has no response sounds or ranking fields of its own (BA-24): FoC resolves it to
    // its leading live craft, in roster order, for both the ranking category and the sound played
    // (debug build). A craft that has since died is skipped: it stays in roster order (BA-24 does
    // not track FoC's own leader reassignment), but `live_entities_` excludes it once it leaves the
    // tactical snapshot, ahead of the destruction event that would otherwise age it out of
    // `last_seen_` a tick or more later.
    const auto sounds_of = [&](const sim::EntityId entity) -> const TypeSounds* {
        const auto seen = last_seen_.find(entity);
        if (seen == last_seen_.end() || seen->second.owner != live.local_player() || !live_entities_.contains(entity)) return nullptr;
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
        return type == types_.end() ? nullptr : &type->second;
    };
    const TypeSounds* selected = nullptr;
    sim::EntityId chosen = sim::invalid_entity_id;
    int best_category = -1, best_ranking = -1;
    for (const auto entity : acknowledgement.units) {
        const auto* sounds = sounds_of(entity);
        if (!sounds) continue;
        int category = -1;
        for (std::size_t index = 0; index < rankings_.size() && category < 0; ++index) {
            for (const auto& name : sounds->categories) if (same(name, rankings_[index])) {
                category = static_cast<int>(index);
                break;
            }
        }
        if (category < 0) continue;
        const int ranking = sounds->ranking.value_or(25);
        // BA-20 retains the category threshold after a lower-ranking takeover.
        if (best_category == -1 || category < best_category) {
            best_category = category;
            best_ranking = ranking;
            selected = sounds;
            chosen = entity;
        } else if (best_ranking == -1 || ranking < best_ranking) {
            best_ranking = ranking;
            selected = sounds;
            chosen = entity;
        }
    }
    if (!selected) return;
    const TypeSounds& speaker = *selected;
    const bool group = acknowledgement.units.size() > 1;
    const audio::SfxEvent* line = nullptr;
    const char* kind = "select";
    switch (acknowledgement.kind) {
    case BattleInput::Acknowledgement::Kind::select:
        line = speaker.select;
        break;
    case BattleInput::Acknowledgement::Kind::move:
        {
        bool asteroid = false, nebula = false;
        const auto* motion = live.motion();
        const auto snapshot = live.battle_frame().latest;
        if (acknowledgement.destination && motion && snapshot) {
            const auto point = vec(*acknowledgement.destination);
            for (const auto& instance : snapshot->instances()) {
                const auto* footprint = motion->footprint(instance.type_id);
                if (!footprint || footprint->layer != tactical::SpaceLayer::static_object
                    || !footprint->obstacle || (!footprint->asteroid_field && !footprint->nebula)) continue;
                const auto& transform = instance.fixed_transform.rows;
                const double centre_x = to_double(transform[0][3]);
                const double centre_y = to_double(transform[1][3]);
                const double angle = std::atan2(to_double(transform[1][0]), to_double(transform[0][0]));
                const double ox = to_double(footprint->obstacle_offset.x);
                const double oy = to_double(footprint->obstacle_offset.y);
                const double dx = point[0] - centre_x - ox * std::cos(angle) + oy * std::sin(angle);
                const double dy = point[1] - centre_y - ox * std::sin(angle) - oy * std::cos(angle);
                const double radius = to_double(footprint->radius);
                if (dx * dx + dy * dy <= radius * radius) {
                    asteroid = asteroid || footprint->asteroid_field;
                    nebula = nebula || footprint->nebula;
                }
            }
        }
        line = audio::move_response(asteroid, nebula, group, speaker.move_asteroid,
                                   speaker.move_nebula, speaker.group_move, speaker.move);
        kind = "move";
        if (line && line == speaker.move_asteroid && asteroid) kind = "move_asteroid";
        else if (line && line == speaker.move_nebula && !asteroid && nebula) kind = "move_nebula";
        }
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
    const auto voice = play(line, std::nullopt, false, std::string("response_") + kind);
    const bool attack = acknowledgement.kind == BattleInput::Acknowledgement::Kind::attack;
    const bool move = acknowledgement.kind == BattleInput::Acknowledgement::Kind::move;
    if (!voice || !line || line->is_3d || !group || (!move && !attack)) return;
    // SND-22: the random-object helper excludes the primary, with 51 bounded retries;
    // eligibility of the one resulting other object is tested once, without reselection.
    sim::EntityId other = chosen;
    for (int attempt = 0; attempt < 51 && other == chosen; ++attempt) {
        const int index = random_.between(0, static_cast<int>(acknowledgement.units.size() - 1));
        other = acknowledgement.units[static_cast<std::size_t>(index)];
    }
    // SND-22 reads the random selected object's own type, without the BA-24 speaker proxy.
    const auto candidate = last_seen_.find(other);
    const auto primary = last_seen_.find(chosen);
    if (candidate == last_seen_.end() || primary == last_seen_.end()
        || candidate->second.owner != live.local_player() || !live_entities_.contains(other)) return;
    const auto assist = types_.find(candidate->second.type);
    if (assist == types_.end() || !audio::assist_eligible(other == chosen,
        candidate->second.type == primary->second.type, assist->second.vehicle_thief)) return;
    const auto* cue = attack ? assist->second.assist_attack : assist->second.assist_move;
    if (cue && !cue->is_3d) events_.chain(*voice, cue, attack, other);
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
            next[key] = {status.active, timed, status.active ? status.remaining_frames : 0U, status.started_tick};
            if (before != ability_memory_.end() && status.started_tick != 0 && status.started_tick != before->second.started) {
                if (status.kind == tactical::AbilityKind::harmonic_bomb || status.kind == tactical::AbilityKind::weaken_enemy) {
                    const auto visible = snapshot.visible_entities(local);
                    spawned_ability(instance, status, !std::binary_search(visible.begin(), visible.end(), instance.entity_id));
                }
            }
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

void BattleAudio::target_ability(const LiveSessionView::AbilityClick& click, const LiveSessionView& live) {
    // BA-53: source-object attachment; confirming a target is separate from arming the button.
    if (!click.activate || !click.targeted || click.units.empty()) return;
    const sim::EntityId source = click.units.front();
    const auto seen = last_seen_.find(source);
    if (seen == last_seen_.end() || seen->second.owner != live.local_player()) return;
    const auto type = types_.find(seen->second.type);
    if (type == types_.end()) return;
    const auto cue = type->second.ability_targets.find(click.ability);
    if (cue == type->second.ability_targets.end()) return;
    play(cue->second, seen->second.position, false, "ability_target", sim::invalid_entity_id, source);
}

void BattleAudio::spawned_ability(const tactical::TacticalInstance& instance, const tactical::AbilityStatus& status,
                                const bool hidden) {
    // BA-54: only the successful spawn route owns this effect (including script/AI starts).
    if (status.kind != tactical::AbilityKind::harmonic_bomb && status.kind != tactical::AbilityKind::weaken_enemy) return;
    const auto type = types_.find(instance.type_id);
    if (type == types_.end()) return;
    const auto cue = type->second.ability_targets.find(status.kind);
    if (cue == type->second.ability_targets.end() || cue->second == nullptr) return;
    // BA-54: the GUI acknowledgement may already have started the same event.
    for (std::size_t voice = 0; voice < voice_states_.size(); ++voice) {
        if (voices_.playing(voice) == cue->second) return;
    }
    const auto& rows = instance.fixed_transform.rows;
    const audio::Vec3 point{to_double(rows[0][3]), to_double(rows[1][3]), to_double(rows[2][3])};
    play(cue->second, point, hidden, "ability_spawn", sim::invalid_entity_id, instance.entity_id);
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
                        std::vector<LiveSessionView::AbilityClick> ability_clicks, const FixedCamera& camera, const double delta,
                        std::vector<LiveSessionView::ReinforcementFeedback> reinforcement_feedback,
                        std::vector<BattleEffects::TerminalSound> terminal_sounds) {
    if (released_) return;
    ++frames_;
    gui_dialog_ = live.time().paused(); // the viewer's pause banner is its modal dialog
    event_delta_ms_ = std::max(0.0, delta) * 1000.0;
    clock_ += delta;
    update_pause(live.time().paused(), delta);
    for (auto& warning : radar_warnings_) warning.age += std::max(0.0, delta);
    std::erase_if(radar_warnings_, [](const auto& warning) { return warning.age >= 4.0; });
    const LiveSessionView::BattleFrame& battle = live.battle_frame();
    if (!outcome_shown_ && live.battle_end()) {
        outcome_shown_ = true;
        const bool won = live.battle_end()->result == ui::BattleResult::victory;
        const bool hostile = !battle.latest || !battle.latest->outcome()
            || tactical::players_hostile(battle.latest->players(), live.local_player(), battle.latest->outcome()->winner);
        // WBF-38: allied winner gets win HUD audio; neutral observers get no result HUD audio.
        if (won || hostile) play(outcome_events_[won ? 0 : 1], std::nullopt, false, won ? "battle_victory" : "battle_defeat");
        if (music_ && battle.latest && battle.latest->outcome()) {
            const auto& outcome = *battle.latest->outcome();
            const bool exact_winner = outcome.winner == live.local_player();
            const auto loser = owners_.find(outcome.deciding_unit);
            // SND-54: the music equality test is distinct from the HUD's allied-win test.
            if (!exact_winner || loser != owners_.end()) {
                const std::string opposing = live.player_faction(exact_winner ? loser->second : outcome.winner);
                const std::string name = audio::tactical_music_event(tactical_music_fields_, exact_winner, opposing);
                const auto event = std::find_if(music_events_.begin(), music_events_.end(),
                    [&](const auto& entry) { return same(entry.name, name); });
                if (event != music_events_.end()) {
                    music_tick_ = outcome.decided_tick;
                    if (const auto cue = music_->result(exact_winner, &*event)) cue_music(*cue);
                }
            }
        }
    }
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
    for (const auto& cue : reinforcement_feedback) {
        const auto index = static_cast<std::size_t>(cue.kind);
        const audio::SfxEvent* sound = reinforcement_sounds_[index];
        if (cue.kind == LiveSessionView::ReinforcementFeedback::Kind::enroute) {
            const auto type = types_.find(cue.type);
            if (type != types_.end() && type->second.fleet_move) sound = type->second.fleet_move;
        }
        constexpr std::array reasons{"reinforcement_pane", "reinforcement_placement", "reinforcement_enroute", "reinforcement_cancelled"};
        play(sound, std::nullopt, false, reasons[index]);
    }
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
    // BA-53/54: the confirmation precedes spawn playback, so the active-event guard avoids doubling it.
    for (const auto& click : ability_clicks) target_ability(click, live);
    // The ticks this frame reaches: an event of tick t sounds once the frame presents t - 1 (as
    // the battle effects fire), up to the newest completed tick.
    const std::uint64_t latest = battle.latest->completed_tick();
    const auto reach = std::min<std::uint64_t>(latest, static_cast<std::uint64_t>(std::max(0.0, std::floor(battle.presented_tick))) + 1U);
    if (!ambient_begun_) {
        auto initial = live.snapshot_at(0);
        if (!initial) initial = battle.previous ? battle.previous : battle.latest;
        const auto visible = initial->visible_entities(local);
        ambient_moving(*initial, live, visible);
        engine_loops(*initial, visible);
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
        // SND-40: logical service precedes this frame's attack notifications.
        if (base_warning_countdown_ > 0) --base_warning_countdown_;
        // The bounded journal retains notification-time metadata across snapshot eviction.
        const auto record = std::lower_bound(battle.reached.begin(), battle.reached.end(), tick,
            [](const auto& entry, const std::uint64_t value) { return entry.tick < value; });
        if (record != battle.reached.end() && record->tick == tick) {
            for (const auto& notification : record->base_attacks) {
                if (base_warning_countdown_ != 0 || notification.attacker == notification.owner
                    || live.is_ally_of_local(notification.attacker) == live.is_ally_of_local(notification.owner)) continue;
                const auto type = types_.find(notification.type);
                if (type == types_.end() || !type->second.base
                    || (notification.owner != local && !(type->second.community_property && live.is_ally_of_local(notification.owner)))) continue;
                base_warning_countdown_ = base_warning_delay_frames_;
                if (base_under_attack_) {
                    play(base_under_attack_, std::nullopt, false, "base_under_attack");
                    radar_warnings_.push_back({vec(notification.position), 0.0});
                }
                if (base_warning_rows_.size() < log_limit) base_warning_rows_.push_back({tick, notification.target, base_under_attack_ != nullptr});
            }
        }
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
            engine_loops(*snapshot, visible);
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
                const auto involved = [&](const tactical::PlayerId owner, const sim::EntityId entity) {
                    if (owner == local) return true;
                    if (!live.is_ally_of_local(owner)) return false;
                    const auto identity = entity_types_.find(entity);
                    const auto type = identity == entity_types_.end() ? types_.end() : types_.find(identity->second);
                    return type != types_.end() && type->second.community_property;
                };
                // SND-51: either local-owned side or allied community property; no fog/camera test.
                if (involved(projectile.owner, projectile.shooter)
                    || (target_owner != owners_.end() && involved(target_owner->second, projectile.target))) attack = true;
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
        events_.stop(loop->second, event_backend());
        loop = pad_loops_.erase(loop);
    }
    // WPJ-35/37/40: only successful lifetime-effect receipts enter ordinary detonation audio.
    for (const auto& terminal : terminal_sounds) {
        const auto cue = terminal_detonations_.find(skirmish::type_id(terminal.projectile));
        if (cue != terminal_detonations_.end()) {
            play(cue->second, audio::Vec3{terminal.position[0], terminal.position[1], terminal.position[2]},
                false, terminal.death_payload ? "death_projectile_detonation" : "lifetime_detonation");
        }
    }
    for (const platform::LiveTickEvents& record : battle.reached) {
        // BA-85: one request at the countdown transition, independent of the
        // number of units hit by its blast and the presentation frame's step.
        const auto before_spawn = record.tick > 0 ? live.snapshot_at(record.tick - 1) : nullptr;
        if (before_spawn) for (const auto& spawn : before_spawn->ability_spawns()) {
            if (spawn.detonated || spawn.due + 1 != record.tick || !last_seen_.contains(spawn.source)) continue;
            if (const auto cue = projectile_detonations_.find(spawn.type); cue != projectile_detonations_.end()) {
                play(cue->second, vec(spawn.position), false, "projectile_detonation");
            }
        }
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
                // WPR-52 / EUS-25: relationship to local selects the line;
                // the upgraded station owner's faction supplies it. Neutral is silent.
                if (!live.battle_participant(event.player)) continue;
                const std::string owner_faction = live.player_faction(event.player);
                const auto faction = catalog_->resolve(owner_faction, data::Category::faction);
                if (faction) {
                    const auto field = event.player == live.local_player() ? "SFXEvent_Starbase_Upgraded"
                        : live.is_ally_of_local(event.player) ? "SFXEvent_Starbase_Ally_Upgraded" : "SFXEvent_Starbase_Enemy_Upgraded";
                    play(this->event(tag(faction.value(), field), owner_faction), std::nullopt, false, "station_upgraded");
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
                events_.detach(event.unit, event_backend());
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

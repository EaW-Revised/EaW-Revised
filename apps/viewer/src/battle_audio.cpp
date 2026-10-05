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
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_audio_detail;


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
    speech_player_ = memnew(godot::AudioStreamPlayer);
    speech_player_->set_bus(buses_[1].name);
    host_->add_child(speech_player_);
    // The viewer has no other sound: muting the master bus mutes the battle.
    options_.muted = options_.muted || audio_output_muted();
    godot::AudioServer::get_singleton()->set_bus_mute(0, options_.muted);
}

BattleAudio::~BattleAudio() { release(); }

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
    speech_player_->stop();
    speech_player_->queue_free();
    listener_viewport_->queue_free();
    players_3d_.clear();
    players_2d_.clear();
}

} // namespace eawr::presentation::godot_backend

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

namespace {

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
} // namespace

void BattleAudio::Spread::add(const double value) {
    low = count == 0 ? value : std::min(low, value);
    high = count == 0 ? value : std::max(high, value);
    sum += value;
    ++count;
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
           << ", \"stolen\": " << stolen_ << ", \"attacks\": " << attacks_
           << ", \"attached_moves\": " << attached_moves_;
    output << ", \"lifecycle\": {\"retained\": " << events_.size()
           << ", \"completed_loops\": " << events_.completed_loops() << "}";
    output << ", \"pause\": {\"active\": " << (paused_ ? "true" : "false")
           << ", \"transitions\": " << pause_transitions_ << ", \"voice_frames\": " << paused_voice_frames_
           << ", \"spatial\": " << pause_3d_ << ", \"loops_2d\": " << pause_2d_loops_
           << ", \"speech\": " << pause_speech_ << ", \"speech_frames\": " << paused_speech_frames_
           << ", \"position_drift\": " << pause_position_drift_ << "}";
    output << ", \"allocations\": {";
    bool allocation_first = true;
    for (const auto& [reason, row] : allocations_) {
        output << (allocation_first ? "" : ", ") << json(reason) << ": {\"requested\": " << row.requested
               << ", \"admitted\": " << row.admitted << ", \"allocated\": " << row.allocated
               << ", \"refused\": " << row.refused << ", \"stolen\": " << row.stolen
               << ", \"samples_requested\": " << row.samples_requested << ", \"samples_failed\": " << row.samples_failed
               << ", \"audible\": " << row.audible << "}";
        allocation_first = false;
    }
    output << "}";
    output << ", \"engines\": {\"idle_speed\": " << engine_idle_speed_
           << ", \"visits\": " << engine_visits_ << ", \"sources\": " << engine_states_.size()
           << ", \"peak_sources\": " << engine_peak_ << ", \"transitions\": " << engine_transitions_
           << ", \"stops\": " << engine_stops_ << ", \"retired\": " << engine_retired_
           << ", \"hidden_updates\": " << engine_hidden_updates_ << ", \"fade_updates\": " << engine_fade_updates_
           << ", \"detached\": " << engine_detached_ << "}";
    output << ", \"ambient\": {\"visits\": " << ambient_visits_
           << ", \"initialized\": " << ambient_initialized_ << ", \"retired\": " << ambient_retired_
           << ", \"timers\": " << ambient_timers_.size() << ", \"peak_timers\": " << ambient_peak_
           << ", \"due\": " << ambient_due_ << ", \"stationary\": " << ambient_stationary_
           << ", \"follower_skips\": " << ambient_follower_skips_
           << ", \"attached_updates\": " << ambient_attached_updates_
           << ", \"hidden_updates\": " << ambient_hidden_updates_ << ", \"detached\": " << ambient_detached_
           << ", \"rows\": [";
    bool ambient_first = true;
    for (const auto& row : ambient_rows_) {
        output << (ambient_first ? "" : ", ") << "{\"tick\": " << row.tick << ", \"unit\": " << row.unit
               << ", \"moving\": " << (row.moving ? "true" : "false")
               << ", \"hidden\": " << (row.hidden ? "true" : "false") << "}";
        ambient_first = false;
    }
    output << "]}";
    output << ", \"announcements\": {\"standalone_intro\": false, \"skirmish_hero_respawn\": false"
           << ", \"enemy_sighted\": " << (sightings_.enemy_seen() ? "true" : "false")
           << ", \"speech_queued\": " << speech_queued_ << ", \"speech_pending\": " << speech_queue_.size()
           << ", \"speech_completed\": " << speech_completed_ << ", \"speech_failed\": " << speech_failed_
           << ", \"speech_overflow\": " << speech_overflow_ << ", \"speech_active\": " << (speech_stream_.active() ? "true" : "false")
           << ", \"ducked_starts\": " << ducked_starts_ << ", \"ducked_updates\": " << ducked_updates_ << "}";
    counts("requested", requested_);
    output << ", \"base_warning\": {\"delay_frames\": " << base_warning_delay_frames_
           << ", \"countdown\": " << base_warning_countdown_ << ", \"radar_active\": " << radar_warnings_.size() << ", \"rows\": [";
    for (std::size_t index = 0; index < base_warning_rows_.size(); ++index) {
        const auto& row = base_warning_rows_[index];
        output << (index ? ", " : "") << "{\"tick\": " << row.tick << ", \"target\": " << row.target
               << ", \"radar\": " << (row.radar ? "true" : "false") << "}";
    }
    output << "]}";
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
               << ", \"spin_death\": " << name(sounds.spin_death)
               << ", \"ambient_moving\": " << name(sounds.ambient_moving)
               << ", \"engine_idle\": " << name(sounds.engine_idle)
               << ", \"engine_moving\": " << name(sounds.engine_moving)
               << ", \"fleet_move\": " << name(sounds.fleet_move)
               << ", \"ambient_min_delay_frames\": " << sounds.ambient_min_delay
               << ", \"ambient_max_delay_frames\": " << sounds.ambient_max_delay
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
        output << "}, \"ability_targets\": {";
        inner = true;
        for (const auto& [kind, cue] : sounds.ability_targets) {
            output << (inner ? "" : ", ") << json(tactical::to_string(kind)) << ": " << name(cue);
            inner = false;
        }
        output << "}}";
        first = false;
    }
    output << "}";
    output << ", \"music_mode\": "
           << json(!music_ ? "none" : music_->mode() == audio::MusicDirector::Mode::battle ? "battle"
                   : music_->mode() == audio::MusicDirector::Mode::ambient ? "ambient"
                   : music_->mode() == audio::MusicDirector::Mode::victory ? "victory"
                   : music_->mode() == audio::MusicDirector::Mode::defeat ? "defeat" : "none");
    strings("music", music_log_);
    strings("music_streams", music_stream_log_);
    strings("responses", response_log_);
    strings("abilities", ability_log_);
    const auto starts = [&output, this](const char* name, const bool announcements) {
        output << ", \"" << name << "\": [";
        bool first_row = true;
        for (const StartRow& row : start_rows_) {
            const bool announcement = row.reason.starts_with("sighting_") || row.reason.starts_with("speech_");
            if (announcement != announcements) continue;
            output << (first_row ? "" : ", ") << "{\"reason\": " << json(row.reason) << ", \"event\": " << json(row.event)
                   << ", \"sample\": " << json(row.sample) << ", \"tick\": " << row.tick << ", \"volume\": " << row.volume
                   << ", \"pitch\": " << row.pitch << ", \"attached\": " << row.attached
                   << ", \"localized\": " << (row.localized ? "true" : "false")
                   << ", \"bus\": " << json(row.reason.starts_with("speech_") || row.localized ? "EAWR_Voice" : "EAWR_SFX")
                   << ", \"position\": ";
            if (row.position) output << "[" << (*row.position)[0] << ", " << (*row.position)[1] << ", " << (*row.position)[2] << "]";
            else output << "null";
            output << "}";
            first_row = false;
        }
        output << "]";
    };
    starts("ability_starts", false);
    starts("announcement_starts", true);
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

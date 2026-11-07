#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace eawr::presentation::godot_backend {

// --eawr-perf-trace columns of the battle audio frame (bka_*): wall-clock laps in ms per frame that
// add up to audio_ms, and counters of the work behind them. Opt-in measurement only; with the trace
// off no clock is read and nothing is allocated.
namespace audio_trace {

// Each lap is the time since the previous mark, so the laps partition the frame in call order.
// The t_* laps run once per reached tick and are summed over the frame.
enum class Lap : std::size_t {
    head,           // frame start to update_pause: ready-buffer commit, radar warning ageing
    pause,          // update_pause
    outcome,        // present_outcome_cues
    listener,       // update_listener without update_voices
    v_chains,       // update_voices: cancel_missing_chains and the local seat lookup
    v_events,       // update_voices: the loop over event_states_ (attachments, hidden flags, positions)
    v_voices,       // update_voices: the loop over the 48 voices (gain and position writes)
    v_service,      // update_voices: events_.service (sort, stage machine), finished-event erase, authored chain plays
    v_playing,      // events_.service: the backend is-playing probes (Godot is_playing)
    v_start,        // events_.service: start_sample bookkeeping (allocation, maps, state) without the player calls
    v_start_godot,  // events_.service: start_sample player calls (bus, stream, position, play, pause)
    v_start_loop,   // events_.service: start_sample private loop stream copies of continuous cues
    music,          // reinforcement cues and update_music
    meter,          // meter_buses
    r_meta,         // remember_visible_units: snapshot metadata (live set, owners, types)
    r_units,        // remember_visible_units: the per-unit last-seen pass
    remember,       // remember_visible_units: the spinning-craft pass
    ability_click,  // target_ability for this frame's clicks
    t_init,         // present_reached_audio before the tick loop and per-tick loop setup
    t_base,         // per tick: base-attack notifications and snapshot lookup
    t_visible,      // per tick: visible_entities
    t_meta,         // per tick: entity_types_/owners_/stealth_hidden_ writes
    t_fade,         // per tick: alive list and sighting_fade_.advance, sighting prune
    t_sight,        // per tick: sighting loop
    t_toggle_main,  // per tick: toggle_abilities map rebuild and per-status checks (without spawned_ability)
    t_toggle_spawn, // per tick: spawned_ability (visible_entities copy and its play() work)
    t_toggle_leech, // per tick: toggle_abilities self-destruct and leech passes
    t_toggle_rest,  // per tick: toggle_abilities droids and switch sounds
    t_ambient,      // per tick: ambient_moving
    t_engine,       // per tick: engine_loops
    t_econ,         // per tick: economy cues, productions, population
    t_proj,         // per tick: projectile scan and shot sounds
    t_music,        // per tick: music tick
    combat,         // present_combat_audio
    respond,        // unit responses to gestures
    speech,         // ability voices and update_speech
    count
};
inline constexpr std::size_t lap_count = static_cast<std::size_t>(Lap::count);

inline constexpr std::array<const char*, lap_count> lap_names{
    "bka_head_ms", "bka_pause_ms", "bka_outcome_ms", "bka_listener_ms", "bka_v_chains_ms", "bka_v_events_ms", "bka_v_voices_ms", "bka_v_service_ms", "bka_v_playing_ms", "bka_v_start_ms", "bka_v_start_godot_ms", "bka_v_start_loop_ms", "bka_music_ms",
    "bka_meter_ms", "bka_r_meta_ms", "bka_r_units_ms", "bka_remember_ms", "bka_ability_click_ms", "bka_t_init_ms", "bka_t_base_ms",
    "bka_t_visible_ms", "bka_t_meta_ms", "bka_t_fade_ms", "bka_t_sight_ms", "bka_t_toggle_main_ms",
    "bka_t_toggle_spawn_ms", "bka_t_toggle_leech_ms", "bka_t_toggle_rest_ms", "bka_t_ambient_ms", "bka_t_engine_ms", "bka_t_econ_ms",
    "bka_t_proj_ms", "bka_t_music_ms", "bka_combat_ms", "bka_respond_ms", "bka_speech_ms"};

enum class Count : std::size_t {
    reached_ticks,    // ticks the frame presented
    unit_visits,      // instances scanned, summed over the reached ticks
    ability_rows,     // ability statuses scanned by toggle_abilities, summed over the reached ticks
    ability_active,   // of those, active
    ability_switched, // on/off flips that queued a switch sound
    ability_expired,  // timed abilities that ran out without a sound
    ability_spawned,  // spawned_ability calls (first cast, new start tick)
    leech_active,     // shield-leech endpoints collected
    play_calls,       // play() calls with a sound
    play_admitted,    // of those, admitted by the queue
    voices_started,   // voices the frame started (3D and 2D)
    continuous_starts, // of those, continuous cues (private loop stream copy)
    playing_probes,   // backend is-playing probes made by events_.service
    voices_live,      // started voices at the end of the frame
    pause_godot,      // player calls made by update_pause
    voice_godot,      // player calls made by update_voices
    meter_frames,     // stereo frames pulled from the bus captures
    last_seen_size,   // last_seen_ entries at the end of the frame
    entity_types_size, // entity_types_ entries at the end of the frame
    owners_size,      // owners_ entries at the end of the frame
    ambient_timers,   // ambient_timers_ entries at the end of the frame
    engine_states,    // engine_states_ entries at the end of the frame
    snapshot_units,   // instances of the newest snapshot
    count
};
inline constexpr std::size_t count_count = static_cast<std::size_t>(Count::count);

[[nodiscard]] constexpr bool all_named(const auto& names) {
    for (const char* name : names) {
        if (name == nullptr) return false;
    }
    return true;
}

inline constexpr std::array<const char*, count_count> count_names{
    "bka_reached_ticks", "bka_unit_visits", "bka_ability_rows", "bka_ability_active", "bka_ability_switched",
    "bka_ability_expired", "bka_ability_spawned", "bka_leech_active", "bka_play_calls",
    "bka_play_admitted", "bka_voices_started", "bka_continuous_starts", "bka_playing_probes", "bka_voices_live", "bka_pause_godot", "bka_voice_godot",
    "bka_meter_frames", "bka_last_seen_size", "bka_entity_types_size", "bka_owners_size", "bka_ambient_timers", "bka_engine_states",
    "bka_snapshot_units"};

// A running FNV-1a hash of the audio decisions of a run, so two builds can be compared decision by decision
// from the per-frame columns: play = every sound request and what the queue answered, stop = every voice the
// queue stopped or faded, voice = every voice start and every per-frame parameter written to a player.
class Digest final {
public:
    void add_u64(const std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) mix((value >> shift) & 0xffU);
    }
    void add_f64(const double value) {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof bits);
        add_u64(bits);
    }
    void add_text(const std::string_view text) {
        for (const char letter : text) mix(static_cast<unsigned char>(letter));
        mix(0xffU);
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
private:
    void mix(const std::uint64_t byte) { value_ = (value_ ^ byte) * 1099511628211ULL; }
    std::uint64_t value_{14695981039346656037ULL};
};
enum class Stream : std::size_t { play, stop, voice, count };
inline constexpr std::size_t digest_count = static_cast<std::size_t>(Stream::count);
inline constexpr std::array<const char*, digest_count> digest_names{"bka_play_digest", "bka_stop_digest", "bka_voice_digest"};

static_assert(all_named(lap_names), "every lap has a column name");
static_assert(all_named(count_names), "every counter has a column name");

// Inclusive time spent inside play(); it overlaps the laps above and is not part of their sum.
inline constexpr const char* play_ms_name = "bka_play_ms";

} // namespace audio_trace

} // namespace eawr::presentation::godot_backend

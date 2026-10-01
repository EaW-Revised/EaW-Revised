#pragma once

#include "battle_input.hpp"
#include "live_session_view.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/audio_effect_capture.hpp>
#include <godot_cpp/classes/audio_listener3d.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <tuple>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// The live battle's sound (P2-21, #84, docs/behaviour/battle-audio.md): FoC's SFXEvents for the
// shots, hits, deaths and hardpoint deaths the session publishes, the unit responses to the local
// player's selections and orders, and the ambient/battle music. Like the battle effects it only
// reads the published snapshots and event log and never feeds anything back, so the session's
// hashes are the same with or without it. `--eawr-audio off` mutes the output (the logic and its
// report still run); the interactive view has sound by default, a capture run is muted.
class BattleAudio final {
public:
    struct Options final {
        bool muted{};
    };
    BattleAudio(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog, Options options);
    ~BattleAudio();
    BattleAudio(const BattleAudio&) = delete;
    BattleAudio& operator=(const BattleAudio&) = delete;

    // Reads the SFXEvents, audio.xml, the command rankings, the music events and each unit type's
    // sound events. Never fails the view: what does not resolve is reported.
    void prepare(const units::UnitTables& tables, const sim::tactical::CombatTable& combat, std::string_view local_faction);
    // One presentation frame: the events of the ticks the frame reached, the gestures the player
    // made, the camera it renders and the frame's duration in seconds.
    void frame(const LiveSessionView& live, std::vector<BattleInput::Acknowledgement> acknowledgements,
               std::vector<LiveSessionView::AbilityClick> ability_clicks, const FixedCamera& camera, double delta);
    void release();
    // The report's "battle_audio" member, followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    // The sound events of one unit type (BA-13 to BA-17, BA-20 to BA-22).
    struct TypeSounds final {
        std::string name;
        std::map<std::uint32_t, const audio::SfxEvent*> fire;          // by weapon slot key
        std::map<std::uint32_t, const audio::SfxEvent*> detonate;      // Projectile_SFXEvent_Detonate
        std::map<std::uint32_t, const audio::SfxEvent*> detonate_armor; // ..._Reduced_By_Armor
        const audio::SfxEvent* death{};
        std::vector<const audio::SfxEvent*> hardpoint_deaths;           // HardPoints order
        std::vector<sim::math::Vec3> hardpoint_points;
        const audio::SfxEvent* select{};
        const audio::SfxEvent* move{};
        const audio::SfxEvent* attack{};
        const audio::SfxEvent* group_move{};
        const audio::SfxEvent* group_attack{};
        std::vector<std::string> categories;
        std::optional<int> ranking;
        // The unit's voice lines for switching an ability on and off (BA-52), by kind.
        struct AbilityVoice final {
            const audio::SfxEvent* activated{};
            const audio::SfxEvent* deactivated{};
        };
        std::map<sim::tactical::AbilityKind, AbilityVoice> ability_voices;
    };
    // A faction's toggle sounds for one ability (BA-50): what its own and its allies' switches play,
    // and what an enemy's play.
    struct Toggle final {
        const audio::SfxEvent* on{};
        const audio::SfxEvent* off{};
        const audio::SfxEvent* enemy_on{};
        const audio::SfxEvent* enemy_off{};
    };
    struct Sample final {
        godot::Ref<godot::AudioStream> stream;
        std::string cause;  // why it did not load
    };
    struct Seen final {
        std::array<double, 3> position{};
        double yaw_degrees{};
        sim::tactical::TypeId type{};
        sim::tactical::PlayerId owner{};
    };

    [[nodiscard]] const audio::SfxEvent* event(std::string_view name, const std::string& where);
    [[nodiscard]] const Sample& sample(const std::string& name);
    [[nodiscard]] godot::Ref<godot::AudioStream> music_stream(const std::string& file);
    // Starts `sfx` (a 3D one at `position`); `reason` keys the report's counts.
    void play(const audio::SfxEvent* sfx, std::optional<audio::Vec3> position, bool hidden, const std::string& reason);
    void respond(const BattleInput::Acknowledgement& acknowledgement, const LiveSessionView& live);
    // BA-50, BA-51: the toggle sound of an ability the snapshot at `tick` shows switched on or off.
    void toggle_abilities(const sim::tactical::TacticalSnapshot& snapshot, const LiveSessionView& live);
    // BA-52: the unit voice of a command bar press.
    void voice_ability(const LiveSessionView::AbilityClick& click, const LiveSessionView& live);
    [[nodiscard]] const std::map<sim::tactical::AbilityKind, Toggle>& toggles_of(const std::string& faction);
    void cue_music(const audio::MusicDirector::Cue& cue);
    void update_voices(const audio::Vec3& listener);
    void update_music(double delta);

    godot::Node3D* host_;
    const vfs::Vfs* filesystem_;
    const data::Catalog* catalog_;
    Options options_;
    audio::SfxRegistry registry_;
    audio::Space3D space_;
    audio::Voices voices_;
    audio::Random random_{0x84};
    std::vector<std::string> rankings_;
    std::map<sim::tactical::TypeId, TypeSounds> types_;
    std::map<std::string, Sample> samples_;
    std::vector<audio::MusicEvent> music_events_;
    std::unique_ptr<audio::MusicDirector> music_;
    godot::SubViewport* listener_viewport_{};
    godot::Camera3D* listener_camera_{};
    godot::AudioListener3D* listener_{};
    std::vector<godot::AudioStreamPlayer3D*> players_3d_;
    std::vector<godot::AudioStreamPlayer*> players_2d_;
    // The playing voices' base volume (event volume) and position, for the per-frame falloff.
    struct VoiceState final {
        double volume{};
        audio::Vec3 position{};
        bool started{};
        std::uint64_t frame{};  // the frame it started (the report's lifetimes)
        double started_at{};    // the clock when it started
    };
    double clock_{};  // seconds of frames presented
    std::array<VoiceState, audio::Voices::voices_3d + audio::Voices::voices_2d> voice_states_{};
    // Two music players crossfade (Fade_In_Seconds, Fade_Out_Previous_Seconds).
    struct MusicTrack final {
        godot::AudioStreamPlayer* player{};
        double volume{};       // the event's Volume_Percent
        double level{};        // 0..1 of the fade
        double fade_seconds{}; // this fade's length (0: at once)
        bool fading_out{};
        bool playing{};
    };
    std::array<MusicTrack, 2> music_tracks_{};
    std::size_t music_current_{};
    std::map<std::string, godot::Ref<godot::AudioStream>> music_streams_;
    std::uint64_t music_tick_{};
    bool music_begun_{};
    // Where each unit stood the last frame the local player saw it (explosions of units that left).
    // A dead unit's entry stays here until its destruction event reaches the report (BA-16 needs its
    // last position), so `respond()` checks `live_entities_`, not this map, for liveness.
    std::map<sim::EntityId, Seen> last_seen_;
    // Every entity this tick's snapshot still carries (BA-24): a squadron's dead craft leave this
    // set the tick they leave the instances, ahead of last_seen_'s own, event-driven pruning.
    std::set<sim::EntityId> live_entities_;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> metadata_snapshot_;
    std::map<sim::EntityId, sim::tactical::PlayerId> owners_;
    // The type of every unit the session has held (a hit's shooter may have died).
    std::map<sim::EntityId, sim::tactical::TypeId> entity_types_;
    audio::Vec3 listener_position_{};
    std::set<std::uint64_t> projectiles_seen_;
    // The abilities each unit showed active in the last snapshot read. A timed one is remembered as such
    // once it has read frames left, with its latest count: the snapshot of its last active tick reads
    // zero, like an untimed ability, so BA-51 tells a natural end from a switch-off by this history.
    struct AbilityMemory final {
        bool active{};
        bool timed{};
        std::uint32_t remaining{};
    };
    std::map<std::pair<sim::EntityId, sim::tactical::AbilityKind>, AbilityMemory> ability_memory_;
    bool ability_memory_started_{};
    std::map<std::string, std::map<sim::tactical::AbilityKind, Toggle>> toggles_;
    std::vector<std::string> ability_log_;
    // Every 2D start of the ability sounds with the tick it was presented at, for a clip's sound track
    // (#559: tools/rig/mux_clip_audio.py mixes the samples at these ticks).
    struct StartRow final {
        std::string reason;
        std::string event;
        std::string sample;
        double tick{};
        double volume{};
        double pitch{};
    };
    std::vector<StartRow> start_rows_;
    double presented_tick_{};
    std::uint64_t fired_through_{};
    bool released_{};
    // Report.
    std::uint64_t frames_{};
    std::map<std::string, std::uint64_t> requested_;                  // reason:event -> starts asked
    std::map<std::string, std::map<std::string, std::uint64_t>> results_; // reason -> result -> count
    std::map<std::string, std::uint64_t> played_samples_;
    std::map<std::string, std::string> missing_samples_;               // sample -> cause
    std::set<std::string> missing_events_;                             // "where: event"
    std::uint64_t max_voices_{};
    std::uint64_t stolen_{};
    std::uint64_t attacks_{};
    std::vector<std::string> music_log_;
    std::vector<std::string> response_log_;
    std::vector<std::string> problems_;
    std::string listener_text_;
    // Loudness (#443): each start's distance and gain by reason, how many frames the voices lived,
    // and the SFX, voice and music buses' levels as the mixer metered them each frame.
    struct Spread final {
        std::uint64_t count{};
        double sum{};
        double low{};
        double high{};
        void add(double value);
    };
    std::map<std::string, Spread> start_distance_;
    std::map<std::string, Spread> start_gain_db_;
    std::map<std::string, std::uint64_t> lifetimes_;
    struct BusLevel final {
        const char* name{};
        std::int32_t index{-1};
        std::uint64_t frames{};        // frames with sound on the bus (peak above -60 dBFS)
        double power{};                // the sum of those frames' squared peaks
        double peak{};                 // the loudest peak (linear)
        std::uint64_t clipped{};       // frames whose peak reached full scale
        godot::Ref<godot::AudioEffectCapture> capture;  // the bus's signal before its own slider
        std::uint64_t samples{};       // stereo frames captured
        double energy{};               // their summed squares (both channels)
    };
    std::array<BusLevel, 4> buses_{};  // sfx (3D), voice (2D), music, and their mix
    void end_voice(std::size_t voice);
    void meter_buses();
};

} // namespace eawr::presentation::godot_backend

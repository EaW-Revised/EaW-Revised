#pragma once

#include "battle_input.hpp"
#include "live_session_view.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/audio/announcements.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/ui/retreat_presentation.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/audio_effect_capture.hpp>
#include <godot_cpp/classes/audio_effect_panner.hpp>
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
#include <span>
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
        // MAT-18: the demo battle admits no sound event, queues no speech and starts no tactical
        // music, so nothing of it reaches the shared buses; no bus is muted, because menu music, clicks and
        // slider previews play on the same SFX and music buses. The refusals are counted
        // (attract_silenced()).
        bool attract_silent{};
        audio::MixLevels levels{};
        audio::Admission admission{};
    };
    BattleAudio(godot::Node3D& host, std::shared_ptr<const vfs::Vfs> filesystem, const data::Catalog& catalog, Options options);
    ~BattleAudio();
    BattleAudio(const BattleAudio&) = delete;
    BattleAudio& operator=(const BattleAudio&) = delete;

    // Reads the SFXEvents, audio.xml, the command rankings, the music events and each unit type's
    // sound events. Never fails the view: what does not resolve is reported.
    void prepare(const units::UnitTables& tables, const sim::tactical::CombatTable& combat, std::string_view local_faction,
                 std::set<sim::tactical::TypeId> first_types);
    // One presentation frame: the events of the ticks the frame reached, the gestures the player
    // made, the camera it renders and the frame's duration in seconds.
    void frame(const LiveSessionView& live, std::vector<BattleInput::Acknowledgement> acknowledgements,
               std::vector<LiveSessionView::AbilityClick> ability_clicks, const FixedCamera& camera, double delta,
               std::vector<LiveSessionView::ReinforcementFeedback> reinforcement_feedback = {},
               std::vector<BattleEffects::TerminalSound> terminal_sounds = {});
    void release();
    // RP-05: the local player pressed the retreat button and the request was refused.
    void retreat_refused(sim::tactical::RetreatRefusal refusal);
    void loading_complete();
    [[nodiscard]] bool briefing_queue_empty() const { return speech_queue_.size() == 0; }
    [[nodiscard]] bool campaign_briefing(std::string_view event_name);
    void set_mix_levels(const audio::MixLevels& levels);
    void set_admission(const audio::Admission& admission) { options_.admission = admission; }
    void set_attached_silent(sim::EntityId source, bool silent) {
        if (silent) silent_sources_.insert(source);
        else silent_sources_.erase(source);
    }
    struct RadarWarning final {
        audio::Vec3 position{};
        double age{};
    };
    [[nodiscard]] std::span<const RadarWarning> radar_warnings() const noexcept { return radar_warnings_; }
    // MAT-18: what the attract gate refused (speech, GUI and announcement events, queued speech) and
    // what started on the voice path anyway (queued speech played, a speech-class event allocated).
    // `started` stays 0 while Options::attract_silent holds. The 3D members are the same for the
    // positional events: refused before the queue, and any that started anyway. Event totals cover
    // every SFX category, including non-positional effects; the death subtotal proves that path ran.
    struct AttractSilence final {
        std::uint64_t refused{};
        std::uint64_t queued{};
        std::uint64_t started{};
        std::uint64_t sfx_3d_refused{};
        std::uint64_t sfx_3d_started{};
        std::uint64_t events_refused{};
        std::uint64_t events_admitted{};
        std::uint64_t events_started{};
        std::uint64_t deaths_2d_refused{};
    };
    [[nodiscard]] AttractSilence attract_silenced() const noexcept {
        return {attract_refused_, speech_queued_, speech_class_starts_, attract_3d_refused_, spatial_starts_,
                attract_events_refused_, events_admitted_, spatial_starts_ + two_d_starts_, attract_2d_deaths_refused_};
    }
    // The report's "battle_audio" member, followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    // MAT-18: SFX, queued speech and music share this admission policy. The front end owns
    // its own audio, so the battle can refuse every category without muting shared buses.
    [[nodiscard]] bool admits_battle_audio() const noexcept { return !options_.attract_silent; }
    void present_outcome_cues(const LiveSessionView& live, const LiveSessionView::BattleFrame& battle, const bool recovery_quiet);
    void update_listener(const LiveSessionView& live, const FixedCamera& camera);
    void remember_visible_units(const LiveSessionView& live, const LiveSessionView::BattleFrame& battle);
    void present_reached_audio(const LiveSessionView& live, const LiveSessionView::BattleFrame& battle, const sim::tactical::PlayerId local);
    void present_combat_audio(const LiveSessionView& live, const LiveSessionView::BattleFrame& battle, const sim::tactical::PlayerId local, const std::vector<BattleEffects::TerminalSound>& terminal_sounds);
    void expand_initial_audio_types(const units::UnitTables& tables, std::set<sim::tactical::TypeId>& first_types);
    void prepare_event_registry();
    template <typename Event>
    void prepare_music_and_faction_cues(const std::string_view local_faction, Event& event);
    void prepare_speech_registry();
    template <typename Event>
    void prepare_unit_cues(const units::UnitTables& tables, const sim::tactical::CombatTable& combat, const std::set<sim::tactical::TypeId>& first_types, const std::map<sim::tactical::TypeId, const audio::SfxEvent*>& spotted, const audio::SfxEvent* generic_spotted, Event& event, bool& binding_unit, bool& first_type);
    // The sound events of one unit type (BA-13 to BA-17, BA-20 to BA-22).
    struct TypeSounds final {
        std::string name;
        std::map<std::uint32_t, const audio::SfxEvent*> fire;          // by weapon slot key
        std::map<std::uint32_t, const audio::SfxEvent*> detonate;      // Projectile_SFXEvent_Detonate
        std::map<std::uint32_t, const audio::SfxEvent*> detonate_armor; // ..._Reduced_By_Armor
        std::map<std::uint32_t, const audio::SfxEvent*> ability_detonate;
        std::map<std::uint32_t, const audio::SfxEvent*> ability_detonate_armor;
        std::map<std::uint32_t, const audio::SfxEvent*> barrage_detonate;
        std::map<std::uint32_t, const audio::SfxEvent*> barrage_detonate_armor;
        std::map<std::uint32_t, const audio::SfxEvent*> charged_detonate;
        std::map<std::uint32_t, const audio::SfxEvent*> charged_detonate_armor;
        const audio::SfxEvent* blast_activate{};
        const audio::SfxEvent* retreat_start{};  // SFXEvent_Retreat_Start (WR-44, RP-02)
        const audio::SfxEvent* retreat_cancel{}; // SFXEvent_Retreat_Cancel (RP-03)
        const audio::SfxEvent* death{};
        const audio::SfxEvent* spin_death{};
        const audio::SfxEvent* asteroid_damage{};
        const audio::SfxEvent* ambient_moving{};
        const audio::SfxEvent* engine_idle{};
        const audio::SfxEvent* engine_moving{};
        const audio::SfxEvent* fleet_move{};
        int ambient_min_delay{150};
        int ambient_max_delay{300};
        const audio::SfxEvent* build_started{};
        const audio::SfxEvent* build_complete{};
        const audio::SfxEvent* special_ready{}; // WSW-34: source preference before faction fallback
        const audio::SfxEvent* build_cancelled{};
        struct SpeechEvent final {
            std::string name;
            std::vector<godot::Ref<godot::AudioStream>> streams;
            std::vector<std::string> files;
            double volume{1.0};
        };
        const SpeechEvent* build_underway_speech{};
        const SpeechEvent* build_completed_speech{};
        const SpeechEvent* build_stopped_speech{};
        bool announce_sighting{};
        bool type_sighted{};
        const audio::SfxEvent* spotted{};
        bool squadron{};
        bool base{};
        bool community_property{};
        const audio::SfxEvent* sold{};
        std::vector<const audio::SfxEvent*> hardpoint_deaths;           // HardPoints order
        std::vector<sim::math::Vec3> hardpoint_points;
        std::vector<std::string> hardpoint_types;
        std::map<std::string, const audio::SfxEvent*> hardpoint_loss;
        const audio::SfxEvent* all_weapons_lost{};
        const audio::SfxEvent* select{};
        const audio::SfxEvent* move{};
        const audio::SfxEvent* attack{};
        const audio::SfxEvent* group_move{};
        const audio::SfxEvent* group_attack{};
        const audio::SfxEvent* assist_move{};
        const audio::SfxEvent* assist_attack{};
        const audio::SfxEvent* move_asteroid{};
        const audio::SfxEvent* move_nebula{};
        bool vehicle_thief{};
        const audio::SfxEvent* stop{};
        const audio::SfxEvent* guard{};
        std::map<std::string, const audio::SfxEvent*> attack_hardpoint;
        std::vector<std::string> categories;
        std::optional<int> ranking;
        // The unit's voice lines for switching an ability on and off (BA-52), by kind.
        struct AbilityVoice final {
            const audio::SfxEvent* activated{};
            const audio::SfxEvent* deactivated{};
        };
        std::map<sim::tactical::AbilityKind, AbilityVoice> ability_voices;
        std::map<sim::tactical::AbilityKind, const audio::SfxEvent*> ability_targets;
        const audio::SfxEvent* leech_loop{};
        const audio::SfxEvent* corrupt_activate{};
        // WLD-12: LASER_DEFENSE's Zap_SFXEvent, started on the Crusader at each shot it shoots down.
        const audio::SfxEvent* point_defence_zap{};
        std::vector<const audio::SfxEvent*> heal_pulses;
        // WSD-11: the countdown's start sound (Selt_Destruct_SFXEvent_Start_Die) of a type with SELF_DESTRUCT.
        const audio::SfxEvent* self_destruct_start{};
        // WRB-12: a Redirect_Blaster handler's SFXEvent_Activate, played where it deflects a shot.
        const audio::SfxEvent* redirect_block{};
    };
    // BA-50 faction data; SND-26 owner policy only requests the local seat's on/off cues.
    struct Toggle final {
        const audio::SfxEvent* on{};
        const audio::SfxEvent* off{};
        const audio::SfxEvent* enemy_on{};
        const audio::SfxEvent* enemy_off{};
    };
    // BA-85: countdown-spawned projectiles retain their own detonation cue.
    std::map<sim::tactical::TypeId, const audio::SfxEvent*> projectile_detonations_;
    std::map<sim::tactical::TypeId, const audio::SfxEvent*> terminal_detonations_;
    struct DecodedSample final {
        std::uint32_t rate{};
        std::uint16_t channels{};
        std::vector<std::byte> pcm;
        std::string cause;
    };
    [[nodiscard]] DecodedSample decode_sample(const std::string& name) const;
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
    // A speech or music file through the VFS, WAVE or MP3 by content; a failure is counted in missing_samples_.
    [[nodiscard]] godot::Ref<godot::AudioStream> open_stream(const std::string& path, const std::string& key,
                                                             std::string_view kind);
    // Admits an event; the identifier is independent of later sample allocation (SND-18).
    std::optional<audio::EventQueue::Handle> play(const audio::SfxEvent* sfx, std::optional<audio::Vec3> position, bool hidden, const std::string& reason,
              sim::EntityId loop_child = sim::invalid_entity_id, sim::EntityId attached = sim::invalid_entity_id,
              bool engine_loop = false);
    audio::Start start_sample(const audio::EventQueue::Sample& cue);
    audio::EventQueue::Backend event_backend();
    audio::EventQueue events_;
    struct EventState final {
        std::string reason;
        sim::EntityId attached{};
        sim::EntityId loop_child{};
        bool engine_loop{};
        bool hidden{};
        const audio::SfxEvent* event{};
    };
    std::map<audio::EventQueue::Handle, EventState> event_states_;
    std::set<sim::EntityId> silent_sources_;
    double event_delta_ms_{};
    struct BuildSounds final {
        const audio::SfxEvent* started{};
        const audio::SfxEvent* loop{};
        const audio::SfxEvent* complete{};
        const audio::SfxEvent* captured{};
        const audio::SfxEvent* lost{};
        const audio::SfxEvent* sold{};
    };
    [[nodiscard]] const BuildSounds& build_sounds(const std::string& faction);
    std::map<std::string, BuildSounds> build_sounds_;
    std::array<const audio::SfxEvent*, 4> reinforcement_sounds_{};
    std::array<const audio::SfxEvent*, static_cast<std::size_t>(audio::CommandCue::count)> command_cues_{};
    std::map<sim::EntityId, audio::EventQueue::Handle> pad_loops_;
    std::map<sim::EntityId, audio::EventQueue::Handle> leech_loops_;
    // WSD-02/WSD-08: each running countdown's start sound; it stops when the countdown stops (a cancel
    // or the unit's end), as the debug build's disable path does.
    std::map<sim::EntityId, audio::EventQueue::Handle> self_destruct_sounds_;
    std::uint64_t self_destruct_sound_stops_{};
    void respond(const BattleInput::Acknowledgement& acknowledgement, const LiveSessionView& live);
    // BA-50, BA-51: the toggle sound of an ability the snapshot at `tick` shows switched on or off.
    void toggle_abilities(const sim::tactical::TacticalSnapshot& snapshot, const LiveSessionView& live);
    // BA-52: the unit voice of a command bar press.
    void voice_ability(const LiveSessionView::AbilityClick& click, const LiveSessionView& live);
    void target_ability(const LiveSessionView::AbilityClick& click, const LiveSessionView& live);
    void spawned_ability(const sim::tactical::TacticalInstance& instance, const sim::tactical::AbilityStatus& status,
                         bool hidden, const LiveSessionView& live);
    void arm_droids(const sim::tactical::TacticalSnapshot& snapshot, const LiveSessionView& live);
    [[nodiscard]] const std::map<sim::tactical::AbilityKind, Toggle>& toggles_of(const std::string& faction);
    void cue_music(const audio::MusicDirector::Cue& cue);
    // RP-01..RP-04: the faction cues of the space retreat, from the retreat state each snapshot publishes.
    void retreat_cues(const sim::tactical::TacticalSnapshot& snapshot, const LiveSessionView& live);
    [[nodiscard]] const ui::RetreatFactionPresentation& retreat_faction(const std::string& faction);
    [[nodiscard]] const TypeSounds* retreat_roster_type(const sim::tactical::TacticalSnapshot& snapshot,
        const sim::tactical::RetreatState& state, bool first_with_start_cue) const;
    std::map<std::string, ui::RetreatFactionPresentation> retreat_factions_;
    sim::tactical::RetreatState retreat_seen_;
    std::uint64_t retreat_cue_tick_{};
    bool retreat_cue_seen_{};
    std::string local_faction_;
    void update_voices(const audio::Vec3& listener, const LiveSessionView& live);
    void update_pause(bool paused, double delta);
    void update_music(double delta);
    void engine_loops(const sim::tactical::TacticalSnapshot& snapshot, std::span<const sim::EntityId> visible);
    void stop_engine_loop(sim::EntityId source, const audio::SfxEvent* event);
    struct EngineState final {
        bool online{};
        bool idle{};
        bool moving{};
        std::uint64_t seen{};
    };
    std::map<sim::EntityId, EngineState> engine_states_;
    double engine_idle_speed_{};
    std::uint64_t engine_visits_{};
    std::uint64_t engine_transitions_{};
    std::uint64_t engine_stops_{};
    std::uint64_t engine_retired_{};
    std::uint64_t engine_peak_{};
    std::uint64_t engine_hidden_updates_{};
    std::uint64_t engine_fade_updates_{};
    std::uint64_t engine_detached_{};
    void ambient_moving(const sim::tactical::TacticalSnapshot& snapshot, const LiveSessionView& live,
                        std::span<const sim::EntityId> visible);
    struct AmbientTimer final {
        std::uint64_t next{};
        std::uint64_t seen{};
    };
    struct AmbientMember final {
        sim::EntityId container{};
        sim::EntityId leader{};
    };
    struct AmbientRow final {
        std::uint64_t tick{};
        sim::EntityId unit{};
        bool moving{};
        bool hidden{};
    };
    std::map<sim::EntityId, AmbientTimer> ambient_timers_;
    bool ambient_begun_{};
    std::vector<sim::tactical::Squadron> ambient_roster_;
    std::map<sim::EntityId, AmbientMember> ambient_members_;
    audio::Random ambient_random_{0x1501}; // presentation-only delays, independent of voice admission
    std::vector<AmbientRow> ambient_rows_;
    std::uint64_t ambient_visits_{};
    std::uint64_t ambient_initialized_{};
    std::uint64_t ambient_retired_{};
    std::uint64_t ambient_peak_{};
    std::uint64_t ambient_due_{};
    std::uint64_t ambient_stationary_{};
    std::uint64_t ambient_follower_skips_{};
    std::uint64_t ambient_attached_updates_{};
    std::uint64_t ambient_hidden_updates_{};
    std::uint64_t ambient_detached_{};
    using SpeechEvent = TypeSounds::SpeechEvent;
    [[nodiscard]] const SpeechEvent* speech_event(std::string_view name);
    void production_cue(const SpeechEvent* speech, const audio::SfxEvent* fallback, std::string_view reason);
    void update_speech(bool paused);
    // Speech-class diagnostics (MAT-18 gates every class before admission).
    [[nodiscard]] static bool speech_class(const audio::SfxEvent& event) noexcept {
        return event.localized || event.hud_vo || event.unit_response_vo || event.ambient_vo || event.gui;
    }
    std::map<std::string, SpeechEvent> speech_events_;
    audio::SpeechQueue<SpeechEvent> speech_queue_;
    audio::SpeechStream speech_stream_;
    godot::AudioStreamPlayer* speech_player_{};
    audio::SpeechStream::Handle queued_speech_handle_{};
    double speech_started_at_{};
    bool queued_speech_started_{};
    std::uint64_t speech_queued_{};
    std::uint64_t speech_completed_{};
    std::uint64_t speech_failed_{};
    std::uint64_t speech_overflow_{};
    std::uint64_t attract_refused_{};
    std::uint64_t attract_3d_refused_{};
    std::uint64_t attract_events_refused_{};
    std::uint64_t attract_2d_deaths_refused_{};
    std::uint64_t events_admitted_{};
    std::uint64_t speech_class_starts_{};
    audio::SightingAnnouncements sightings_;
    const audio::SfxEvent* enemy_spotted_{};
    // WST-14: SFX_Stealth_Reveal, a 2D cue when a cloaked unit's detection reveals it; the units
    // whose cloak hid them in the last presented snapshot.
    const audio::SfxEvent* stealth_reveal_{};
    std::set<sim::EntityId> stealth_hidden_;
    audio::SightingService sighting_service_;
    space::UnitFade sighting_fade_;
    std::uint64_t sighting_pruned_at_{};
    std::uint64_t ducked_starts_{};
    std::uint64_t ducked_updates_{};
    bool gui_dialog_{};

    godot::Node3D* host_;
    // Own the mounted input through release and member teardown.
    std::shared_ptr<const vfs::Vfs> filesystem_;
    const data::Catalog* catalog_;
    std::map<sim::tactical::TypeId, const audio::SfxEvent*> beacon_sounds_;
    std::map<sim::tactical::PlayerId, std::uint64_t> beacon_heard_;
    Options options_;
    audio::SfxRegistry registry_;
    audio::Space3D space_;
    bool spatial_audio_available_{};
    std::uint64_t spatial_starts_{};
    std::uint64_t two_d_starts_{};
    audio::Voices voices_;
    audio::Random random_{0x84};
    std::vector<std::string> rankings_;
    std::map<sim::tactical::TypeId, TypeSounds> types_;
    // WSW-33/34: own-ready, own-fire, enemy-ready, enemy-fire; blanks retain their indices.
    std::array<std::vector<const audio::SfxEvent*>, 4> special_speech_;
    std::map<std::string, Sample> samples_;
    std::set<std::string> preloaded_samples_;
    bool samples_prepared_{};
    std::uint64_t synchronous_runtime_loads_{};

    std::vector<audio::MusicEvent> music_events_;
    std::vector<audio::Field> tactical_music_fields_;
    std::array<const audio::MusicEvent*, 2> summary_events_{}; // WBF-47: resolved win, lose
    const audio::MusicEvent* summary_event_{};
    std::size_t summary_next_file_{};
    bool summary_shown_{};
    std::array<const audio::SfxEvent*, 2> outcome_events_{}; // local faction's space win, lose
    bool outcome_shown_{};
    std::unique_ptr<audio::MusicDirector> music_;
    godot::SubViewport* listener_viewport_{};
    godot::Camera3D* listener_camera_{};
    godot::AudioListener3D* listener_{};
    std::vector<godot::AudioStreamPlayer3D*> players_3d_;
    std::vector<godot::AudioStreamPlayer*> players_2d_;
    std::array<godot::Ref<godot::AudioEffectPanner>, audio::Voices::voices_2d> panners_;
    std::array<std::int32_t, audio::Voices::voices_2d> pan_buses_{};
    // The playing voices' base volume (event volume) and position, for the per-frame falloff.
    struct VoiceState final {
        double volume{};
        audio::Vec3 position{};
        bool started{};
        std::uint64_t frame{};  // the frame it started (the report's lifetimes)
        double started_at{};    // the clock when it started
        sim::EntityId attached{sim::invalid_entity_id}; // SP-03: a spinning dead copy
        double positional_gain{};
        bool ambient{}; // SND-11: live-object attachment, separate from spin-away's dead copy
        bool engine_loop{};
        double fade_started{};
        double fade_seconds{};
        bool paused{};
        double paused_position{};
        audio::EventQueue::Handle event_handle{};
    };
    double clock_{};  // seconds of frames presented
    bool paused_{};
    std::uint64_t pause_transitions_{};
    std::uint64_t paused_voice_frames_{};
    std::uint64_t paused_speech_frames_{};
    std::uint64_t pause_3d_{};
    std::uint64_t pause_2d_loops_{};
    std::uint64_t pause_speech_{};
    double pause_position_drift_{};
    double speech_paused_position_{};
    std::array<VoiceState, audio::Voices::voices_3d + audio::Voices::voices_2d> voice_states_{};
    std::uint64_t attached_moves_{};
    // Two music players crossfade (Fade_In_Seconds, Fade_Out_Previous_Seconds).
    struct MusicTrack final {
        godot::AudioStreamPlayer* player{};
        double volume{};       // the event's Volume_Percent
        audio::MusicFade fade;
        std::string file;
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
    // WSD-11: the tick each unit the local player saw die was destroyed (its blast sounds when it was seen).
    std::map<sim::EntityId, std::uint64_t> seen_deaths_;
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
        std::uint64_t started{};
        bool positioned{};
    };
    std::map<std::pair<sim::EntityId, sim::tactical::AbilityKind>, AbilityMemory> ability_memory_;
    // WAB-75: a buzz-droid object type's own arming cue (Activate_SFX) and arming delay in frames,
    // and each live droid's first presented tick, so the cue starts once at its arming frame.
    struct DroidArming final {
        const audio::SfxEvent* cue{};
        std::uint64_t frames{};
    };
    std::map<sim::tactical::TypeId, DroidArming> droid_arming_;
    std::map<sim::EntityId, std::pair<std::uint64_t, bool>> droid_seen_;
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
        std::optional<audio::Vec3> position;
        sim::EntityId attached{};
        bool localized{};
    };
    std::vector<StartRow> start_rows_;
    double presented_tick_{};
    std::uint64_t fired_through_{};
    std::size_t economy_cues_delivered_{};
    std::size_t productions_delivered_{};
    bool population_full_{};
    std::map<std::string, const audio::SfxEvent*> arrival_sounds_;
    bool released_{};
    // Report.
    std::uint64_t frames_{};
    std::map<std::string, std::uint64_t> requested_;                  // reason:event -> starts asked
    std::map<std::string, std::map<std::string, std::uint64_t>> results_; // reason -> result -> count
    struct Allocation final {
        std::uint64_t requested{}, admitted{}, allocated{}, refused{}, stolen{}, audible{};
        std::uint64_t samples_requested{}, samples_failed{};
    };
    std::map<std::string, Allocation> allocations_;
    std::map<std::string, std::uint64_t> played_samples_;
    std::map<std::string, std::string> missing_samples_;               // sample -> cause
    std::set<std::string> missing_events_;                             // "where: event"
    std::uint64_t max_voices_{};
    std::uint64_t stolen_{};
    std::uint64_t attacks_{};
    const audio::SfxEvent* base_under_attack_{};
    std::uint64_t base_warning_delay_frames_{};
    std::uint64_t base_warning_countdown_{};
    struct BaseWarningRow final { std::uint64_t tick{}; sim::EntityId target{}; bool radar{}; };
    std::vector<BaseWarningRow> base_warning_rows_;
    std::vector<RadarWarning> radar_warnings_;
    std::vector<std::string> music_log_;
    std::vector<std::string> music_stream_log_;
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

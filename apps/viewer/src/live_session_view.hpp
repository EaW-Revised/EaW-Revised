#pragma once

#include "presentation_constants.hpp"

#include "battle_effects.hpp"
#include "battle_scoring.hpp"
#include "debris_props.hpp"
#include "space_environment.hpp"
#include "space_populate.hpp"

#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/space/snapshot_index.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/ui/battle_results.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/time_controls.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/skirmish/melee.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// `--eawr-live-session m2` on a space map with `--eawr-populate` (#80 part A): the viewer runs
// the authoritative tactical session of the M2 skirmish (plan/phase-2/m2-skirmish.md) and draws
// its units from the session's snapshots. The session steps on its own thread
// (platform::LiveSession, worker 0 of the #276 pool); this class only reads snapshots,
// interpolates between the two newest ticks and hides what the local player does not see
// (TacticalSnapshot::visible_entities). The local player's input goes through order_input()
// into the view's one CommandScheduler (UI-07), which the simulation thread takes from right
// before each step; --eawr-live-order is the debug hook a test injects scripted orders with.
//
// A capture run (no --eawr-camera-interactive) paces the session from the frame count: the
// presentation tick advances --eawr-live-step ticks per frame after the warm-up frames, so a
// frame always shows the same tick whatever the host's speed, and a capture tick must be one a
// frame shows. The interactive view runs the session in real time. When the simulation thread
// fails, the view keeps the last poses and reports the error; a capture run then fails.
//
// `--eawr-live-session replay --eawr-live-replay <file>` (#80) runs a recorded setup and its
// commands instead of the M2 start, such as the #74 duel sim_headless --scenario writes; its
// tables must be the content the replay names. `--eawr-live-reveal on` draws every unit while
// retaining the recorded session's sensors and fog rules.
//
// `--eawr-live-reveal on` also works with `--eawr-live-session m2` (a viewer debug aid, #80): it
// draws every instance regardless of the local player's fog there too, but presentation-only
// (space::interpolate_units' `reveal`), never by touching the m2 session's sensor content, so the
// live AI's decisions, the commands it issues and the session's headless hashes are unaffected.
// Selection and order input still read TacticalSnapshot ownership, not the reveal, so a revealed
// enemy cannot be selected or ordered.
class LiveSessionView final {
public:
    // #76 for the command bar (#454, docs/behaviour/foc-ability-buttons.md): the ability state of a
    // card unit from the latest snapshot's ability status (space-abilities AB-50; a squadron's
    // container stands for its craft, AB-15), and requests issued as ability commands through the
    // order scheduler, so they enter the replay like any order. An unsupported ability shows as
    // disabled. A targeted request (ION_CANNON_SHOT, #561) goes out once the battle input's
    // targeting gave it a target; without one it is refused.
    // #559: a command bar press of an ability's button or hotkey that reached the simulation (FoC
    // plays the unit's voice line for it, battle-audio BA-52); taken once per frame by the sound.
    struct AbilityClick final {
        sim::tactical::AbilityKind ability{sim::tactical::AbilityKind::none};
        bool activate{};                   // switched on (false: off)
        std::vector<sim::EntityId> units;  // the group's card units, in card order
        bool targeted{};                  // BA-53: a confirmed world target, not button arming
    };
    [[nodiscard]] std::vector<AbilityClick> take_ability_clicks() { return std::exchange(ability_clicks_, {}); }

    class Abilities final : public ui::AbilityState, public ui::AbilityCommands {
    public:
        explicit Abilities(LiveSessionView& view) noexcept : view_(view) {}
        [[nodiscard]] std::optional<ui::UnitAbilityState> state(sim::EntityId unit, std::uint32_t ability) const override;
        void request(const ui::AbilityRequest& request) override;
        [[nodiscard]] std::uint64_t issued() const noexcept { return issued_; }
        [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

    private:
        LiveSessionView& view_;
        std::uint64_t issued_{};
        std::uint64_t refused_{};
    };
    [[nodiscard]] Abilities& abilities() noexcept { return abilities_; }

    struct ScheduledOrder final {
        std::uint64_t tick{};
        sim::tactical::OrderKind kind{sim::tactical::OrderKind::stop};
        sim::EntityId unit{};
        std::vector<sim::EntityId> more{};  // #552: further units of the same command (`2+3+4`)
        sim::math::Vec3 point{};
        sim::math::Fixed amount{};  // damage only
        std::uint32_t hardpoint{0xffffffffU};  // damage only: the hardpoint index, or the hull
        sim::EntityId target{};     // attack, an attack-move or guard of a unit (#452), a targeted ability (#561)
        // ability only (#76): which ability, and on, off or its autofire.
        sim::tactical::AbilityKind ability{sim::tactical::AbilityKind::none};
        sim::tactical::AbilityAction action{sim::tactical::AbilityAction::activate};
    };
    // --eawr-live-input (#82): a player gesture the battle input replays through the viewer's
    // own input path at a presentation tick, so a test drives selection and orders as a player.
    // <tick>:<click|dclick|rclick|hover>:<unit=N|@x,y,z|screen=x,y>[+shift][+ctrl][+alt]
    // <tick>:<hover|click>:icon=N  (the pointer over squadron N's icon, #424)
    // WR-15: <tick>:press:hud=r_RRCC, hover:@x,y,0, release:@x,y,0 drives a held pool drag.
    // <tick>:<hover|click|rclick>:reticle=N:H  (#531: the pointer over the hardpoint reticle of
    //     unit N's hardpoint H, an index in its type's HardPoints list, or `first` for the lowest
    //     one drawn; the reticle must be drawn, so hover the unit first)
    // (hover moves the pointer there; unit=N of a squadron craft points at that craft)
    // <tick>:box:@x,y,z/@x,y,z[+shift]  (a left drag between two source points)
    // <tick>:box:screen=x,y/screen=x,y[+shift]  (the same between two viewport pixels)
    // <tick>:<click|dclick|hover>:card=N[+shift]  (#425: the HUD's unit card in slot N)
    // <tick>:key:<key name>[+shift][+ctrl][+alt]
    // <tick>:mdrag:<dx>,<dy>[+ctrl]  (a middle drag of dx, dy pixels from the viewport centre)
    // <tick>:mclick:centre[+ctrl]    (a middle click at the viewport centre)
    // <tick>:wheel:out             (one outward wheel click at the viewport centre)
    // <tick>:<click|rclick|hover>:ability=N  (#454: the N-th shown ability button)
    // <tick>:<click|rclick>:minimap=x,y, <tick>:box:minimap=x,y/minimap=x,y  (#455: a minimap
    //     point, x and y from -1 to 1 with +y up; a box is a left drag across the minimap)
    // <tick>:<click|rclick|hover>:hud=<pause|fast_forward|resume|quit>  (#459, #453: a time panel
    //     button, the pause banner's Resume Game or the end panel's Quit Game)
    // f<frame> in place of <tick> fires the gesture on that frame after the warm-up instead:
    // a paused battle's presented tick holds, so a gesture that plays again needs a frame.
    // With +ctrl the mouse gestures hold the Ctrl key around the button as a player's hand does:
    // the key's own press, an auto-repeat and its release are events too (#328 camera law).
    struct ScriptedInput final {
        std::uint64_t tick{};
        std::optional<std::uint64_t> frame; // f<frame>: fires on that shown frame instead
        std::string kind;
        std::string hud; // hud=<name>: a HUD control's centre
        std::optional<sim::EntityId> unit;
        std::optional<std::size_t> card; // #425: a unit card slot
        std::optional<std::size_t> ability; // #454: an ability button, by its index in the bar
        std::vector<std::array<double, 3>> points;
        std::array<double, 2> drag{};
        bool screen{};  // points are viewport pixels (x, y, 0), not source points
        bool icon{};    // `unit` is a squadron whose icon is the target (#424)
        std::optional<std::uint32_t> reticle; // #531: `unit`'s hardpoint whose reticle is the target
        bool minimap{}; // points are minimap points (x, y, 0) (#455)
        bool offset{};  // #665: points[0] is an offset from `unit`'s position (unit=N+@dx,dy,dz)
        std::string key;
        bool shift{};
        bool ctrl{};
        bool alt{};
    };
    // A unit the local player sees in the frame just drawn (#82).
    struct VisibleUnit final {
        sim::EntityId entity{};
        std::size_t ship{};
        sim::tactical::TypeId type{};
        sim::tactical::PlayerId owner{};
        bool own{};
        bool hostile{};
        std::array<double, 3> position{};
        double yaw{}; // degrees, (-180, 180]
        bool neutral{}; // WSU-63: owner-directed neutral-prop presentation policy
    };
    struct Options final {
        std::string fixture;                                  // --eawr-live-session (m2|skirmish|replay|melee)
        skirmish::FixtureOptions skirmish;                    // --eawr-skirmish-* (#908)
        std::filesystem::path replay_input;                   // --eawr-live-replay <file> (replay)
        // #601: --eawr-live-melee s|m|l and --eawr-live-melee-seed <n> (melee, skirmish/melee.hpp).
        std::optional<skirmish::MeleeSize> melee_size;
        std::optional<std::uint64_t> melee_seed;
        bool reveal{};                                        // --eawr-live-reveal on|off
        // Explicit capture-driver policy; interactive local battles require Begin by default.
        std::optional<bool> begin_barrier;                     // --eawr-live-begin manual|auto
        bool deploy_overlay{};                                // --eawr-live-deploy-overlay on|off (#563)
        // --eawr-live-ai on|off (#79): the FoC tactical AI runs the non-human lobby players of
        // the m2 fixture; on by default there. A replay already holds its AI's commands.
        std::optional<bool> ai;
        // --eawr-live-defend on|off (#427): every unit whose type has a DEFEND ability counts
        // as running it, which shows its shield shell (BP-22). Without it the shell follows
        // the unit's DEFEND in the snapshot (#76, AB-50).
        bool defend{};
        // --eawr-live-ability-demo on|off (#454, default off): the ability buttons' stand-in state
        // (docs/behaviour/foc-ability-buttons.md) shows the selection's first ability group active,
        // the second recharging (0.35) on autofire, the third disabled and the fourth on autofire,
        // so the buttons' looks can be seen before the simulation has ability state (#76).
        bool ability_demo{};
        bool shield_flash{};                                 // --eawr-live-shield-flash on|off (default off)
        // --eawr-live-purchase-slots <N> (#530, test hook): purchase model slots per buyable type,
        // 10 by default (space-purchasing PU-G25); a small N shows the reuse of a dead unit's slot
        // without buying past ten squadrons.
        std::uint32_t purchase_slots{10};
        // --eawr-live-late-orders on (#530, test hook): an --eawr-live-order may name a unit the
        // start does not hold (bought or launched later); it is given as the local player.
        bool late_orders{};
        std::optional<sim::tactical::PlayerId> player;        // --eawr-live-player
        std::vector<ScheduledOrder> orders;                   // --eawr-live-order, repeatable
        std::vector<ScriptedInput> inputs;                    // --eawr-live-input, repeatable
        std::vector<std::uint64_t> capture_ticks;             // --eawr-live-capture-ticks a,b,...
        // --eawr-live-follow-group <tick>:<entity>[,<entity>...], repeatable in tick order (#449 eye
        // checks): from <tick> the camera eases towards the centre of the named units still alive.
        struct FollowGroup final {
            std::uint64_t tick{};
            std::vector<sim::EntityId> entities;
        };
        std::vector<FollowGroup> follow_groups;
        // --eawr-live-capture-frames a,b,...: captures by shown frame after the warm-up (#459: a
        // paused battle shows one tick on many frames), named _f<frame>.
        std::vector<std::uint64_t> capture_frames;
        std::optional<std::uint64_t> end_tick;                // --eawr-live-ticks
        double ticks_per_frame{0.5};                          // --eawr-live-step
        std::optional<std::size_t> workers;                   // --eawr-live-workers
        particles::ParticleDetail particle_detail{};          // independent presentation settings
        std::optional<std::size_t> particle_workers;          // --eawr-live-particle-workers (#638)
        // --eawr-live-speed 0..4: the tactical speed setting (#459 TP-05, default 2).
        std::uint32_t speed_step{ui::default_speed_step};
        std::filesystem::path hashes_path;                    // --eawr-live-hashes <csv>
        std::filesystem::path replay_path;                    // --eawr-live-replay-out <file>
        // --eawr-live-fault-tick: a test hook that throws on the simulation thread right
        // before this tick runs (#316 review 2).
        std::optional<std::uint64_t> fault_tick;
        // --eawr-live-stall <tick>:<ticks>: a test hook for a presentation stall (#370 review 3).
        // Driven pacing only: the first frame past <tick> presents <ticks> ticks later than its
        // step says, as a frame after a hitch would. `start:<ticks>` stalls before the first
        // frame: the session has reached <ticks> when the view first draws (#370 re-review 2).
        struct Stall final {
            std::uint64_t tick{};
            std::uint64_t ticks{};
            bool start{};
        };
        std::optional<Stall> stall;
        // #81 test hooks: every death clone's Specific_Death_Anim_Type and
        // Death_Persistence_Duration (seconds) instead of its XML value.
        std::string death_anim_type;                          // --eawr-live-death-anim <TYPE>
        std::optional<double> death_persistence;              // --eawr-live-death-persistence <s>
        // --eawr-live-follow <unit> (#447, eye-check clips): every frame the tactical camera
        // looks at where this unit was last drawn (live, or spinning away), so it tracks the
        // unit smoothly and holds its last spot once it is gone.
        std::optional<sim::EntityId> follow;
        // #660 capture hook: follow the first homing projectile fired by this unit,
        // then hold its final camera focus. Uses BP-61's existing presented pose only.
        std::optional<sim::EntityId> follow_projectile;
        // --eawr-live-audio-pace on|off (#474, default off): driven pacing only. Caps the render
        // rate to logical_frames_per_second / ticks_per_frame so a frame's real duration matches
        // the battle's own tick rate, on every host. Without it, driven pacing advances ticks (and
        // so BattleAudio's requests) once per render frame with no floor on how fast that frame
        // arrives in real time; a host that renders faster reaches more ticks, and so schedules
        // more sound, in the same real second, so the sounds a slow host spreads over real seconds
        // instead genuinely overlap in Godot's real-time mixer on a fast one. BattleAudio's own
        // bookkeeping (voice slots, the 0.1 s grace) stays correct either way; only the real
        // Godot playback the report's bus levels measure depends on the schedule.
        bool audio_pace{};
        // --eawr-live-verify on|off: whether finish() replays the recorded command stream headlessly
        // and compares the hashes (report row headless_hashes_equal, else null). The replay costs about
        // as long as the battle did, on one thread, so an interactive (real-time) session skips it by
        // default: closing the window must not wait for it. Driven runs verify by default.
        std::optional<bool> verify;
        // Set by the map mode.
        bool real_time{};
        std::uint32_t warmup_frames{};
    };

    // Consumes one --eawr-live-* argument (and its value). False when `name` is not one;
    // `error` is set when the value is bad.
    static bool parse_argument(std::string_view name, const std::optional<std::string>& value, Options& options,
                               bool& value_used, std::string& error);

    explicit LiveSessionView(Options options);
    ~LiveSessionView();
    LiveSessionView(const LiveSessionView&) = delete;
    LiveSessionView& operator=(const LiveSessionView&) = delete;

    // Builds tick zero of the fixture from the mounted FoC view (skirmish::build_start) and
    // the placed ships that draw its units. False with `failure` set otherwise.
    [[nodiscard]] bool prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string_view map_path,
                               std::string& failure);
    [[nodiscard]] const std::vector<SpacePopulation::Options::PlacedShip>& placed_ships() const noexcept {
        return placed_ships_;
    }
    [[nodiscard]] const std::vector<std::uint32_t>& session_records() const noexcept { return session_records_; }
    // The local player's faction as the start names it (Empire, Rebel, ...); for a replay, the playable
    // faction whose ID the player carries. Empty before prepare().
    [[nodiscard]] std::string local_faction() const;
    // #455: a player's lobby colour (SK-12; nothing for a non-lobby player or in a replay), and its team.
    [[nodiscard]] std::optional<std::array<std::uint8_t, 3>> player_colour(sim::tactical::PlayerId player) const;
    [[nodiscard]] std::optional<sim::tactical::TeamId> team_of(sim::tactical::PlayerId player) const;
    // A start player's faction name (Rebel, Neutral, Pirates, ...); empty in a replay or for another ID.
    [[nodiscard]] std::string player_faction(sim::tactical::PlayerId player) const;

    // Starts the simulation thread and queues the --eawr-live-order orders.
    [[nodiscard]] bool start(std::string& failure);
    // The local player's order input on the view's CommandScheduler (UI-07; selection and
    // clicks come with #82). Main thread only; null before start().
    [[nodiscard]] ui::OrderInput* order_input() noexcept { return order_input_.get(); }
    [[nodiscard]] const ui::OrderInput* order_input() const noexcept { return order_input_.get(); }
    [[nodiscard]] const Options& options() const noexcept { return options_; }
    // The tick an order issued now is stamped for (UI-07 scheduler's open tick; the report's evidence).
    [[nodiscard]] std::uint64_t order_tick() const { return scheduler_ ? scheduler_->open_tick() : 0U; }
    [[nodiscard]] sim::tactical::PlayerId local_player() const noexcept { return player_; }
    // BA-71: battle parties come from retained setup metadata, including replays.
    [[nodiscard]] bool battle_participant(sim::tactical::PlayerId owner) const noexcept {
        if (!setup_) return false;
        for (const auto& player : setup_->players) {
            if (player.player_id == owner) return player.commandable();
        }
        return false;
    }
    [[nodiscard]] const skirmish::SkirmishStart* start_data() const noexcept { return start_ ? &*start_ : nullptr; }
    // The tick the last frame drew (fractional between ticks).
    [[nodiscard]] double presented_tick() const noexcept { return presented_tick_; }
    // The frames shown after the warm-up (scripted f<frame> gestures count these).
    [[nodiscard]] std::uint64_t shown_frames() const noexcept {
        return frames_ > options_.warmup_frames ? frames_ - options_.warmup_frames : 0U;
    }
    // #459: the time panel (docs/behaviour/tactical-time-controls.md). The buttons change only
    // when ticks run (TP-01, TP-02); each change enters the time track (TP-04). Main thread.
    [[nodiscard]] const ui::TimeControls& time() const noexcept { return time_; }
    void press_pause();
    void press_fast_forward();
    void resume();
    enum class Phase : std::uint8_t { loading, ready, running, quitting, results, returning };
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    // WBF-05/10: called after scene, camera and HUD finalization, never by the builder.
    void loading_complete();
    void begin();
    // #453 (docs/behaviour/battle-end.md): the local player's result from the first frame that
    // carries the outcome, and the frame that reached end_tick, where the session halts.
    struct BattleEnd final {
        ui::BattleResult result{ui::BattleResult::victory};
        std::uint64_t decided_tick{};
        std::uint64_t end_tick{};
        std::uint64_t shown_frame{};
        std::optional<std::uint64_t> ended_frame;
    };
    [[nodiscard]] const std::optional<BattleEnd>& battle_end() const noexcept { return battle_end_; }
    [[nodiscard]] const ui::BattleResults& results() const noexcept { return results_; }
    // WBF-43/48: active Quit records an intentional departure; results Exit returns to staging.
    void quit();
    // The units the last frame drew for the local player (#82 selection and orders).
    [[nodiscard]] const std::vector<VisibleUnit>& visible_units() const noexcept { return visible_; }
    struct RadarMemory final {
        sim::tactical::TypeId type{};
        sim::tactical::PlayerId owner{};
        std::array<double, 3> position{};
        double yaw{};
        std::array<double, 2> world_half_size{};
        bool capture_point{};
        bool retained_model{};
    };
    [[nodiscard]] const std::map<sim::EntityId, RadarMemory>& radar_memories() const noexcept { return radar_memories_; }
    [[nodiscard]] presentation::ui::MinimapLiveState radar_state(sim::EntityId entity) const {
        presentation::ui::MinimapLiveState state;
        const auto* instance = snapshot_index_.instance(entity);
        state.alive = instance != nullptr;
        state.limbo = instance && instance->arrival && *instance->arrival < sim::tactical::arrival_visible_frame;
        state.model_hidden = !pad_visible(entity);
        state.radar_faded = !options_.reveal && !fade_.opacity(entity).has_value();
        state.replay = options_.fixture == "replay";
        state.locally_visible = options_.reveal || std::binary_search(snapshot_index_.visible().begin(),
            snapshot_index_.visible().end(), entity);
        return state;
    }
    [[nodiscard]] const sim::tactical::PadView* pad_view(sim::EntityId entity) const noexcept;
    [[nodiscard]] bool pad_visible(sim::EntityId entity) const noexcept;
    [[nodiscard]] bool pad_action_allowed(sim::EntityId entity) const;
    [[nodiscard]] sim::EntityId pad_selection_target(sim::EntityId entity) const noexcept;
    [[nodiscard]] const sim::tactical::ConstructionState* pad_construction(sim::EntityId child) const noexcept;
    // #558: what the ticks completed after `after` cost the simulation thread (empty before the
    // session runs); the performance overlay's tick-cost source.
    [[nodiscard]] std::vector<platform::LiveTickCost> tick_costs_after(std::uint64_t after) const;
    // #535: the unit's fog fade opacity this frame (space-fog-presentation.md FW-16 to FW-18); 1
    // when it is not fading (or the fade is off, --eawr-live-reveal).
    [[nodiscard]] float unit_opacity(const sim::EntityId entity) const noexcept {
        return fade_.opacity(entity).value_or(1.0F);
    }
    // Whether every entity the session holds is still standing (destroyed units leave it).
    [[nodiscard]] const std::vector<sim::EntityId>& alive_units() const noexcept { return snapshot_index_.alive(); }
    [[nodiscard]] std::span<const sim::EntityId> unfogged_map_props() const noexcept { return unfogged_map_props_; }
    // One presentation frame: poses the population's live ships and says whether to capture.
    // After a simulation failure it keeps the last poses and carries the error instead. A unit
    // the session destroyed is replaced by its death clone playing its death clip (#81).
    [[nodiscard]] core::Result<SpaceLiveUpdate> frame(SpacePopulation& population, GodotRenderer& renderer,
                                                      double delta);
    // #421: where death clone `ship` stands at presented tick `tick` and its death clip's frame
    // there, as frame() draws it. Nullopt when the ship is no clone frame() shows, or when at
    // that tick it had not appeared yet or had faded out. `death` is empty for a clone whose clip
    // did not start (it keeps its pose).
    struct CloneFrame final {
        SpacePopulation::LivePose pose;
        std::optional<animation::DeathFrame> death;
    };
    [[nodiscard]] std::optional<CloneFrame> clone_frame(const SpacePopulation& population, std::size_t ship,
                                                        double tick) const;
    // #429: retires the ships of the clones frame() stopped drawing. The map mode calls it once
    // the unit emitters have run the frame's samples, a stall's catch-up included, in which such
    // a clone may still have stood; frame() calls it first in case the last frame did not.
    void retire_clones(SpacePopulation& population, GodotRenderer& renderer);
    // Stops the session, checks its hashes against a headless run of its recording and writes
    // the requested hash and replay files. Idempotent.
    void finish();
    // #615: once the simulation thread failed, writes the session's replay through the failed
    // tick beside Godot's log, so the failure replays headless. Once per session.
    void save_failure_replay();
    // The report's "live_session" member, followed by ",\n".
    void write_report(std::ostream& output) const;

    // What the battle effects (#80) read after each frame(): the two snapshots the poses
    // interpolate, the blend, the events of the ticks newly reached by this frame (oldest first,
    // from the session's event log, so a stall longer than the snapshot history loses none) and
    // where the units the local player sees stand.
    struct BattleFrame final {
        std::shared_ptr<const sim::tactical::TacticalSnapshot> previous;
        std::shared_ptr<const sim::tactical::TacticalSnapshot> latest;
        double alpha{};
        std::vector<platform::LiveTickEvents> reached;
        double presented_tick{};
        // #494: the local player's fog cells after `latest`'s tick, or null in a battle without fog.
        std::shared_ptr<const platform::LiveFog> fog;
    };
    void trace_frames(bool enabled) noexcept { trace_frames_ = enabled; }
    [[nodiscard]] double bookkeeping_ms() const noexcept { return bookkeeping_ms_; }
    [[nodiscard]] double pose_ms() const noexcept { return pose_ms_; }
    [[nodiscard]] double opacity_ms() const noexcept { return opacity_ms_; }
    [[nodiscard]] double tick_wait_ms() const noexcept { return tick_wait_ms_; }
    [[nodiscard]] double live_frame_ms() const noexcept { return live_frame_ms_; }
    [[nodiscard]] double session_tail_ms() const noexcept { return session_tail_ms_; }
    [[nodiscard]] double clip_pose_ms() const noexcept { return clip_pose_ms_; }
    [[nodiscard]] const space::SnapshotIndex& snapshot_index() const noexcept { return snapshot_index_; }
    void set_pose_workers(const particles::StepExecutor* workers) noexcept { pose_workers_ = workers; }
    [[nodiscard]] const BattleFrame& battle_frame() const noexcept { return battle_frame_; }
    [[nodiscard]] std::optional<BattleEffects::UnitFrame> unit_frame(sim::EntityId entity) const;
    // The session's snapshot of `tick` while its history still holds it (#406: the unit emitters
    // place each sample a stalled frame catches up on at that sample's own tick); null otherwise.
    [[nodiscard]] std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(std::uint64_t tick) const;
    // #425: the squadron `craft` flies in (its container and launch roster), or null. #518: a
    // squadron a spawner launches registers when a snapshot first lists it, like the setup's; a
    // craft keeps its squadron for life.
    [[nodiscard]] const sim::tactical::Squadron* squadron_of(sim::EntityId craft) const noexcept;
    // #530 (docs/behaviour/space-purchasing.md PU-60 to PU-68): the session's economy rules (empty
    // in a battle without them), the local player's economy in the latest snapshot, and the
    // command bar's buy, cancel and reinforce requests. They go through the order scheduler like any
    // order, so they reach the simulation at a tick boundary and enter the replay.
    [[nodiscard]] const sim::tactical::EconomyRules& economy() const noexcept { return economy_; }
    [[nodiscard]] const sim::tactical::EconomyView* local_economy() const noexcept;
    [[nodiscard]] std::uint64_t reinforcement_notifications() const noexcept { return reinforcement_notifications_; }
    [[nodiscard]] double reinforcement_notification_tick() const noexcept { return reinforcement_notification_tick_; }
    [[nodiscard]] bool build_allowed(const sim::tactical::BuildOption& option) const;
    [[nodiscard]] ui::BuildOptionState build_menu_state(const sim::tactical::BuildOption& option) const;
    // The local player's faction ID in the setup (the station menus' key), or nothing.
    [[nodiscard]] std::optional<sim::tactical::FactionId> local_faction_id() const noexcept;
    [[nodiscard]] bool hardpoint_repair_allowed(sim::EntityId station, std::uint32_t hardpoint) const;
    bool repair_hardpoint(sim::EntityId station, std::uint32_t hardpoint);
    bool buy(sim::EntityId station, sim::tactical::TypeId type);
    [[nodiscard]] bool pad_sale_allowed(sim::EntityId child, bool single_step = false) const;
    bool sell_pad_structure(sim::EntityId child, bool single_step = false);
    bool cancel_build(sim::tactical::BuildQueue queue, std::uint64_t entry_id);
    bool reinforce(sim::tactical::TypeId type, const sim::math::Vec3& point);
    // WR-X01: EAWR extension, wheel up adds 15 degrees; the local player's choice lasts this battle.
    bool rotate_reinforcement(double detents);
    [[nodiscard]] sim::math::Fixed reinforcement_facing() const noexcept;
    struct ReinforcementFeedback final {
        enum class Kind : std::uint8_t { pane, placement, enroute, cancelled };
        Kind kind{};
        sim::tactical::TypeId type{};
    };
    void reinforcement_feedback(ReinforcementFeedback::Kind kind, sim::tactical::TypeId type = {});
    [[nodiscard]] std::vector<ReinforcementFeedback> take_reinforcement_feedback() {
        return std::exchange(reinforcement_feedback_, {});
    }
    [[nodiscard]] bool reinforcement_allowed() const noexcept;
    [[nodiscard]] bool reinforcement_room(sim::tactical::TypeId type) const noexcept;
    // TM-10: a drop made while paused waits for the next tick that runs. Until the snapshot shows
    // it applied, the reserve pane leaves its unit out and counts its population, so the same
    // reserve unit cannot be dropped twice.
    [[nodiscard]] std::vector<sim::tactical::TypeId> reinforcement_pool() const;
    [[nodiscard]] std::uint32_t pending_reinforcement_population() const;
    [[nodiscard]] std::size_t pending_reinforcements() const;
    // WR-13: preview pose input only; simulation is queried through its nonblocking platform seam.
    void placement_preview(std::optional<sim::tactical::TypeId> type, std::optional<sim::math::Vec3> point);
    [[nodiscard]] bool placement_valid() const noexcept { return preview_valid_ && reinforcement_allowed(); }
    struct EconomyRequests final {
        std::uint64_t buys{};
        std::uint64_t cancels{};
        std::uint64_t reinforcements{};
        std::uint64_t refused{};
    };
    [[nodiscard]] const EconomyRequests& economy_requests() const noexcept { return economy_requests_; }
    // The unit and combat tables the session runs with (prepare() loaded them).
    [[nodiscard]] const units::UnitTables* tables() const noexcept { return tables_ ? &*tables_ : nullptr; }
    [[nodiscard]] const sim::tactical::CombatTable* combat() const noexcept { return content_ ? &content_->combat : nullptr; }
    [[nodiscard]] const sim::tactical::MotionTable* motion() const noexcept { return content_ ? &content_->motion : nullptr; }
    [[nodiscard]] const sim::tactical::DurabilityTable* durability() const noexcept {
        return content_ ? &content_->durability : nullptr;
    }
    // #424: the squadron (team container) of each craft of the registered squadrons (the start's
    // and, #518, the launched ones), and their craft.
    [[nodiscard]] const std::map<sim::EntityId, sim::EntityId>& squadron_of() const noexcept { return squadron_of_; }
    [[nodiscard]] const std::map<sim::EntityId, std::vector<sim::EntityId>>& squadron_members() const noexcept {
        return squadron_members_;
    }
    // #632 (foc-battle-world-ui WU-37, WU-38): whether a squadron came out of a hangar after tick
    // zero, and whether a player is an ally of the local player (the flag shows on allies' icons).
    [[nodiscard]] bool squadron_launched(sim::EntityId container) const;
    [[nodiscard]] bool is_ally_of_local(sim::tactical::PlayerId owner) const;
    // #391: sets up the breakoff props of the session's units as placed ships (after prepare(),
    // before placed_ships() is composed); every frame() then spawns and poses them.
    void attach_debris(DebrisProps& debris);
    // #456: the model projectiles' slots join the placed ships (after attach_debris, before
    // placed_ships() is composed); every frame() then poses the model projectiles in flight.
    void attach_projectile_models(BattleEffects& effects);

private:
    [[nodiscard]] bool prepare_m2(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure);
    [[nodiscard]] bool prepare_replay(const vfs::Vfs& filesystem, const data::Catalog& catalog,
        std::string_view map_path, std::string& failure);
    // #459: hands the time panel's state to the session (pause, target rate).
    void apply_time();

    Options options_;
    std::optional<skirmish::SkirmishStart> start_;
    // #79: the FoC AI beside the world (m2 with the AI on), and the players it runs.
    std::shared_ptr<const platform::LiveScripts> ai_scripts_;
    std::vector<sim::tactical::PlayerId> ai_players_;
    // --eawr-live-ai has no effect outside the live m2/skirmish starts (report row + warning).
    bool ai_flag_ignored_{};
    std::optional<sim::tactical::TacticalSetup> setup_;
    std::optional<sim::tactical::TacticalReplay> replay_;
    std::optional<units::UnitTables> tables_;
    std::map<sim::tactical::TypeId, const units::UnitType*> presentation_types_;
    std::map<sim::EntityId, sim::tactical::ConstructionState> presented_construction_;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> construction_snapshot_;
    std::map<sim::EntityId, sim::EntityId> construction_successors_;
    std::map<sim::EntityId, std::array<float, 3>> pad_tints_;
    std::map<sim::EntityId, std::uint64_t> pad_empty_since_; // WBP-32 presentation epochs
    std::optional<skirmish::SessionContent> content_;
    sim::tactical::VictoryRules victory_; // #77: the start's victory rules
    sim::tactical::EconomyRules economy_; // #530: the start's economy
    EconomyRequests economy_requests_;
    // The session's command source reads the scheduler on the simulation thread: both are
    // declared before session_, which is stopped and destroyed first.
    std::unique_ptr<ui::CommandScheduler> scheduler_;
    std::unique_ptr<ui::OrderInput> order_input_;
    std::unique_ptr<platform::LiveSession> session_;
    std::vector<SpacePopulation::Options::PlacedShip> placed_ships_;
    std::vector<std::uint32_t> session_records_;
    std::vector<sim::EntityId> unfogged_map_props_;
    std::vector<sim::EntityId> draw_visibility_;
    std::uint64_t reinforcement_notifications_{};
    std::vector<ReinforcementFeedback> reinforcement_feedback_;
    double reinforcement_notification_tick_{};
    std::map<sim::EntityId, std::size_t> ship_of_entity_;
    // #79: model slots for the craft the SK-23 launches bring in after tick zero, composed with
    // the start. A launched craft takes the first free slot of its type when it first shows,
    // and with it the slot's death clone (#458), if its type has one.
    struct LaunchSlot {
        sim::tactical::TypeId type{};
        std::size_t ship{};
        bool bound{};
        std::optional<std::size_t> clone;  // index in launch_clones_
        sim::tactical::TypeId required_station{};
        sim::tactical::PlayerId station_owner{};
    };
    std::vector<LaunchSlot> launch_slots_;
    struct PlacementClone {
        sim::tactical::TypeId type{};
        std::size_t ship{};
        sim::math::Vec3 offset{};
        sim::math::Fixed layer_z{};
    };
    std::vector<PlacementClone> placement_clones_;
    std::optional<sim::tactical::TypeId> preview_type_;
    std::optional<sim::math::Fixed> deployment_facing_;
    std::optional<sim::math::Vec3> preview_point_;
    bool preview_valid_{};
    struct PendingReinforcement final {
        std::uint64_t tick{};  // the tick its command was stamped for
        sim::tactical::TypeId type{};
    };
    std::vector<PendingReinforcement> pending_reinforcements_;
    [[nodiscard]] bool reinforcement_pending(const PendingReinforcement& drop) const noexcept;
    [[nodiscard]] std::uint32_t population_of(sim::tactical::TypeId type) const noexcept;
    std::array<std::array<float, 3>, 2> preview_colours_{};
    std::uint64_t preview_frames_{};
    std::uint64_t preview_queries_{};
    std::optional<std::uint64_t> preview_checked_tick_;
    std::vector<std::string> preview_rows_;
    std::size_t slots_released_{};  // slots freed by a unit leaving the snapshot (report: slots_released)
    std::map<sim::EntityId, std::size_t> launched_ship_of_entity_;
    std::map<sim::EntityId, sim::tactical::PlayerId> owner_of_entity_;
    std::map<sim::EntityId, sim::EntityId> squadron_of_;
    std::map<sim::EntityId, std::vector<sim::EntityId>> squadron_members_;
    std::map<sim::EntityId, sim::tactical::Squadron> squadrons_;  // by container, as first registered
    std::map<sim::EntityId, std::uint64_t> squadron_seen_;       // the tick of the snapshot that first listed it
    std::map<sim::EntityId, BattleEffects::UnitFrame> unit_frames_;
    // #427 BP-21: per unit, the presented tick its last shield colour flash started at, while
    // the flash runs; how many flashes started.
    presentation_constants::Flash shield_flash_constants_;
    std::uint64_t shield_flash_samples_{};
    std::array<float, 3> max_shield_flash_scale_{1.0F, 1.0F, 1.0F};
    std::map<sim::EntityId, double> shield_flash_start_;
    std::uint64_t shield_flashes_{};
    // #862 (space-abilities AB-63 to AB-65, space-damage IS-03): per frame's newest tick, each
    // squadron container's ION_CANNON_SHOT switch-ons (first and last tick seen on) and each
    // stunned unit's first stunned tick and most stun frames left seen, for the report.
    struct IonShotRow final {
        std::uint64_t switched_on{};
        std::uint64_t first_on{};
        std::uint64_t last_on{};
        bool on{};
    };
    struct IonStunRow final {
        std::uint64_t first{};
        std::uint32_t max_frames{};
    };
    std::map<sim::EntityId, IonShotRow> ion_shot_rows_;
    std::map<sim::EntityId, IonStunRow> ion_stun_rows_;
    BattleFrame battle_frame_;
    std::uint64_t reached_tick_{};
    // Ticks whose events the event log had dropped before a frame reached them (report rows).
    std::vector<std::string> event_gap_rows_;
    // #497: the tick of each shooter's first hit on each target the frames reached (report only;
    // at most first_hits_limit pairs).
    std::map<std::pair<sim::EntityId, sim::EntityId>, std::uint64_t> first_hits_;
    std::map<sim::tactical::PlayerId, sim::tactical::TeamId> team_of_player_;
    std::vector<VisibleUnit> visible_;
    space::SnapshotIndex snapshot_index_;
    std::vector<space::LiveUnitPose> poses_;
    std::vector<SpacePopulation::LivePose> live_poses_;
    std::vector<SpacePopulation::LiveClipPose> clip_poses_;
    const particles::StepExecutor* pose_workers_{};
    // #535: the local player's per-unit fog fade (space-fog-presentation.md FW-16 to FW-18),
    // and the ticks it has already run through (frame() may see the same tick more than once
    // while paused).
    space::UnitFade fade_;
    space::FogGhosts fog_ghosts_;
    std::map<sim::EntityId, RadarMemory> radar_memories_;
    std::vector<sim::EntityId> fog_immediate_;
    std::vector<space::FogGhosts::Observation> fog_observations_;
    std::optional<std::array<float, 3>> neutral_fog_colour_;
    std::map<sim::tactical::TypeId, std::vector<animation::BonePose>> neutral_fog_poses_;
    std::map<sim::tactical::TypeId, std::pair<bool, bool>> fog_memory_types_;
    std::vector<sim::RenderInstance> fog_ghost_instances_;
    std::vector<std::string> fog_ghost_rows_;
    std::map<sim::EntityId, std::tuple<bool, bool, std::size_t>> fog_ghost_logged_states_;
    std::uint64_t fog_ghost_logged_tick_{std::numeric_limits<std::uint64_t>::max()};
    std::map<sim::EntityId, space::NebulaBlend> nebula_blends_;
    std::optional<std::uint64_t> nebula_blend_tick_;
    std::uint64_t nebula_blend_missing_ticks_{};
    data::ui::Rgba8 nebula_colour_{255, 255, 255, 64};
    std::optional<double> fade_presented_tick_;
    std::size_t fading_units_{};
    // #535: one row a fading entity every reached tick (fading_log_limit rows), so a test can
    // read a unit's opacity across several ticks in one report, without a rerun a tick (K-4).
    std::vector<std::string> fading_log_rows_;
    std::uint64_t fading_logged_tick_{~std::uint64_t{}};
    sim::tactical::PlayerId player_{};
    std::uint64_t frames_{};
    double presented_tick_{};
    std::uint64_t latest_tick_{};
    std::size_t visible_units_{};
    std::size_t hidden_units_{};
    std::size_t next_capture_{};
    std::size_t next_capture_frame_{};
    bool finished_{};
    std::string finish_status_{"not_finished"};
    std::optional<bool> headless_equal_;
    std::vector<std::string> hashes_;
    std::string failure_;
    // The simulation thread's failure, once it stopped.
    std::string simulation_error_;
    // #615: the failure replay's path, or why it was not written; empty until a failure.
    bool failure_replay_tried_{};
    std::string failure_replay_;
    std::string failure_replay_error_;
    // #77: the battle's outcome, from the first presented snapshot that carries one.
    std::optional<sim::tactical::BattleOutcome> outcome_;
    // #459: the time panel, and the driven presentation clock's current segment (TP-06): from
    // the frame `driven_origin_frame_` on, the tick advances step x factor per frame.
    ui::TimeControls time_;
    double driven_factor_{1.0};
    double driven_origin_tick_{};
    std::uint64_t driven_origin_frame_{};
    double driven_base_{};
    std::uint64_t last_shown_{};
    // #453.
    std::optional<BattleEnd> battle_end_;
    bool quit_{};
    Phase phase_{Phase::loading};
    std::optional<std::uint64_t> ready_tick_;
    std::optional<std::uint64_t> begin_tick_;
    std::optional<std::uint64_t> begin_frame_;
    std::optional<std::chrono::steady_clock::time_point> battle_clock_start_;
    ui::BattleResults results_;
    std::unique_ptr<BattleScoring> scoring_;
    std::string scoring_failure_;

    // #81 (docs/behaviour/unit-animation.md): each start unit's death clone, a placed ship
    // shown from the frame its unit is destroyed, at the unit's last drawn pose, playing
    // its Specific_Death_Anim_Type clip (DIE) on the presentation clock, or keeping its pose
    // when that clip cannot start. The simulation never sees it.
    struct DeathClone final {
        std::size_t ship{};
        std::string type;
        std::string clip;  // empty: no clip of the type; the clone keeps its pose
        bool remove_upon_death{};
        animation::DeathPlayback playback{};
    };
    struct ActiveClone final {
        sim::EntityId unit{};
        std::uint64_t death_tick{};  // the first completed tick without the unit
        SpacePopulation::LivePose pose;
    };
    // The death clip frame `tick` whole ticks after the clone appeared; nullopt when the clone
    // is removed at once (its clip did not start and it has Remove_Upon_Death).
    [[nodiscard]] static std::optional<animation::DeathFrame> clone_death_frame(
        const DeathClone& clone, const animation::Player* player, std::uint64_t tick);
    // Sets up the death clones and the unit_clips report rows (end of prepare()).
    void prepare_clips(const vfs::Vfs& filesystem, const data::Catalog& catalog);
    // The death clone of placed ship `source` (a unit of type `unit_type`), its clip variant drawn
    // from `variant_key`: its report row, and its own placed ship appended when it is drawn.
    // Nullopt when the type has no Death_Clone, or the clone is not drawn.
    [[nodiscard]] std::optional<DeathClone> prepare_death_clone(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                                                std::size_t source, std::uint64_t variant_key);
    void prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog);
    // A slot whose unit is no longer in the tactical snapshot, and whose death clone (if any) has
    // left, is free for the next unit of its type (space-purchasing PU-G25).
    void release_dead_slots(const sim::tactical::TacticalSnapshot& latest);
    // #424, #518: the squadron selects and orders as one unit, its team container, from the first
    // snapshot that lists it; its roster stays the one it had then.
    void register_squadron(const sim::tactical::Squadron& squadron, std::uint64_t tick);
    // The composed ship of a live unit: a start unit's, or a launched craft's slot (bound on
    // first sight); nothing for a unit without a model.
    [[nodiscard]] std::optional<std::size_t> ship_of(sim::EntityId entity, sim::tactical::TypeId type,
                                                  sim::tactical::PlayerId owner);
    std::map<sim::EntityId, DeathClone> death_clones_;
    // #76 AB-31 (UA-06): a SPOILER_LOCK craft's S-foil clip. `alternate` is UNDEPLOY (off) rather
    // than DEPLOY (on); it runs from `start_frame` at the presentation tick `since`. Before the
    // first switch no clip runs and the craft keeps its bind pose.
    struct SFoil final {
        bool on{};
        bool started{};
        bool alternate{};
        double since{};
        std::uint32_t start_frame{};
    };
    std::map<sim::EntityId, SFoil> sfoils_;
    // #76: the command bar's view of abilities reads the newest snapshot frame() reached.
    std::shared_ptr<const sim::tactical::TacticalSnapshot> ability_snapshot_;
    std::vector<AbilityClick> ability_clicks_;
    Abilities abilities_{*this};
    std::uint64_t sfoil_switches_{};
    // #458: the launch slots' death clones, handed to death_clones_ when a craft binds its slot.
    std::vector<DeathClone> launch_clones_;
    // The clones shown now; one leaves (its ship retired) when it is removed or has faded out.
    std::vector<ActiveClone> active_clones_;
    // Clones frame() stopped drawing this frame; their ships are retired by retire_clones().
    std::vector<ActiveClone> retiring_clones_;
    std::vector<std::string> retired_clone_rows_;
    bool trace_frames_{};
    double bookkeeping_ms_{};
    double pose_ms_{};
    double opacity_ms_{};
    double tick_wait_ms_{};
    double live_frame_ms_{};
    double session_tail_ms_{};
    double clip_pose_ms_{};
    std::map<sim::EntityId, SpacePopulation::LivePose> last_poses_;
    // Report rows: the clips each start unit type's model has, and each clone set up.
    std::vector<std::string> unit_clip_rows_;
    std::vector<std::string> death_clone_rows_;
    // #530: the hyperspace arrivals seen (evidence for the report).
    struct ArrivalRow final {
        sim::tactical::PlayerId owner{};
        sim::tactical::TypeId type{};
        std::uint64_t first_tick{};
        std::uint64_t visible_tick{};
        std::uint64_t landed_tick{};
        std::uint32_t last_frame{};
        double facing_yaw{};
    };
    std::map<sim::EntityId, ArrivalRow> arrivals_;
    // #391: the breakoff props (map mode owns them); null without.
    DebrisProps* debris_{};
    // #447 report: each spin-away the frames reached (the ticks of its start and end events) and
    // the most spinning craft one frame drew.
    struct SpinRow final {
        std::uint64_t started{};
        std::optional<std::uint64_t> ended;
    };
    std::map<sim::EntityId, SpinRow> spin_rows_;
    std::size_t spinning_drawn_max_{};
    std::size_t spinning_ships_max_{}; // of those, the most with a composed ship (start or launched)
    BattleEffects* projectile_models_{};
};

} // namespace eawr::presentation::godot_backend

#pragma once

#include "eawr/battle/start.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/ui/time_controls.hpp"
#include "eawr/skirmish/melee.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::platform { class TacticalFeed; }

namespace eawr::presentation::godot_backend {

class LanMatch;
struct SavedBattlePreparation;

namespace live_session {

struct PreparedBattle {
    sim::tactical::TacticalReplay replay;
    skirmish::SessionContent content;
    sim::tactical::VictoryRules victory;
    sim::tactical::EconomyRules economy;
    units::UnitTables tables;
    std::optional<battle::BattleStart> start;
};

// legacy #6634: one injected tactical-script letterbox call (--eawr-live-letterbox), at the tick of the script service that makes it.
struct DebugLetterbox final {
    std::uint64_t tick{};
    bool in{};
    double seconds{};
};
struct DebugStoryLock final { std::uint64_t tick{}; bool locked{}; };

// One injected tactical-script story call (--eawr-live-story): the verb of story_presentation.hpp and its staged text arguments, at the
// tick of the script service that makes it.
struct DebugStory final {
    std::uint64_t tick{};
    std::string verb;
    std::vector<std::string> arguments;
};

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
    // `<tick>:buy:<player>@<TYPE>` and `<tick>:reinforce:<player>@<TYPE>,<x>,<y>,<z>` (#5097 measurement hooks): the
    // purchase and the call-in of a type as that player, through the commands the HUD issues. `unit` holds the player id.
    enum class Purchase : std::uint8_t { none, buy, reinforce };
    Purchase purchase{Purchase::none};
    std::string type_name;
};

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

struct Options final {
    bool land{};
    // HCB-46/63: skirmish has tactical production; GC tactical children do not.
    bool tactical_production{true};
    bool observer_multiplayer{};
    sim::SeatId controller_seat{};
    // #8182: a battle view of a seat that commands nothing (a spectator of the player team's perspective): it sees the battle
    // and never selects a unit or issues an order, and its HUD shows no order controls.
    bool view_only{};
    std::vector<std::pair<sim::SeatId, std::string>> controller_names;
    std::vector<sim::galactic::Seat> colour_seats{};
    std::vector<sim::galactic::TeamLead> colour_teams{};
    std::filesystem::path land_bindings;
    bool land_vehicles_only{}; // --eawr-land-vehicles-only: human land slots start with stock vehicle companies
    // #5130: names belong to the observer, outside deterministic battle content.
    std::vector<std::pair<sim::tactical::PlayerId, std::string>> observer_names;
    std::shared_ptr<const PreparedBattle> borrowed_battle;
    std::shared_ptr<platform::TacticalFeed> borrowed_feed;
    std::shared_ptr<LanMatch> lan;
    std::string fixture;                                  // --eawr-live-session (m2|skirmish|replay|melee)
    skirmish::FixtureOptions skirmish;                    // --eawr-skirmish-* (#908)
    bool skirmish_tsw_conflict{};                         // --eawr-skirmish-tsw-conflict on: staging, the super weapon's gate is open
    bool skirmish_destroyed_planet{};                     // --eawr-skirmish-destroyed-planet on: start as at a destroyed planet (WGT-57)
    std::optional<std::int64_t> skirmish_credits;           // --eawr-skirmish-credits <n>: the lobby's starting credits (default: MP_Default_Credits)
    // `attract` (MAT-08): the main-menu demo battle. `skirmish.map` names the demo map; the
    // forces are its placed objects and `attract_ai` names the AI player type each editor
    // player index runs (MAT-09). There is no lobby, no victory condition and no local player.
    std::vector<std::pair<std::uint32_t, std::string>> attract_ai;
    // The attract load's unit models are decoded ahead on reader threads (scene::ModelPrefetch); `model_volumes` are the
    // roots whose disk kind sets the reader count.
    // The initial compose decodes the drawn ships' hull base textures ahead on reader threads (scene::TexturePrefetch).
    // Set on the Separate render thread only, for every live session (configure_ready_options); temporary A/B switch
    // `--eawr-texture-prefetch off|on` (default on there). `texture_prefetch_readers` nonzero fixes the reader count.
    bool texture_prefetch{};
    unsigned texture_prefetch_readers{};
    std::vector<std::filesystem::path> model_volumes;
    // The attract load's first three stages (fixture + unit tables, start inputs, start + content) run back to back on one
    // worker thread while the main thread polls (OffloadedStages). Set on the Separate render thread (Safe stays inline).
    bool load_offload{};
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
    // --eawr-live-purchase-slots <N> (#530): purchase model slots composed at load per buyable type
    // (space-purchasing PU-G25). Default 10 keeps ordinary purchases warm so model composition stays at load.
    // Further purchases grow a slot on first use under the per-frame arrival budget (arrival_slot_budget).
    std::uint32_t purchase_slots{10};
    // --eawr-live-warm-launches <N>: non-campaign launch slots at load per planned row (the first-wave
    // pool; the rest grow on first use). Default 64 keeps the established warm capacity.
    std::uint32_t warm_launches{64};
    // --eawr-live-arrival-budget <N>: at most N new hull/clone pairs are composed per frame for purchases and
    // reinforcement pools that arrive without a warm slot (a burst beyond N is drawn over the following frames).
    // Planned launches are exempt: a launched craft is drawn in its launch frame. Default 8.
    std::uint32_t arrival_slot_budget{8};
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
    std::optional<std::uint64_t> lan_quit_tick;
    bool lan_acceptance{};
    std::optional<std::uint64_t> end_tick;                // --eawr-live-ticks
    double ticks_per_frame{0.5};                          // --eawr-live-step
    std::optional<std::size_t> workers;                   // --eawr-live-workers
    particles::ParticleDetail particle_detail{};          // independent presentation settings
    // True when --eawr-live-particle-detail / --eawr-live-particle-heat chose the value; otherwise
    // the saved Advanced video settings may (#6560). The local LOD is a flag only.
    bool particle_global_given{};
    bool particle_heat_given{};
    std::optional<std::size_t> particle_workers;          // --eawr-live-particle-workers (#638)
    // --eawr-live-speed 0..4: the tactical speed setting (#459 TP-05, default 2).
    std::uint32_t speed_step{ui::default_speed_step};
    // True when --eawr-live-speed chose the step; otherwise a player's saved setting may (#2927).
    bool speed_step_given{};
    // --eawr-live-max-rate <ticks/s>: test knob, a ceiling on this viewer's real-time pacing (0 = none), so a fixture can
    // stand in for a machine that cannot reach a requested shared rate. Pacing only; what a tick does is unchanged.
    std::uint32_t max_rate{};
    std::filesystem::path hashes_path;                    // --eawr-live-hashes <csv>
    std::filesystem::path replay_path;                    // --eawr-live-replay-out <file>
    // Skirmish battle saves (docs/checkpoint-format.md "Skirmish battle saves"), solo local battles with the AI:
    // --eawr-live-save-at <tick>:<file> writes the battle once that tick has completed and keeps running;
    // --eawr-live-load <file> starts the prepared battle from such a save, at its tick (the same scene
    // arguments must prepare the same battle). After a load --eawr-live-ticks counts from the saved tick.
    std::optional<std::uint64_t> save_tick;
    std::filesystem::path save_path;
    std::filesystem::path load_path;
    // legacy #6634: --eawr-live-letterbox <tick>:<in|out>:<seconds> (repeatable) injects a tactical script's Letter_Box_In / Letter_Box_Out
    // in the script service of that tick, for tests of the battle HUD's bands; the battle must run scripts (the AI on).
    std::vector<DebugLetterbox> debug_letterbox;
    // --eawr-live-story <tick>:<camera|fade|environment>:<arguments separated by commas> (repeatable) injects a tactical script's story
    // cinematic, fade or environment call the same way (the arguments are the ones the script host stages, e.g.
    // 30:camera:start,1 or 60:fade:out,2); the battle must run scripts (the AI on).
    std::vector<DebugStory> debug_story;
    std::vector<DebugStoryLock> debug_story_lock;
    // A saved battle the host already prepared for exactly this `load_path` (the queue step, skirmish_prepare.hpp). The
    // first load stage takes it only for the same mounted content and path; otherwise it prepares the battle itself.
    std::shared_ptr<SavedBattlePreparation> saved_preparation;
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
    // Hidden start-time A/B diagnostic; never persisted in player settings.
    bool audio_preload{true};
    // --eawr-live-verify on|off: whether finish() replays the recorded command stream headlessly
    // and compares the hashes (report row headless_hashes_equal, else null). The replay costs about
    // as long as the battle did, on one thread, so an interactive (real-time) session skips it by
    // default: closing the window must not wait for it. Driven runs verify by default.
    std::optional<bool> verify;
    // Set by the map mode.
    bool real_time{};
    std::uint32_t warmup_frames{};
};

} // namespace live_session
} // namespace eawr::presentation::godot_backend

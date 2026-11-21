#pragma once

// The Lua side of a galactic campaign's story (WST-53..55, docs/behaviour/walks/gc-story.md): each loaded
// sub-plot that names a `Lua_Script` gets one script instance in the story scheduler, and the plot engine's
// triggers reach it as `Story_Event_Trigger(event)` calls. The engine and its XML events stay in
// sim/galactic/story_engine; this host only loads, starts and feeds the scripts.

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace eawr::campaign {
class GalacticStoryLink;
class CampaignSetupPlan;
class GameSession;
struct FleetTypeBinding;
struct GalacticWorldImmediateRules;
class GalacticObjectPerception;

// Instance identities of story scripts. They start far above the AI scheduler's player and plan instances, so the
// two scheduler sets stay disjoint (ADR-012).
inline constexpr std::uint64_t first_story_script_instance = 1'000'000'000;

// WST-90..93: immutable lookup inputs, rebound from loaded factions and the campaign's player table on load.
// The faction database includes names with no registered player. `local_player` is the recorded canonical observer's
// player; multiplayer schedulers deliberately expose no local player (docs/lua-sandbox.md).
struct GalacticStoryPlayers {
    struct Player {
        sim::galactic::PlayerId id{};
        std::string faction;
        std::string difficulty{"Normal"};
        // #7102: Player.Is_Human. True when the player's seat is human-controlled; SWITCH_CONTROL never changes it (WGS-136).
        bool human{};
        // GA-10: Player.Is_Neutral, the faction's authored Is_Neutral flag.
        bool neutral{};
    };
    std::vector<std::string> factions;
    std::vector<Player> players;
    std::optional<sim::galactic::PlayerId> local_player;
    std::vector<std::pair<std::string, sim::galactic::PlanetId>> planets;
    std::vector<std::pair<std::string, sim::galactic::TypeId>> types{};
    // Project deviation (co-op design D5): the one faction every human seat holds in a networked single-player-rules
    // session. Derived from the agreed roster, so it is the same on every peer; a multiplayer scheduler exposes
    // `Find_Player("local")` as this player only, never a per-peer local one.
    std::optional<sim::galactic::PlayerId> shared_local_player{};
    // Authored galactic prices, keyed by the same original-type identities as `types`.
    std::map<sim::galactic::TypeId, sim::math::Fixed> type_build_costs{};
    // GA-12: Type.Get_Combat_Rating, the type's combat power metric (the AI's force estimate reads the same value).
    std::map<sim::galactic::TypeId, script::numeric::LuaNumber> type_combat_ratings{};
    // GA-13: the story scripts' EvaluatePerception. The mounted equations and the campaign facts they read that the
    // completed world does not hold (each planet's normalized authored base income); null equations refuse the call.
    std::shared_ptr<const GalacticObjectPerception> perception{};
    std::map<sim::galactic::PlanetId, script::numeric::LuaNumber> planet_base_income{};
    std::uint64_t seed{};
};

struct GalacticObjectQueryType {
    sim::galactic::TypeId id{};
    std::string name;
    std::uint64_t categories{}, properties{};
    bool qualified{true};
};
struct GalacticObjectQueryCatalog {
    std::map<sim::galactic::TypeId, GalacticObjectQueryType> types;
    std::map<std::string, sim::galactic::TypeId, std::less<>> names;
    std::map<std::string, std::uint64_t, std::less<>> categories, properties;
};
struct GalacticObjectFilter {
    std::optional<sim::galactic::PlayerId> player;
    std::optional<sim::galactic::TypeId> type;
    std::optional<std::uint64_t> categories, properties;
};
// Build the query vocabulary from the mounted type data rather than from
// mod-specific names. Missing loaded types keep their identity but their
// category/property flags remain unavailable.
[[nodiscard]] core::Result<GalacticObjectQueryCatalog> make_galactic_object_query_catalog(
    const units::UnitTables&, std::span<const std::pair<std::string, sim::galactic::TypeId>>);
// RGC-112: parse in authored argument order; the world owner applies this
// immutable filter to its current actor view, including staged births.
[[nodiscard]] core::Result<GalacticObjectFilter> parse_galactic_object_filter(
    const script::authoritative::ValueList&, const GalacticObjectQueryCatalog&);
[[nodiscard]] core::Result<bool> matches_galactic_object_filter(const GalacticObjectFilter&,
    const sim::galactic::CensusObject&, const GalacticObjectQueryType* qualified_flags);
// Query an immutable, identity-ordered actor view. The effect owner supplies
// its projected rows so a script can see births made earlier in its call.
[[nodiscard]] core::Result<script::authoritative::ValueList> query_galactic_object_view(
    std::string_view, const script::authoritative::ValueList&,
    std::span<const sim::galactic::CensusObject>, const GalacticObjectQueryCatalog&, const sim::PartitionExecutor&);
// Extend PR5's world host; staged removals remain private until publication.
// `perception` answers EvaluatePerception for the scripts (GOL-17); without it the call is refused as unavailable.
void add_galactic_object_world_rules(GalacticWorldImmediateRules&, GalacticObjectQueryCatalog,
    const sim::PartitionExecutor&, std::shared_ptr<const GalacticObjectPerception> perception = {});

// Mounted GameObject modules and their authored type bindings. This inventory
// is independent of story plots and of the galactic AI scheduler.
class GalacticObjectScripts final {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<const GalacticObjectScripts>> load(
        const vfs::Vfs&, std::span<const std::pair<sim::galactic::TypeId, std::string>> scripts);
    [[nodiscard]] const std::map<sim::galactic::TypeId, std::string>& types() const noexcept { return types_; }
    [[nodiscard]] const std::map<std::string, std::string>& modules() const noexcept { return modules_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    // The mounted perception equations the scripts may evaluate (GOL-17); null when no script is attached.
    [[nodiscard]] const std::shared_ptr<const GalacticObjectPerception>& perception() const noexcept { return perception_; }
    [[nodiscard]] std::string fingerprint() const;
    [[nodiscard]] core::Result<script::authoritative::ScriptScheduler> create_scheduler(
        script::authoritative::SessionConfig) const;
    // The attachments the actor records ask for, in actor order (a pure function of the records and this content).
    [[nodiscard]] core::Result<std::vector<script::authoritative::ObjectScript>> attachments(
        std::span<const sim::galactic::CensusObject>, const sim::PartitionExecutor&) const;
    // attachments(), then the scheduler's reconcile of them.
    [[nodiscard]] core::Result<std::vector<script::authoritative::ScriptDiagnostic>> reconcile(
        script::authoritative::ScriptScheduler&, std::span<const sim::galactic::CensusObject>,
        const sim::PartitionExecutor&) const;
    [[nodiscard]] core::Result<script::authoritative::ServiceReport> service(
        script::authoritative::ScriptScheduler&, std::uint64_t galactic_frame,
        const sim::PartitionExecutor&) const;

private:
    std::map<sim::galactic::TypeId, std::string> types_;
    std::map<std::string, std::string> modules_;
    std::vector<std::string> notes_;
    std::shared_ptr<const GalacticObjectPerception> perception_;
};

// Resolve the same authored attachments without constructing an AI runtime.
[[nodiscard]] core::Result<std::shared_ptr<const GalacticObjectScripts>> load_world_object_scripts(
    const vfs::Vfs&, const data::Catalog&, const CampaignSetupPlan&);
[[nodiscard]] core::Result<std::shared_ptr<const GalacticObjectScripts>> load_world_object_scripts(
    const vfs::Vfs&, const data::Catalog&, std::span<const FleetTypeBinding>);

// Bind the engine primitives used by data-authored GameObject libraries. The
// caller installs the authoritative immediate-effect owner before attachment.
[[nodiscard]] core::Result<void> register_galactic_object_bindings(
    script::authoritative::ScriptScheduler&, const GalacticStoryPlayers&, const std::shared_ptr<GalacticStoryLink>& = {});
using GalacticObjectEffectsFactory = std::function<core::Result<std::shared_ptr<script::authoritative::ImmediateEffectHost>>(
    sim::galactic::GalacticSession&, const sim::PartitionExecutor&)>;
[[nodiscard]] std::shared_ptr<script::authoritative::GalacticObjectScriptEngine> make_galactic_object_engine(
    std::shared_ptr<const GalacticObjectScripts>, GalacticObjectEffectsFactory = {},
    std::size_t max_instances = sim::galactic::galactic_max_objects);
// Fresh setup and checkpoint reconstruction are world-owned; neither requires
// a native AI adapter. The same bound content/link must be supplied on restore.
[[nodiscard]] core::Result<void> configure_world_object_scripts(GameSession&,
    std::shared_ptr<const GalacticObjectScripts>, const std::shared_ptr<GalacticStoryLink>&,
    GalacticObjectEffectsFactory, std::size_t max_instances = sim::galactic::galactic_max_objects);
[[nodiscard]] core::Result<void> install_world_object_scripts(GameSession&,
    std::shared_ptr<const GalacticObjectScripts>, script::authoritative::SessionConfig,
    const GalacticStoryPlayers&, const std::shared_ptr<GalacticStoryLink>&,
    GalacticObjectEffectsFactory, std::size_t max_instances = sim::galactic::galactic_max_objects);

// Immutable content of one campaign start: the pinned script modules and which sub-plot runs which one.
class GalacticStoryScripts final {
public:
    struct Entry {
        std::string subplot;   // the sub-plot file name as written (the plot engine's key)
        std::string script;    // the authored `Lua_Script` text
        std::string module;    // logical module path; empty when the script text names nothing
        std::uint64_t instance{};
    };

    // Reads every Library and Story module of the mounted data (a script may require any of them) and binds
    // each scripted sub-plot, in sub-plot order, to the Story-directory module of its `Lua_Script`. A script
    // that names no module of the mounted data is listed in notes(); its sub-plot simply runs no Lua.
    [[nodiscard]] static core::Result<std::shared_ptr<const GalacticStoryScripts>> load(
        const vfs::Vfs&, std::span<const sim::galactic::story::SubPlotDef>);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::map<std::string, std::string>& modules() const noexcept { return modules_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    // #7073: what the effective scoring module's Base_Definitions set through GlobalValue.Set, once, in its own fresh script
    // instance (FS-44: a fresh scoring attachment runs the definitions before any plot script exists). Filled when a story
    // scheduler is made from this content (`create_galactic_story_scheduler`), because the definitions look up the campaign's
    // object types (a mod's MOD_ID comes from a dummy type). The story scripts read these as the registry's starting values, so
    // a plot's own chunk and Base_Definitions already see them; a write by any script replaces the value. Empty when no module
    // is named or authored, or it sets nothing. Its `main`/service is not started (#7075).
    [[nodiscard]] const std::map<std::string, script::authoritative::Value>& initial_globals() const noexcept { return initial_globals_; }
    // Notes of that run (a fault in the definitions), listed by name; start_galactic_story_scripts adds them to its notes.
    [[nodiscard]] const std::vector<std::string>& scoring_notes() const noexcept { return scoring_notes_; }
    // Runs the pinned scoring module's definitions once for a scheduler about to be made (called by the factory; deterministic in
    // the content and the players). Replaces initial_globals() and scoring_notes().
    void evaluate_scoring_definitions(const script::authoritative::SessionConfig& base, bool multiplayer, const GalacticStoryPlayers& players) const;
    // True when any pinned module names Spawn_Unit or Prevent_AI_Usage (a lexical scan at load, comments included, so
    // it only errs toward true). Only such a campaign runs its story instances through the ordered native shadow;
    // every other campaign keeps the partitioned story service and its previous hashes.
    [[nodiscard]] bool uses_immediate_effects() const noexcept { return uses_immediate_effects_; }
    // GA-13: a mounted story or library module names EvaluatePerception, so the story host needs the perception equations.
    [[nodiscard]] bool uses_perception() const noexcept { return uses_perception_; }
    // The entry of the sub-plot with this file name, or null.
    [[nodiscard]] const Entry* find(std::string_view subplot) const noexcept;
    // The sub-plot file names of these script instances (ascending IDs in, document order out; unknown IDs are skipped).
    [[nodiscard]] std::vector<std::string> subplots_of(std::span<const std::uint64_t> instances) const;
    // Stable text over module paths and bytes and the sub-plot bindings, for the content identity.
    [[nodiscard]] std::string fingerprint() const;

private:
    // Pins the scoring module named by the effective Game_Scoring_Script_Name and the Miscellaneous modules it may require; an absent
    // constant or module is not an error.
    [[nodiscard]] static core::Result<void> pin_scoring_module(const vfs::Vfs&, GalacticStoryScripts&);

    std::vector<Entry> entries_;
    std::map<std::string, std::string> modules_;
    std::vector<std::string> notes_;
    std::string scoring_module_;                          // logical path of the pinned scoring module, empty when none
    std::map<std::string, std::string> scoring_modules_;  // the Miscellaneous modules pinned for that run
    mutable std::map<std::string, script::authoritative::Value> initial_globals_;
    mutable std::vector<std::string> scoring_notes_;
    bool uses_immediate_effects_{};
    bool uses_perception_{};
};

// What the story scripts reach in the plot engine while a campaign runs (WST-37, WST-49, WST-55, WST-56): Get_Story_Plot
// and the event edits, Story_Event, and the TRIGGER_AI flags Check_Story_Flag reads. The scheduler is built before the
// game session exists, so the scripts hold this empty link and `bind` fills it at install, before the first step. A
// link that was never bound answers "no plot engine" to the plot calls and "not set" to every flag.
class GalacticStorySink {
public:
    virtual ~GalacticStorySink() = default;
    // Commits one plot edit, a broadcast notification and a flag clear to the plot engine; the flag read.
    [[nodiscard]] virtual core::Result<void> plot(const sim::galactic::story::PlotCommand&) = 0;
    [[nodiscard]] virtual core::Result<void> notify(const sim::galactic::story::TextNotice&) = 0;
    virtual void clear_flag(sim::galactic::story::Player, std::uint64_t planet, std::string_view name) = 0;
    [[nodiscard]] virtual bool flag(sim::galactic::story::Player, std::uint64_t planet, std::string_view name) const = 0;
};

// legacy #6634: the verb a story script's Letter_Box_In / Letter_Box_Out stages (arguments: "in" or "out", then the seconds as
// text). Not a plot-engine verb: the session turns it into a local presentation request for the plot owner's seat, in the
// ordered commit of the story service (see GameSession::connect_story_link).
inline constexpr std::string_view verb_letter_box = "galactic.letter_box";

class GalacticStoryLink final {
public:
    GalacticStoryLink();
    ~GalacticStoryLink();
    GalacticStoryLink(const GalacticStoryLink&) = delete;
    GalacticStoryLink& operator=(const GalacticStoryLink&) = delete;
    // Before the first step only: the scripts read the link from the service threads without a lock.
    void bind(std::span<const sim::galactic::story::SubPlotDef> definitions, std::unique_ptr<GalacticStorySink> sink);
    // The ordered commit of the commands the scripts staged in one service (see ScriptedGalacticSession::
    // set_story_command_committer). Consumes the story verbs it commits, in command order.
    [[nodiscard]] core::Result<void> commit(script::authoritative::ServiceReport&) const;
    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }
private:
    std::unique_ptr<Impl> impl_;
};

// The story scheduler of a galactic campaign: the content's modules pinned, the Library and Story directories on
// the script path, Find_Player (WST-90..94), DiscreteDistribution, GameRandom and GameRandom.Free_Random (RGC-190..199) bound, and every other
// galactic engine function this host does not implement bound to a named EAWR-SCRIPT-0215 fault. `base` supplies seed
// and clock. `multiplayer`: the game mode is a multiplayer one (RGC-196: Free_Random uses the synchronised stream).
[[nodiscard]] core::Result<script::authoritative::ScriptScheduler> create_galactic_story_scheduler(
    const GalacticStoryScripts&, script::authoritative::SessionConfig base, bool multiplayer = false,
    const GalacticStoryPlayers& players = {}, script::authoritative::GalacticSnapshotReader = {},
    script::authoritative::GlobalValueReader = {}, const std::shared_ptr<GalacticStoryLink>& link = {});

// A fresh campaign start: one instance per bound sub-plot, then the engine's initialization events
// (`Base_Definitions`, then the `main` thread) for the first service. A script whose chunk fails is left out and
// its reason appended to `notes`; the campaign goes on without it.
[[nodiscard]] core::Result<void> start_galactic_story_scripts(
    script::authoritative::ScriptScheduler&, const GalacticStoryScripts&, std::vector<std::string>& notes);

// WST-20 (3)/55: deliver the plot engine's callbacks. `definitions` is the engine's sub-plot table the calls index.
// A call for a sub-plot whose script is not running (never started, finished or detached) is dropped.
// `sequence` is the caller's strictly increasing event counter.
[[nodiscard]] core::Result<void> forward_galactic_story_calls(script::authoritative::ScriptScheduler&,
    const GalacticStoryScripts&, std::span<const sim::galactic::story::SubPlotDef> definitions,
    std::span<const sim::galactic::story::ScriptCall> calls, std::uint64_t& sequence);

} // namespace eawr::campaign

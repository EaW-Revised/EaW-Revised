#pragma once

// The Lua side of a galactic campaign's story (WST-53..55, docs/behaviour/walks/gc-story.md): each loaded
// sub-plot that names a `Lua_Script` gets one script instance in the story scheduler, and the plot engine's
// triggers reach it as `Story_Event_Trigger(event)` calls. The engine and its XML events stay in
// sim/galactic/story_engine; this host only loads, starts and feeds the scripts.

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::campaign {

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
    };
    std::vector<std::string> factions;
    std::vector<Player> players;
    std::optional<sim::galactic::PlayerId> local_player;
};

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
    // The entry of the sub-plot with this file name, or null.
    [[nodiscard]] const Entry* find(std::string_view subplot) const noexcept;
    // Stable text over module paths and bytes and the sub-plot bindings, for the content identity.
    [[nodiscard]] std::string fingerprint() const;

private:
    std::vector<Entry> entries_;
    std::map<std::string, std::string> modules_;
    std::vector<std::string> notes_;
};

// The story scheduler of a galactic campaign: the content's modules pinned, the Library and Story directories on
// the script path, Find_Player (WST-90..94), DiscreteDistribution, GameRandom and GameRandom.Free_Random (RGC-190..199) bound, and every other
// galactic engine function this host does not implement bound to a named EAWR-SCRIPT-0215 fault. `base` supplies seed
// and clock. `multiplayer`: the game mode is a multiplayer one (RGC-196: Free_Random uses the synchronised stream).
[[nodiscard]] core::Result<script::authoritative::ScriptScheduler> create_galactic_story_scheduler(
    const GalacticStoryScripts&, script::authoritative::SessionConfig base, bool multiplayer = false,
    const GalacticStoryPlayers& players = {});

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

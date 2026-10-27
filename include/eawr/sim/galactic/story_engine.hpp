#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/story_adapters.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Live story engine for the plot files of a galactic campaign (docs/behaviour/walks/gc-story.md,
// docs/behaviour/gc-story-engine.md). Part 1 of the engine: loading is done by the caller (campaign
// layer) into the plain definitions below; this class owns the event graph, the evaluation of
// STORY_TRIGGER / STORY_ELAPSED / STORY_FLAG / STORY_TECH_LEVEL events (WST-30, WST-33), the engine's
// own rewards (WST-24..27, WST-46) and the saved state (WST-60..62). Every other reward is recorded as an
// ordered `Triggered` request for the owning service (part 2 of #683). Land and tactical events and
// rewards are refused with `land_battle_refusal` until land battles exist.
namespace eawr::sim::galactic::story {

// WST-U01 is an open owner question (#4344, C1): the original evaluates same-type events and
// dependants in a hashed container's order. Until a debug-build trace settles it, EVERY ordered walk
// in this engine (same-type events of a sub-plot, dependants, sub-plots of a story, branch members)
// uses DOCUMENT ORDER. This is the one place that names the interim rule, so it can be flipped.
enum class EventOrder : std::uint8_t { document };
inline constexpr EventOrder interim_event_order = EventOrder::document; // interim rule, #4344 C1

inline constexpr std::string_view land_battle_refusal = "not available until land battles";
inline constexpr std::size_t token_slots_event = 7;   // Event_Param1..7 (WST-05)
inline constexpr std::size_t token_slots_reward = 14; // Reward_Param1..14 (WST-05)
inline constexpr int max_branch_reset_depth = 3;       // WST-24
inline constexpr int max_trigger_depth = 128;           // recursion guard: dependant chains and TRIGGER_EVENT cycles (WST-21)

// One parsed `Event` (WST-04/05/06). Names are upper-cased by the loader, parameters are token lists
// as authored (WST-05), event and reward type names are kept exactly as written (WST-08).
struct EventDef {
    std::string name;
    std::string authored_name; // the name as written: the spelling the sub-plot's Lua event table is keyed by (WST-55)
    std::string type, reward_type;
    std::array<std::vector<std::string>, token_slots_event> params;
    std::array<std::vector<std::string>, token_slots_reward> reward_params;
    std::vector<std::string> reward_list;
    std::vector<std::vector<std::string>> prereqs; // groups: AND inside, OR between (WST-06)
    std::string branch;
    bool perpetual{}, multiplayer{};
    math::Fixed inactive_delay{}; // seconds, WST-17
    math::Fixed timeout{};        // parsed, never serviced (WST-18)
    std::string dialog, tag, variable;
    std::int64_t chapter{};
    bool popup{true}, sound{true};
    std::optional<bool> authored_tag_present{}; // before a nonempty Story_Incoming overrides the tag (WST-58/70)
    friend bool operator==(const EventDef&, const EventDef&) = default;
};
// One sub-plot file of a plot (WST-02/03). `name` is the file name as written.
struct SubPlotDef {
    std::string name;
    std::string script; // attached Lua script, held for the Lua story host (WST-53)
    Player owner{};
    bool active{true};
    std::vector<EventDef> events;
    friend bool operator==(const SubPlotDef&, const SubPlotDef&) = default;
};
struct PlotRecord { // WST-01
    Player owner{};
    std::string file;
    friend bool operator==(const PlotRecord&, const PlotRecord&) = default;
};
// An event or reward the loader or engine refused to run, with the reason.
struct Refusal { Player owner{}; std::string subplot, event, what, reason; };

struct EventState {
    std::string name; // restore is by event name (WST-61)
    bool triggered{}, active{}, disabled{};
    math::Fixed start{};                        // WST-21
    math::Fixed clock{}, clock_last{};          // elapsed accumulators (WST-12)
    bool tech_primed{};                         // STORY_TECH_LEVEL -1: first value seen
    std::int64_t tech_last{};                   // and the last level seen, to tell a change from a same-level notice
    bool idle_stamped{};                        // WST-17
    math::Fixed idle_stamp{};
};
struct SubPlotState {
    std::string name;
    Player owner{};
    bool active{};
    std::vector<EventState> events;
};
// Everything the checkpoint keeps (WST-60..62); the definitions are rebuilt from data.
struct State {
    bool started{};   // the initial tech levels have been delivered
    math::Fixed now{};
    std::uint64_t sequence{}; // Triggered sequence, strictly increasing
    std::vector<SubPlotState> subplots;
    std::map<std::string, std::int64_t> flags; // upper-cased flag name -> value (WST-27)
};

// A triggered event and its reward, ordered by `sequence`. Rewards the engine runs itself
// (WST-46 graph/flag rewards) are not listed. WST-20: this record exists after the triggered mark.
struct Triggered {
    std::uint64_t sequence{};
    Player owner{};
    std::string subplot, event, reward_type;
    std::array<std::vector<std::string>, token_slots_reward> reward_params;
    std::vector<std::string> reward_list;
    std::string dialog, tag, variable;
    std::int64_t chapter{};
    bool popup{true}, sound{true};
    bool remove_after_viewed{}; // resolved from authored Story_Tag, independently of Story_Incoming
    bool engine_applied{}; // a graph/flag reward the engine already ran (WST-46); consumers skip it
    std::string refusal; // non-empty: the reward is refused (land_battle_refusal) and must not run
    std::string outcome; // filled by the consumer that applied the record: "applied" or "refused: <reason>"
    friend bool operator==(const Triggered&, const Triggered&) = default;
};

// WST-20 (3): a triggered event of a sub-plot with an attached script asks the Lua story host to call the
// script's `Story_Event_Trigger(name)` before the reward record exists. `subplot` indexes definitions().
struct ScriptCall {
    std::size_t subplot{};
    std::string event; // authored spelling
    friend bool operator==(const ScriptCall&, const ScriptCall&) = default;
};

struct Config {
    // WST-14: outside solo and skirmish games only `Multiplayer` events are evaluated.
    bool solo{true};
};

class Engine final {
public:
    [[nodiscard]] static core::Result<Engine> create(Config config, std::vector<PlotRecord> plots,
        std::vector<SubPlotDef> subplots);
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    ~Engine();

    // WST-13/14: deliver one game notification. `player` names the dispatch target (only that
    // player's sub-plots), absence broadcasts. Part 1 evaluates the tech-level notice (WST-33) and
    // the elapsed notice (same as service); other kinds have no evaluator yet and are ignored.
    [[nodiscard]] core::Result<void> notify(std::optional<Player> player, const Payload& payload);
    // WST-11: one story service. Sends the elapsed notice with the mode time, then polls the flag
    // events of every active sub-plot in table order.
    [[nodiscard]] core::Result<void> service(math::Fixed time_seconds, bool paused);
    // The triggers since the last call, in order.
    [[nodiscard]] std::vector<Triggered> take_triggered();
    // The Lua callbacks the triggers since the last call asked for, in trigger order (WST-20 (3)).
    [[nodiscard]] std::vector<ScriptCall> take_script_calls();

    [[nodiscard]] const State& state() const noexcept;
    [[nodiscard]] core::Result<void> restore(State state);
    // Hex digest of the saved state (not the story clock), recomputed only after a change.
    [[nodiscard]] const std::string& digest() const;
    [[nodiscard]] bool started() const noexcept;
    void mark_started() noexcept;

    [[nodiscard]] std::span<const PlotRecord> plots() const noexcept;
    [[nodiscard]] std::span<const SubPlotDef> definitions() const noexcept;
    // Events or rewards that exist in the data but never run: land and tactical types (refused with
    // land_battle_refusal) and types without an evaluator in this part. Loader drops are separate.
    [[nodiscard]] std::span<const Refusal> unavailable() const noexcept;
    // Engine-reward diagnostics (depth limits, unknown targets) in occurrence order.
    [[nodiscard]] std::span<const Refusal> diagnostics() const noexcept;

    [[nodiscard]] bool triggered(std::string_view subplot, std::string_view event) const;
    [[nodiscard]] bool active(std::string_view subplot, std::string_view event) const;
    [[nodiscard]] std::optional<std::int64_t> flag(std::string_view name) const;

    struct Impl;
private:
    explicit Engine(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Upper-case ASCII copy (WST-04 names, WST-27 flags).
[[nodiscard]] std::string upper(std::string_view text);
// True for the event types the original recognises (WST-08, the shared name table).
[[nodiscard]] bool known_event_type(std::string_view type) noexcept;
// True for the event types that need a tactical mode (land_battle_refusal).
[[nodiscard]] bool tactical_event_type(std::string_view type) noexcept;
[[nodiscard]] bool tactical_reward_type(std::string_view reward) noexcept;

} // namespace eawr::sim::galactic::story

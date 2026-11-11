#include "eawr/sim/galactic/story_engine.hpp"

#include "checkpoint_fields_internal.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <memory>
#include <set>
#include <utility>

namespace eawr::sim::galactic::story {
namespace {
using math::Fixed;

core::Result<void> ok() { return core::Result<void>::success(); }
core::Diagnostic problem(std::string message) {
    core::Diagnostic error;
    error.code = std::string(diagnostic_codes::invalid_setup);
    error.message = "story engine: " + std::move(message);
    return error;
}
core::Result<void> invalid(std::string message) { return core::Result<void>::failure(problem(std::move(message))); }

bool one_of(std::string_view value, std::initializer_list<std::string_view> list) noexcept {
    return std::find(list.begin(), list.end(), value) != list.end();
}

// WST-08: the shared name table. Event types the original evaluates. STORY_POLITICAL_CONTROL has no
// evaluator (WST-32), STORY_BASE_DESTROYED and STORY_MISSION_FAILED are not names (WST-34).
constexpr std::string_view known_events[] = {
    "STORY_TRIGGER", "STORY_ELAPSED", "STORY_FLAG", "STORY_ENTER", "STORY_MOVE", "STORY_LAND_ON", "STORY_DEPLOY",
    "STORY_FLEET_BOUNCED", "STORY_INVASION_BOUNCED", "STORY_PLANET_DESTROYED", "STORY_CONQUER", "STORY_CONQUER_COUNT",
    "STORY_CONSTRUCT", "STORY_CONSTRUCT_LEVEL", "STORY_ACCUMULATE", "STORY_TECH_LEVEL", "STORY_BEGIN_ERA",
    "STORY_DESTROY", "STORY_TACTICAL_DESTROY", "STORY_DESTROY_BASE", "STORY_WIN_BATTLES", "STORY_LOSE_BATTLES",
    "STORY_RETREAT", "STORY_REINFORCE", "STORY_CAPTURE_HERO", "STORY_DEFEAT_HERO", "STORY_GARRISON_UNIT",
    "STORY_ATTACK_HARDPOINT", "STORY_SPACE_TACTICAL", "STORY_LAND_TACTICAL", "STORY_LOAD_TACTICAL_MAP",
    "STORY_VICTORY", "STORY_MISSION_LOST", "STORY_CORRUPTION_CHANGED", "STORY_CORRUPTION_INCREASED",
    "STORY_CORRUPTION_TACTICAL_COMPLETE", "STORY_CORRUPTION_TACTICAL_FAILED", "STORY_OPEN_CORRUPTION",
    "STORY_BUY_BLACK_MARKET", "STORY_GALACTIC_SABOTAGE", "STORY_AI_NOTIFICATION", "STORY_SPEECH_DONE",
    "STORY_GENERIC", "STORY_CLICK_GUI", "STORY_SELECT_PLANET", "STORY_ZOOM_INTO_PLANET", "STORY_ZOOM_OUT_PLANET",
    "STORY_SELECT_UNIT", "STORY_UNIT_ARRIVED", "STORY_FULL_STOP", "STORY_COMMAND_UNIT", "STORY_GUARD_UNIT",
    "STORY_UNIT_PROXIMITY", "STORY_FOG_OBJECT_REVEAL", "STORY_FOG_POSITION_REVEAL", "STORY_DIFFICULTY_LEVEL",
    "STORY_MOVIE_DONE", "STORY_OBJECTIVE_TIMEOUT", "STORY_CHECK_DESTROYED", "STORY_CAPTURE_STRUCTURE"};

// WST-40..49: every reward name the original dispatches (or recognises). A name outside the table
// resolves to no reward (WST-08).
constexpr std::string_view known_rewards[] = {
    "SPAWN_HERO", "UNIQUE_UNIT", "BUILDABLE_UNIT", "LOCK_UNIT", "DISABLE_BUILDABLE", "ENABLE_BUILDABLE", "REMOVE_UNIT",
    "DESTROY_OBJECT", "SET_HEALTH", "CHANGE_OWNER", "FORCE_RESPAWN",
    "CREDITS", "SET_TECH_LEVEL", "SET_MAX_TECH_LEVEL", "STATISTIC_CHANGE", "GIVE_BLACK_MARKET", "NEW_POWER_FOR_ALL",
    "REMOVE_POWER_FROM_ALL",
    "REVEAL_PLANET", "REVEAL_ALL_PLANETS", "ENABLE_GALACTIC_REVEAL", "SET_PLANET_VISIBILITY_LEVEL", "PLANET_FACTION",
    "SET_PLANET_RESTRICTED", "SET_PLANET_SPAWN", "SET_SPAWN", "DISABLE_SPAWN", "ENABLE_INVASION", "RESTRICT_AUTORESOLVE",
    "RESTRICT_CORRUPTION", "RESTRICT_SABOTAGE", "RESTRICT_BLACK_MARKET", "RESTRICT_ALL_ABILITIES", "REMOVE_CORRUPTION",
    "SABOTAGE_STRUCTURE", "ENABLE_SABOTAGE", "ENABLE_FLEET_COMBINE", "ENABLE_DIRECT_INVASION", "DISABLE_DIRECT_INVASION",
    "ENABLE_BOUNTY_COLLECTION", "DISABLE_BOUNTY_COLLECTION", "DISABLE_SPECIAL_STRUCTURE", "BOMBARD_DELAY",
    "DISABLE_AUTORESOLVE", "ENABLE_AUTORESOLVE", "ENABLE_OVERWHELMING_ODDS", "ENABLE_FOW",
    "MOVE_FLEET", "INVADE_PLANET", "FORCE_RETREAT", "DISABLE_RETREAT", "DISABLE_REINFORCEMENTS",
    "LINK_TACTICAL", "SET_TACTICAL_MAP", "SKIRMISH_RULES", "LOAD_CAMPAIGN",
    "VICTORY", "ENABLE_VICTORY", "ACTIVATE_RETRY_DIALOG", "USE_RETRY_DIALOG", "FINISHED_TUTORIAL",
    "TRIGGER_EVENT", "RESET_BRANCH", "DISABLE_BRANCH", "RESET_EVENT", "DISABLE_STORY_EVENT", "SET_FLAG",
    "INCREMENT_FLAG", "STORY_ELEMENT", "RANDOM_STORY", "PICK_PLANET",
    "ADD_OBJECTIVE", "REMOVE_OBJECTIVE", "OBJECTIVE_COMPLETE", "OBJECTIVE_FAILED", "UPDATE_OBJECTIVE",
    "REMOVE_ALL_OBJECTIVES", "ENABLE_OBJECTIVE_DISPLAY", "STORY_GOAL_COMPLETED", "REMOVE_STORY_GOAL",
    "SET_SANDBOX_OBJECTIVES",
    "MULTIMEDIA", "SCREEN_TEXT", "SPEECH", "COMMANDBAR_MOVIE", "STOP_COMMANDBAR_MOVIE", "START_MOVIE", "SFX",
    "DISABLE_MOVIES", "ENABLE_COMBAT_CINEMATIC", "ENABLE_CAMPAIGN_VICTORY_MOVIE", "START_CINEMATIC_MODE",
    "STOP_CINEMATIC_MODE",
    "FLASH_GUI", "FLASH_PLANET_GUI", "FLASH_PLANET", "FLASH_OBJECT", "FLASH_UNIT", "FLASH_TERRAIN",
    "FLASH_FLEET_WITH_UNIT", "FLASH_PRODUCTION_CHOICE", "FLASH_SPECIAL_ABILITY", "FLASH_ADVANCED_MAP_OBJECT",
    "DUAL_FLASH", "HIGHLIGHT_OBJECT", "HIDE_TUTORIAL_CURSOR", "HIDE_CURSOR_ON_CLICK", "FORCE_CLICK_GUI",
    "POSITION_CAMERA", "SCROLL_CAMERA", "SCROLL_LOCK", "ZOOM_IN", "ZOOM_OUT", "SELECT_PLANET",
    "LOCK_PLANET_SELECTION", "LOCK_CONTROLS", "SHOW_COMMAND_BAR", "HIDE_AUTORESOLVE", "RESET_GALACTIC_FILTERS",
    "SHOW_SPECIAL_SLOT", "SHOW_SMUGGLE_SLOT", "HIDE_SMUGGLE_SLOT", "SHOW_RAID_SLOT", "HIDE_RAID_SLOT",
    "SHOW_STEAL_SLOT", "HIDE_STEAL_SLOT", "ENABLE_GALACTIC_CORRUPTION_HOLOGRAM", "TUTORIAL_DIALOG",
    "TUTORIAL_PLAYER", "SET_ADVISOR", "PAUSE_GAME", "PAUSE_GALACTIC", "UNPAUSE_GALACTIC", "PAUSE_GALACTIC_GAME",
    "SWITCH_SIDES", "SWITCH_CONTROL", "TRIGGER_AI", "SET_WEATHER", "INFORMATION", "ENABLE_EVENT", "DISABLE_EVENT"};

enum class Kind : std::uint8_t { trigger, elapsed, flag, tech, click, ai_notification, tactical, pending };

Kind kind_of(std::string_view type) noexcept {
    if (type == "STORY_TRIGGER") return Kind::trigger;
    if (type == "STORY_ELAPSED") return Kind::elapsed;
    if (type == "STORY_FLAG") return Kind::flag;
    if (type == "STORY_TECH_LEVEL") return Kind::tech;
    if (type == "STORY_CLICK_GUI") return Kind::click; // WST-37
    if (type == "STORY_AI_NOTIFICATION") return Kind::ai_notification; // WST-37, WST-55
    if (tactical_event_type(type)) return Kind::tactical;
    return Kind::pending;
}

std::int64_t integer(const std::vector<std::string>& tokens, std::int64_t fallback) {
    if (tokens.empty()) return fallback;
    std::int64_t value{};
    const auto& text = tokens.front();
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    if (begin != end && *begin == '+') ++begin;
    const auto [stop, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && stop == end ? value : fallback;
}
Fixed seconds(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return {};
    auto parsed = Fixed::from_decimal(tokens.front());
    return parsed ? parsed.value() : Fixed{};
}
std::string first(const std::vector<std::string>& tokens) { return tokens.empty() ? std::string{} : tokens.front(); }

// WST-27 comparisons; the keyword is matched exactly (WST-08). Unknown or missing = greater-than,
// NOT_EQUAL_TO has no branch and never fires.
bool compare(std::int64_t flag, std::int64_t value, std::string_view keyword) noexcept {
    if (keyword == "LESS_THAN") return flag < value;
    if (keyword == "EQUAL_TO") return flag == value;
    if (keyword == "GREATER_THAN_EQUAL_TO") return flag >= value;
    if (keyword == "LESS_THAN_EQUAL_TO") return flag <= value;
    if (keyword == "NOT_EQUAL_TO") return false;
    return flag > value;
}

struct Index {
    Kind kind{Kind::pending};
    bool declared_prereqs{};                        // WST-06: any group declared, even one emptied of names
    std::vector<std::vector<std::uint32_t>> groups; // resolved prerequisite events
    std::vector<std::uint32_t> dependants;          // document order (interim WST-U01)
};
} // namespace

std::string upper(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
bool known_event_type(std::string_view type) noexcept {
    return std::find(std::begin(known_events), std::end(known_events), type) != std::end(known_events);
}
bool tactical_event_type(std::string_view type) noexcept {
    return one_of(type, {"STORY_LAND_TACTICAL", "STORY_SPACE_TACTICAL", "STORY_LOAD_TACTICAL_MAP", "STORY_TACTICAL_DESTROY",
        "STORY_LAND_ON", "STORY_DEPLOY", "STORY_MISSION_LOST", "STORY_SELECT_UNIT", "STORY_UNIT_ARRIVED",
        "STORY_FULL_STOP", "STORY_COMMAND_UNIT", "STORY_GUARD_UNIT", "STORY_UNIT_PROXIMITY",
        "STORY_FOG_OBJECT_REVEAL", "STORY_FOG_POSITION_REVEAL", "STORY_CAPTURE_STRUCTURE", "STORY_REINFORCE",
        "STORY_CAPTURE_HERO", "STORY_DEFEAT_HERO", "STORY_GARRISON_UNIT", "STORY_ATTACK_HARDPOINT",
        "STORY_OBJECTIVE_TIMEOUT", "STORY_CHECK_DESTROYED"});
}
bool tactical_reward_type(std::string_view reward) noexcept { return one_of(reward, {"LINK_TACTICAL", "SET_TACTICAL_MAP"}); }

struct Engine::Impl {
    Config config;
    std::vector<PlotRecord> plots;
    std::vector<SubPlotDef> defs;
    State state;
    std::vector<std::vector<Index>> index;
    std::vector<std::map<std::string, std::uint32_t>> by_name;
    std::vector<Triggered> queue;
    std::vector<ScriptCall> script_calls;
    std::vector<Refusal> unavailable, diagnostics;
    int reset_depth{};
    std::map<std::pair<std::size_t, std::size_t>, EventDef> originals; // WST-56: an edited event as loaded
    mutable std::string digest_cache;
    mutable bool digest_dirty{true};
    std::uint64_t listener_revision{}; // bumped when an Event_Param edit can change which components the click events name

    void diagnose(std::size_t s, std::size_t e, std::string what, std::string reason) {
        diagnostics.push_back({defs[s].owner, defs[s].name, defs[s].events[e].name, std::move(what), std::move(reason)});
    }
    [[nodiscard]] std::optional<std::uint32_t> find_event(std::size_t s, std::string_view name) const {
        const auto at = by_name[s].find(upper(name));
        if (at == by_name[s].end()) return std::nullopt;
        return at->second;
    }
    [[nodiscard]] bool eligible(std::size_t s, std::size_t e) const {
        const auto& sub = state.subplots[s];
        const auto& es = sub.events[e];
        return sub.active && es.active && !es.triggered && !es.disabled
            && (config.solo || defs[s].events[e].multiplayer); // WST-14
    }
    void touch() noexcept { digest_dirty = true; }
    void reset_counters(EventState& es) const {
        es.clock = {}; es.clock_last = state.now; es.tech_primed = false; es.idle_stamped = false; es.idle_stamp = {};
    }
    // WST-22.
    void rearm(std::size_t s, std::size_t e) {
        touch();
        auto& es = state.subplots[s].events[e];
        es.triggered = false;
        reset_counters(es);
        if (index[s][e].kind == Kind::trigger) es.active = false; // fires again per activation by its prerequisites
        else { es.active = true; es.start = state.now; }
    }
    // WST-26: a reset event is active again only when it has no prerequisites.
    void reset_event(std::size_t s, std::size_t e) {
        touch();
        auto& es = state.subplots[s].events[e];
        es.triggered = false;
        reset_counters(es);
        es.active = !index[s][e].declared_prereqs;
        es.start = state.now;
    }

    // Sub-plot switch shared by STORY_ELEMENT and Lua Activate (WST-26): clocks of waiting elapsed events start now.
    void open_subplot(std::size_t target) {
        auto& sub = state.subplots[target];
        if (sub.active) return;
        sub.active = true; touch();
        for (std::size_t j = 0; j < sub.events.size(); ++j)
            if (index[target][j].kind == Kind::elapsed && sub.events[j].active && !sub.events[j].triggered)
                reset_counters(sub.events[j]);
    }
    [[nodiscard]] std::optional<std::size_t> find_subplot(std::string_view name) const {
        for (std::size_t s = 0; s < defs.size(); ++s) if (defs[s].name == name) return s;
        return std::nullopt;
    }
    EventEdit& edit_row(std::size_t s, std::size_t e) {
        originals.emplace(std::pair{s, e}, defs[s].events[e]);
        for (auto& row : state.edits) if (row.subplot == defs[s].name && row.event == defs[s].events[e].name) return row;
        state.edits.push_back({defs[s].name, defs[s].events[e].name, {}, {}, {}, {}, {}});
        return state.edits.back();
    }
    core::Result<void> apply_edit(const EventEdit& row);
    void revert_edits() {
        for (const auto& [key, original] : originals) defs[key.first].events[key.second] = original;
        originals.clear();
        state.edits.clear();
    }

    core::Result<void> activate(std::size_t s, std::size_t e, int depth);
    core::Result<void> trigger(std::size_t s, std::size_t e, bool forced, int depth);
    core::Result<void> reward(std::size_t s, std::size_t e, int depth);
    core::Result<void> graph_reward(std::size_t s, std::size_t e, int depth, bool& handled);
    core::Result<void> dispatch_elapsed(Fixed time, bool paused);
    core::Result<void> dispatch_tech(std::optional<Player> player, std::int64_t level);
    core::Result<void> dispatch_click(std::optional<Player> player, std::string_view component);
    core::Result<void> dispatch_ai_notification(std::optional<Player> player, const TextNotice& notice);
    core::Result<void> poll(Fixed time);
};

core::Result<void> Engine::Impl::activate(std::size_t s, std::size_t e, int depth) {
    auto& es = state.subplots[s].events[e];
    if (es.active || es.triggered) return ok(); // WST-21
    for (const auto& group : index[s][e].groups) {
        if (group.empty() || !std::all_of(group.begin(), group.end(), [&](std::uint32_t p) {
                return state.subplots[s].events[p].triggered; })) continue;
        touch();
        es.active = true; es.start = state.now;
        if (index[s][e].kind == Kind::trigger) return trigger(s, e, false, depth); // recursive, same frame
        reset_counters(es); // elapsed clocks start on activation
        return ok();
    }
    return ok();
}

core::Result<void> Engine::Impl::trigger(std::size_t s, std::size_t e, bool forced, int depth) {
    auto& es = state.subplots[s].events[e];
    const auto& def = defs[s].events[e];
    if (es.disabled) return ok(); // WST-20 (1)
    if (index[s][e].kind == Kind::tactical) { // reached by TRIGGER_EVENT or a branch reset: still refused
        diagnose(s, e, def.type, std::string(land_battle_refusal));
        return ok();
    }
    if (def.inactive_delay > Fixed{} && !forced) { // (2) activity restarts the timer
        es.idle_stamp = state.now; es.idle_stamped = true; touch();
        return ok();
    }
    if (depth > max_trigger_depth) {
        diagnose(s, e, "trigger", "trigger chain exceeds the recursion limit of " + std::to_string(max_trigger_depth)
            + " (a TRIGGER_EVENT or prerequisite cycle in the data)");
        return ok();
    }
    // (3) The sub-plot Lua callback runs before the reward (WST-20, WST-55); the host takes the request.
    if (!defs[s].script.empty())
        script_calls.push_back({s, def.authored_name.empty() ? def.name : def.authored_name});
    es.triggered = true; touch(); // (4)
    if (auto done = reward(s, e, depth); !done) return done; // (5)/(6)
    for (const auto dependant : index[s][e].dependants) // (7)
        if (auto done = activate(s, dependant, depth + 1); !done) return done;
    if (def.perpetual) rearm(s, e); // (8)
    return ok();
}

core::Result<void> Engine::Impl::reward(std::size_t s, std::size_t e, int depth) {
    const auto& def = defs[s].events[e];
    const auto known = std::find(std::begin(known_rewards), std::end(known_rewards), def.reward_type) != std::end(known_rewards);
    const bool rewarded = !def.reward_type.empty() && known; // WST-08: an unknown reward name is no reward
    if (!rewarded && def.dialog.empty()) return ok();
    Triggered record;
    record.sequence = ++state.sequence;
    record.owner = defs[s].owner; record.subplot = defs[s].name; record.event = def.name;
    record.dialog = def.dialog; record.tag = def.tag; record.variable = def.variable;
    for (const auto& row : state.edits)
        if (row.subplot == defs[s].name && row.event == def.name) record.dialog_text = row.dialog_text;
    record.chapter = def.chapter; record.popup = def.popup; record.sound = def.sound;
    record.remove_after_viewed = !def.authored_tag_present.value_or(!def.tag.empty());
    if (rewarded) {
        record.reward_type = def.reward_type; record.reward_params = def.reward_params; record.reward_list = def.reward_list;
        if (tactical_reward_type(def.reward_type)) record.refusal = std::string(land_battle_refusal);
    }
    const auto at = queue.size();
    queue.push_back(std::move(record)); // the record precedes what the reward triggers (WST-20 order)
    if (!rewarded) return ok();
    bool handled{};
    if (auto done = graph_reward(s, e, depth, handled); !done) return done;
    queue[at].engine_applied = handled;
    return ok();
}

core::Result<void> Engine::Impl::graph_reward(std::size_t s, std::size_t e, int depth, bool& handled) {
    const auto& def = defs[s].events[e];
    const auto& type = def.reward_type;
    const auto& p = def.reward_params;
    handled = true;
    touch();
    const auto flag_set = [&](std::size_t slot, bool fallback) { return integer(p[slot], fallback ? 1 : 0) != 0; };
    if (type == "TRIGGER_EVENT") {
        // The named event of this sub-plot, else the first sub-plot holding it (document order).
        if (const auto at = find_event(s, first(p[0]))) return trigger(s, *at, false, depth + 1);
        for (std::size_t other = 0; other < defs.size(); ++other)
            if (const auto at = find_event(other, first(p[0]))) return trigger(other, *at, false, depth + 1);
        diagnose(s, e, type, "unknown event " + first(p[0]));
        return ok();
    }
    if (type == "RESET_EVENT") {
        if (const auto at = find_event(s, first(p[0]))) reset_event(s, *at);
        else diagnose(s, e, type, "unknown event " + first(p[0]));
        return ok();
    }
    if (type == "DISABLE_STORY_EVENT") { // WST-25; absent switch disables (unverified)
        const bool disable = flag_set(1, true);
        const bool everywhere = integer(p[2], 0) != 0;
        bool found{};
        for (std::size_t other = 0; other < defs.size(); ++other) {
            if (other != s && !everywhere) continue;
            if (const auto at = find_event(other, first(p[0]))) { state.subplots[other].events[*at].disabled = disable; found = true; }
        }
        if (!found) diagnose(s, e, type, "unknown event " + first(p[0]));
        return ok();
    }
    if (type == "DISABLE_BRANCH" || type == "RESET_BRANCH") {
        const auto branch = upper(first(p[0]));
        std::vector<std::uint32_t> members;
        for (std::uint32_t j = 0; j < defs[s].events.size(); ++j)
            if (!branch.empty() && upper(defs[s].events[j].branch) == branch) members.push_back(j);
        if (type == "DISABLE_BRANCH") {
            const bool disable = flag_set(1, true);
            for (const auto j : members) state.subplots[s].events[j].disabled = disable;
            return ok();
        }
        if (reset_depth >= max_branch_reset_depth) { // WST-24
            diagnose(s, e, type, "branch reset nesting limit reached");
            return ok();
        }
        struct Level {
            int& depth;
            explicit Level(int& d) : depth(d) { ++depth; }
            ~Level() { --depth; }
        } level(reset_depth);
        for (const auto j : members) reset_event(s, j);
        for (const auto j : members) if (auto done = activate(s, j, depth + 1); !done) return done;
        if (!p[1].empty()) {
            if (const auto at = find_event(s, first(p[1]))) return trigger(s, *at, false, depth + 1);
            diagnose(s, e, type, "unknown event " + first(p[1]));
        }
        return ok();
    }
    if (type == "SET_FLAG") { state.flags[upper(first(p[0]))] = integer(p[1], 0); return ok(); }
    if (type == "INCREMENT_FLAG") {
        const auto at = state.flags.find(upper(first(p[0])));
        if (at == state.flags.end()) { diagnose(s, e, type, "undefined flag " + first(p[0])); return ok(); }
        at->second += integer(p[1], 1);
        return ok();
    }
    if (type == "STORY_ELEMENT") { // WST-26: the sub-plot file named by parameter 1 with `.xml` appended
        const auto wanted = first(p[0]) + ".xml";
        std::optional<std::size_t> target;
        for (std::size_t other = 0; other < defs.size() && !target; ++other)
            if (defs[other].name == wanted && defs[other].owner == defs[s].owner) target = other;
        for (std::size_t other = 0; other < defs.size() && !target; ++other)
            if (defs[other].name == wanted) target = other;
        if (!target) { diagnose(s, e, type, "unknown sub-plot " + wanted); return ok(); }
        open_subplot(*target);
        return ok();
    }
    if (type == "TRIGGER_AI") { // the debug build's TRIGGER_AI reward (WST-49): parameter 1 the trigger, 2 the faction, 3 the planet (both optional here)
        const auto name = upper(first(p[0]));
        if (name.empty()) { diagnose(s, e, type, "names no trigger"); return ok(); } // the original needs parameters 1 and 2
        Player target = defs[s].owner;
        if (const auto faction = upper(first(p[1])); !faction.empty()) {
            const auto at = config.factions.find(faction);
            if (at == config.factions.end()) { diagnose(s, e, type, "no player of faction " + first(p[1])); return ok(); }
            target = at->second;
        }
        std::uint64_t planet{};
        if (const auto wanted = upper(first(p[2])); !wanted.empty()) {
            const auto at = config.planets.find(wanted);
            if (at == config.planets.end()) { diagnose(s, e, type, "unknown planet " + first(p[2])); return ok(); }
            planet = at->second;
        }
        const AiFlag flag{target, planet, name};
        const auto where = std::lower_bound(state.ai_flags.begin(), state.ai_flags.end(), flag);
        if (where == state.ai_flags.end() || *where != flag) state.ai_flags.insert(where, flag);
        return ok();
    }
    handled = false; // everything else belongs to the owning service (part 2)
    return ok();
}

core::Result<void> Engine::Impl::dispatch_elapsed(Fixed time, bool paused) {
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active) continue;
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != Kind::elapsed || !eligible(s, e)) continue;
            auto& es = state.subplots[s].events[e];
            // WST-12: unpaused time only; time going backwards resynchronises without adding.
            const auto before = std::pair{es.clock, es.clock_last};
            if (time < es.clock_last) es.clock_last = time;
            else {
                if (!paused) es.clock = Fixed::from_raw(es.clock.raw() + (time.raw() - es.clock_last.raw()));
                es.clock_last = time;
            }
            if (before != std::pair{es.clock, es.clock_last}) touch();
            if (es.clock >= seconds(defs[s].events[e].params[0]))
                if (auto done = trigger(s, e, false, 0); !done) return done;
        }
    }
    return ok();
}

core::Result<void> Engine::Impl::dispatch_tech(std::optional<Player> player, std::int64_t level) {
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active || (player && defs[s].owner != *player)) continue; // WST-14
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != Kind::tech || !eligible(s, e)) continue;
            auto& es = state.subplots[s].events[e];
            const auto wanted = integer(defs[s].events[e].params[0], 0);
            if (wanted == -1) { // the first change after the first value seen; a same-level notice is no change
                if (!es.tech_primed) { es.tech_primed = true; es.tech_last = level; touch(); continue; }
                if (level == es.tech_last) continue;
                es.tech_last = level; touch();
            } else if (level < wanted) continue;
            if (auto done = trigger(s, e, false, 0); !done) return done;
        }
    }
    return ok();
}

// WST-37: a clicked component fires every STORY_CLICK_GUI event of the target player whose Event_Param1 list names it
// (case-insensitive). Each event follows the ordinary eligibility rules, so a spent one-shot event stays quiet.
core::Result<void> Engine::Impl::dispatch_click(std::optional<Player> player, std::string_view component) {
    const auto wanted = upper(std::string(component));
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active || (player && defs[s].owner != *player)) continue; // WST-14
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != Kind::click || !eligible(s, e)) continue;
            const auto& names = defs[s].events[e].params[0];
            if (std::none_of(names.begin(), names.end(), [&](const std::string& name) { return upper(name) == wanted; })) continue;
            if (auto done = trigger(s, e, false, 0); !done) return done;
        }
    }
    return ok();
}

// WST-37/55: Lua Story_Event(name[, planet]) reaches every active sub-plot's STORY_AI_NOTIFICATION events (the debug
// build walks the sub-plots and dispatches synchronously, WST-55). Parameter 1 is the planet list;
// parameters 2..7 are all notification strings. With a planet the list must be non-empty and name it, without one empty.
core::Result<void> Engine::Impl::dispatch_ai_notification(std::optional<Player> player, const TextNotice& notice) {
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active || (player && defs[s].owner != *player)) continue; // WST-14
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != Kind::ai_notification || !eligible(s, e)) continue;
            const auto& params = defs[s].events[e].params;
            std::vector<std::string> planets, texts;
            for (const auto& name : params[0]) planets.push_back(upper(name));
            for (std::size_t slot = 1; slot < token_slots_event; ++slot)
                for (const auto& text : params[slot]) texts.push_back(upper(text));
            TextNotice wanted = notice;
            wanted.text = upper(notice.text);
            if (wanted.planet) wanted.planet = upper(*wanted.planet);
            if (!matches_ai(wanted, planets, texts)) continue;
            if (auto done = trigger(s, e, false, 0); !done) return done;
        }
    }
    return ok();
}

core::Result<void> Engine::Impl::poll(Fixed time) {
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active) continue;
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) { // WST-17 inactivity triggers
            const auto& def = defs[s].events[e];
            if (!(def.inactive_delay > Fixed{}) || !eligible(s, e)) continue;
            auto& es = state.subplots[s].events[e];
            if (!es.idle_stamped) { es.idle_stamped = true; es.idle_stamp = time; touch(); continue; }
            if (time.raw() - es.idle_stamp.raw() >= def.inactive_delay.raw())
                if (auto done = trigger(s, e, true, 0); !done) return done;
        }
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) { // WST-15/27 flag polling
            if (index[s][e].kind != Kind::flag || !eligible(s, e)) continue;
            const auto& def = defs[s].events[e];
            const auto value = integer(def.params[1], 0);
            const auto keyword = first(def.params[2]);
            for (const auto& name : def.params[0]) { // WST-23: no triggered re-check between names
                const auto at = state.flags.find(upper(name));
                if (at == state.flags.end() || !compare(at->second, value, keyword)) continue;
                if (auto done = trigger(s, e, false, 0); !done) return done;
            }
        }
    }
    return ok();
}

core::Result<Engine> Engine::create(Config config, std::vector<PlotRecord> plots, std::vector<SubPlotDef> subplots) {
    auto impl = std::make_unique<Impl>();
    impl->config = config; impl->plots = std::move(plots); impl->defs = std::move(subplots);
    auto& defs = impl->defs;
    impl->index.resize(defs.size()); impl->by_name.resize(defs.size());
    impl->state.subplots.reserve(defs.size());
    for (std::size_t s = 0; s < defs.size(); ++s) {
        auto& sub = defs[s];
        SubPlotState saved;
        saved.name = sub.name; saved.owner = sub.owner; saved.active = sub.active;
        impl->index[s].resize(sub.events.size());
        for (std::uint32_t e = 0; e < sub.events.size(); ++e) {
            auto& def = sub.events[e];
            if (def.name.empty() || def.name != upper(def.name)) return core::Result<Engine>::failure(problem("event names are upper-case and non-empty (WST-04)"));
            if (!impl->by_name[s].emplace(def.name, e).second) // WST-04: a later definition replaces the earlier one
                return core::Result<Engine>::failure(problem("duplicate event " + def.name + " in " + sub.name));
            impl->index[s][e].kind = kind_of(def.type);
            impl->index[s][e].declared_prereqs = !def.prereqs.empty();
            EventState es;
            es.name = def.name; es.active = def.prereqs.empty(); // WST-06
            saved.events.push_back(std::move(es));
        }
        impl->state.subplots.push_back(std::move(saved));
    }
    for (std::size_t s = 0; s < defs.size(); ++s) {
        for (std::uint32_t e = 0; e < defs[s].events.size(); ++e) {
            const auto& def = defs[s].events[e];
            auto& index = impl->index[s][e];
            for (const auto& group : def.prereqs) { // WST-06/07: names resolve inside the same sub-plot
                std::vector<std::uint32_t> resolved;
                for (const auto& name : group) {
                    const auto at = impl->by_name[s].find(upper(name));
                    if (at == impl->by_name[s].end()) {
                        impl->diagnose(s, e, "prereq", "unknown prerequisite " + name); // asserts and is skipped
                        continue;
                    }
                    resolved.push_back(at->second);
                    auto& dependants = impl->index[s][at->second].dependants;
                    if (std::find(dependants.begin(), dependants.end(), e) == dependants.end()) dependants.push_back(e);
                }
                index.groups.push_back(std::move(resolved));
            }
            if (index.kind == Kind::tactical)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type, std::string(land_battle_refusal)});
            else if (index.kind == Kind::pending)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type,
                    "no evaluator yet (the story engine evaluates STORY_TRIGGER, STORY_ELAPSED, STORY_FLAG, STORY_TECH_LEVEL, STORY_CLICK_GUI and STORY_AI_NOTIFICATION)"});
            if (tactical_reward_type(def.reward_type))
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.reward_type, std::string(land_battle_refusal)});
            else if (!def.reward_type.empty() && std::find(std::begin(known_rewards), std::end(known_rewards), def.reward_type) == std::end(known_rewards))
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.reward_type, "unknown reward type: no reward (WST-08)"});
        }
    }
    return core::Result<Engine>::success(Engine(std::move(impl)));
}

Engine::Engine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;
Engine::~Engine() = default;

core::Result<void> Engine::notify(std::optional<Player> player, const Payload& payload) {
    if (const auto* elapsed = std::get_if<ElapsedNotice>(&payload)) return service(elapsed->time_seconds, elapsed->paused);
    if (const auto* economy = std::get_if<EconomyNotice>(&payload); economy && economy->kind == Economy::tech_level) // WST-33
        return impl_->dispatch_tech(player, economy->value);
    if (const auto* text = std::get_if<TextNotice>(&payload); text && text->kind == Text::click_gui) // WST-37
        return impl_->dispatch_click(player, text->text);
    if (const auto* text = std::get_if<TextNotice>(&payload); text && text->kind == Text::ai_notification) // WST-37, WST-55
        return impl_->dispatch_ai_notification(player, *text);
    return ok(); // WST-31..39 evaluators arrive with the next parts; the walk names each seam
}

core::Result<void> Engine::service(Fixed time, bool paused) {
    auto& impl = *impl_;
    impl.state.now = time;
    if (auto done = impl.dispatch_elapsed(time, paused); !done) return done; // WST-11
    return impl.poll(time);
}

std::uint64_t Engine::click_listener_revision() const noexcept { return impl_->listener_revision; }
// #5857: every component name a STORY_CLICK_GUI event of any sub-plot, owner or state lists. Definition-wide on purpose:
// a plot that switches on later must hear the click that comes after it, and the set then changes only by an edit.
std::vector<std::string> Engine::click_listeners() const {
    std::set<std::string> names;
    for (std::size_t s = 0; s < impl_->defs.size(); ++s)
        for (std::size_t e = 0; e < impl_->defs[s].events.size(); ++e)
            if (impl_->index[s][e].kind == Kind::click)
                for (const auto& name : impl_->defs[s].events[e].params[0]) names.insert(upper(name));
    return {names.begin(), names.end()};
}
std::vector<Triggered> Engine::take_triggered() { return std::exchange(impl_->queue, {}); }
std::vector<ScriptCall> Engine::take_script_calls() { return std::exchange(impl_->script_calls, {}); }
const State& Engine::state() const noexcept { return impl_->state; }
bool Engine::started() const noexcept { return impl_->state.started; }
void Engine::mark_started() noexcept { impl_->state.started = true; impl_->touch(); }
std::span<const PlotRecord> Engine::plots() const noexcept { return impl_->plots; }
std::span<const SubPlotDef> Engine::definitions() const noexcept { return impl_->defs; }
std::span<const Refusal> Engine::unavailable() const noexcept { return impl_->unavailable; }
std::span<const Refusal> Engine::diagnostics() const noexcept { return impl_->diagnostics; }

// Applies one saved edit row over the loaded definition of its event.
core::Result<void> Engine::Impl::apply_edit(const EventEdit& row) {
    const auto s = find_subplot(row.subplot);
    const auto e = s ? find_event(*s, row.event) : std::nullopt;
    if (!s || !e) return invalid("edit names an event that is not loaded: " + row.subplot + " " + row.event);
    for (const auto& [slot, tokens] : row.event_params) if (slot >= token_slots_event) return invalid("edit event parameter outside its slots");
    for (const auto& [slot, tokens] : row.reward_params) if (slot >= token_slots_reward) return invalid("edit reward parameter outside its slots");
    originals.emplace(std::pair{*s, static_cast<std::size_t>(*e)}, defs[*s].events[*e]);
    auto& def = defs[*s].events[*e];
    for (const auto& [slot, tokens] : row.event_params) def.params[slot] = tokens;
    if (!row.event_params.empty()) ++listener_revision;
    for (const auto& [slot, tokens] : row.reward_params) def.reward_params[slot] = tokens;
    if (row.reward_type) def.reward_type = *row.reward_type;
    if (row.dialog) def.dialog = *row.dialog;
    touch();
    return ok();
}

core::Result<void> Engine::apply(const PlotCommand& command) {
    auto& impl = *impl_;
    const auto s = impl.find_subplot(command.subplot);
    if (!s) return invalid("Lua names a sub-plot that is not loaded: " + command.subplot);
    using Op = PlotCommand::Op;
    switch (command.op) {
    case Op::activate: impl.open_subplot(*s); return ok();
    case Op::suspend: impl.state.subplots[*s].active = false; impl.touch(); return ok();
    case Op::reset: // WST-26
        for (std::size_t e = 0; e < impl.defs[*s].events.size(); ++e) impl.reset_event(*s, e);
        impl.touch();
        return ok();
    default: break;
    }
    const auto e = impl.find_event(*s, command.event);
    if (!e) return invalid("Lua names an event that is not loaded: " + command.subplot + " " + command.event);
    if (command.op == Op::event_param && command.index >= token_slots_event) return invalid("event parameter index outside Event_Param1..7");
    if (command.op == Op::reward_param && command.index >= token_slots_reward) return invalid("reward parameter index outside Reward_Param1..14");
    auto& row = impl.edit_row(*s, *e);
    auto& def = impl.defs[*s].events[*e];
    const auto text = command.values.empty() ? std::string{} : command.values.front();
    switch (command.op) {
    case Op::event_param: row.event_params[command.index] = command.values; def.params[command.index] = command.values; ++impl.listener_revision; break;
    case Op::reward_param: row.reward_params[command.index] = command.values; def.reward_params[command.index] = command.values; break;
    case Op::reward_type: row.reward_type = text; def.reward_type = text; break;
    case Op::dialog: row.dialog = text; def.dialog = text; break;
    case Op::add_dialog_text: row.dialog_text.insert(row.dialog_text.end(), command.values.begin(), command.values.end()); break;
    case Op::clear_dialog_text: row.dialog_text.clear(); break;
    default: break;
    }
    impl.touch();
    return ok();
}

bool Engine::has_subplot(std::string_view name) const { return impl_->find_subplot(name).has_value(); }
bool Engine::has_event(std::string_view subplot, std::string_view event) const {
    const auto s = impl_->find_subplot(subplot);
    return s && impl_->find_event(*s, event);
}

core::Result<void> Engine::restore(State saved) {
    auto& impl = *impl_;
    if (saved.subplots.size() != impl.defs.size()) return invalid("saved sub-plot list differs from the loaded plots");
    for (std::size_t s = 0; s < saved.subplots.size(); ++s) {
        const auto& row = saved.subplots[s];
        if (row.name != impl.defs[s].name || row.owner != impl.defs[s].owner || row.events.size() != impl.defs[s].events.size())
            return invalid("saved sub-plot " + row.name + " differs from the loaded plots");
        for (std::size_t e = 0; e < row.events.size(); ++e)
            if (row.events[e].name != impl.defs[s].events[e].name)
                return invalid("saved event " + row.events[e].name + " differs from the loaded plots");
    }
    for (const auto& [name, value] : saved.flags) if (name != upper(name)) return invalid("saved flag name is not upper-case");
    if (!std::is_sorted(saved.ai_flags.begin(), saved.ai_flags.end()) ||
        std::adjacent_find(saved.ai_flags.begin(), saved.ai_flags.end()) != saved.ai_flags.end())
        return invalid("saved AI flags are not sorted and unique");
    for (const auto& flag : saved.ai_flags) if (flag.name != upper(flag.name)) return invalid("saved AI flag name is not upper-case");
    // Lua edits: the loaded definitions first, then the saved rows over them.
    // Transactional: a failing row puts the definitions, the originals and the edit rows back as they were.
    auto defs_backup = impl.defs;
    auto originals_backup = impl.originals;
    auto edits_backup = impl.state.edits;
    impl.revert_edits();
    auto edits = std::move(saved.edits);
    saved.edits.clear();
    for (const auto& row : edits) {
        if (auto applied = impl.apply_edit(row); !applied) {
            impl.defs = std::move(defs_backup);
            impl.originals = std::move(originals_backup);
            impl.state.edits = std::move(edits_backup);
            impl.touch();
            return applied;
        }
    }
    impl.state = std::move(saved);
    impl.state.edits = std::move(edits);
    impl.queue.clear(); impl.script_calls.clear(); impl.reset_depth = 0; impl.touch();
    return ok();
}

const std::string& Engine::digest() const {
    if (impl_->digest_dirty) {
        using namespace checkpoint_detail;
        // Everything saved except the story clock, so peers whose event clocks diverged are caught; the digest is
        // recomputed only after something changed (a trigger, an activation, a flag, a moving clock).
        State kept = impl_->state;
        kept.now = {};
        Writer out;
        constexpr std::string_view domain = "eawr-story-engine-v1";
        out(std::string(domain), kept);
        if (!kept.edits.empty() || !kept.ai_flags.empty()) out(kept.edits); // an unedited game keeps its old digest
        if (!kept.ai_flags.empty()) out(kept.ai_flags);
        impl_->digest_cache = core::sha256_hex(out.bytes);
        impl_->digest_dirty = false;
    }
    return impl_->digest_cache;
}

bool Engine::triggered(std::string_view subplot, std::string_view event) const {
    for (std::size_t s = 0; s < impl_->defs.size(); ++s) {
        if (impl_->defs[s].name != subplot) continue;
        if (const auto at = impl_->find_event(s, event)) return impl_->state.subplots[s].events[*at].triggered;
    }
    return false;
}
bool Engine::active(std::string_view subplot, std::string_view event) const {
    for (std::size_t s = 0; s < impl_->defs.size(); ++s) {
        if (impl_->defs[s].name != subplot) continue;
        if (const auto at = impl_->find_event(s, event)) return impl_->state.subplots[s].events[*at].active;
    }
    return false;
}
bool Engine::ai_flag(Player player, std::uint64_t planet, std::string_view name) const {
    const AiFlag key{player, planet, upper(name)};
    return std::binary_search(impl_->state.ai_flags.begin(), impl_->state.ai_flags.end(), key);
}
void Engine::clear_ai_flag(Player player, std::uint64_t planet, std::string_view name) {
    const AiFlag key{player, planet, upper(name)};
    auto& flags = impl_->state.ai_flags;
    const auto at = std::lower_bound(flags.begin(), flags.end(), key);
    if (at == flags.end() || *at != key) return;
    flags.erase(at);
    impl_->touch();
}
std::optional<std::int64_t> Engine::flag(std::string_view name) const {
    const auto at = impl_->state.flags.find(upper(name));
    if (at == impl_->state.flags.end()) return std::nullopt;
    return at->second;
}

} // namespace eawr::sim::galactic::story

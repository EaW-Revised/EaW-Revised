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

enum class Kind : std::uint8_t { trigger, elapsed, flag, tech, click, open_corruption, ai_notification, win, lose, conquer, conquer_count,
    destroy, destroy_base, defeat_hero, enter, move_hero, fleet_bounced, invasion_bounced, construct, construct_level, tactical, pending };

Kind kind_of(std::string_view type) noexcept {
    if (type == "STORY_TRIGGER") return Kind::trigger;
    if (type == "STORY_ELAPSED") return Kind::elapsed;
    if (type == "STORY_FLAG") return Kind::flag;
    if (type == "STORY_TECH_LEVEL") return Kind::tech;
    if (type == "STORY_OPEN_CORRUPTION") return Kind::open_corruption; // WGCH-121
    if (type == "STORY_CLICK_GUI") return Kind::click; // WST-37
    if (type == "STORY_AI_NOTIFICATION") return Kind::ai_notification; // WST-37, WST-55
    if (type == "STORY_WIN_BATTLES") return Kind::win; // WST-35
    if (type == "STORY_LOSE_BATTLES") return Kind::lose;
    if (type == "STORY_CONQUER") return Kind::conquer; // WST-32
    if (type == "STORY_CONQUER_COUNT") return Kind::conquer_count;
    if (type == "STORY_DESTROY") return Kind::destroy; // WST-34
    if (type == "STORY_DESTROY_BASE") return Kind::destroy_base;
    if (type == "STORY_DEFEAT_HERO") return Kind::defeat_hero; // WST-35
    if (type == "STORY_CONSTRUCT") return Kind::construct; // WST-33
    if (type == "STORY_CONSTRUCT_LEVEL") return Kind::construct_level;
    if (type == "STORY_ENTER") return Kind::enter; // WST-31
    if (type == "STORY_MOVE") return Kind::move_hero;
    if (type == "STORY_FLEET_BOUNCED") return Kind::fleet_bounced;
    if (type == "STORY_INVASION_BOUNCED") return Kind::invasion_bounced;
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

bool counting(Kind kind) noexcept {
    return kind == Kind::win || kind == Kind::lose || kind == Kind::conquer || kind == Kind::conquer_count || kind == Kind::destroy
        || kind == Kind::construct || kind == Kind::defeat_hero;
}

// WST-08: the base-kind keyword is looked up exact-case; a spelling that is not in the table reads as the first value
// (ground), so `Land` and `LAND` are ground and any non-`SPACE` spelling is too. An absent parameter is not a keyword:
// it accepts either kind (unverified, WST-U16).
enum class Domain : std::uint8_t { ground, space, either };
Domain domain_of(const std::vector<std::string>& tokens) noexcept {
    if (tokens.empty()) return Domain::either;
    if (tokens.front() == "SPACE") return Domain::space;
    if (tokens.front() == "EITHER") return Domain::either;
    return Domain::ground;
}
bool domain_matches(Domain wanted, BaseKind notice) noexcept {
    return wanted == Domain::either || notice == BaseKind::either
        || (wanted == Domain::space) == (notice == BaseKind::space);
}

// WST-08, WST-32/34: relation filters, relative to the sub-plot's owning player. An absent or unknown keyword is the
// first value, no restriction.
enum class Relation : std::uint8_t { friendly, neutral, enemy };
bool filter_passes(const std::vector<std::string>& tokens, Relation relation) noexcept {
    if (tokens.empty()) return true;
    const auto& key = tokens.front();
    const bool f = relation == Relation::friendly, n = relation == Relation::neutral, e = relation == Relation::enemy;
    if (key == "FILTER_FRIENDLY_ONLY") return f;
    if (key == "FILTER_NEUTRAL_ONLY") return n;
    if (key == "FILTER_ENEMY_ONLY") return e;
    if (key == "FILTER_FRIENDLY_AND_NEUTRAL") return f || n;
    if (key == "FILTER_ENEMY_AND_NEUTRAL") return e || n;
    if (key == "FILTER_FRIENDLY_AND_ENEMY") return f || e;
    return true; // FILTER_NONE, or a spelling outside the table
}
// A planet list: empty accepts any planet, otherwise the notice's planet must be named (case-insensitive).
bool planet_listed(const std::vector<std::string>& tokens, std::string_view planet) {
    if (tokens.empty()) return true;
    const auto wanted = upper(planet);
    return std::any_of(tokens.begin(), tokens.end(), [&](const std::string& name) { return upper(name) == wanted; });
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
    // WST-32/34/35 counters (State::counters): sparse rows, sorted by (sub-plot, event).
    [[nodiscard]] std::vector<EventCounter>::iterator counter_at(std::size_t s, std::size_t e) {
        const std::pair<std::string_view, std::string_view> key{defs[s].name, defs[s].events[e].name};
        return std::lower_bound(state.counters.begin(), state.counters.end(), key,
            [](const EventCounter& row, const std::pair<std::string_view, std::string_view>& wanted) {
                return std::pair<std::string_view, std::string_view>{row.subplot, row.event} < wanted; });
    }
    // The event's running values; a row begins counting from `initial` and stays only until the event fires or resets.
    std::vector<std::int64_t>& counter(std::size_t s, std::size_t e, const std::vector<std::int64_t>& initial) {
        auto at = counter_at(s, e);
        if (at == state.counters.end() || at->subplot != defs[s].name || at->event != defs[s].events[e].name)
            at = state.counters.insert(at, EventCounter{defs[s].name, defs[s].events[e].name, initial});
        else if (at->values.size() != initial.size()) at->values = initial; // a Lua edit changed the type list
        touch();
        return at->values;
    }
    void drop_counter(std::size_t s, std::size_t e) {
        const auto at = counter_at(s, e);
        if (at == state.counters.end() || at->subplot != defs[s].name || at->event != defs[s].events[e].name) return;
        state.counters.erase(at);
        touch();
    }
    [[nodiscard]] Relation relation(Player owner, Player other) const {
        if (owner == other) return Relation::friendly;
        const auto at = config.relations.find({owner, other});
        if (at == config.relations.end()) return Relation::neutral;
        return at->second == Config::Relation::friendly ? Relation::friendly : Relation::enemy;
    }
    // One diagnostic per event and reason, however many notices reach it.
    void diagnose_once(std::size_t s, std::size_t e, const std::string& what, const std::string& reason) {
        for (const auto& row : diagnostics)
            if (row.subplot == defs[s].name && row.event == defs[s].events[e].name && row.what == what && row.reason == reason) return;
        diagnose(s, e, what, reason);
    }
    void reset_counters(EventState& es) const {
        es.clock = {}; es.clock_last = state.now; es.tech_primed = false; es.idle_stamped = false; es.idle_stamp = {};
    }
    // WST-22.
    void rearm(std::size_t s, std::size_t e) {
        touch();
        auto& es = state.subplots[s].events[e];
        es.triggered = false;
        reset_counters(es);
        drop_counter(s, e);
        if (index[s][e].kind == Kind::trigger) es.active = false; // fires again per activation by its prerequisites
        else { es.active = true; es.start = state.now; }
    }
    // WST-26: a reset event is active again only when it has no prerequisites.
    void reset_event(std::size_t s, std::size_t e) {
        touch();
        auto& es = state.subplots[s].events[e];
        es.triggered = false;
        reset_counters(es);
        drop_counter(s, e);
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
    core::Result<void> dispatch_click(std::optional<Player> player, std::string_view component, Kind kind = Kind::click);
    core::Result<void> dispatch_ai_notification(std::optional<Player> player, const TextNotice& notice);
    // WST-32/34/35: `step(s, e)` decides whether one eligible event of the kind fires for the notice (and counts it).
    template <class Step> core::Result<void> dispatch_counting(std::optional<Player> player, Kind kind, Step&& step);
    core::Result<void> dispatch_battle(std::optional<Player> player, const BattleNotice& notice);
    core::Result<void> dispatch_conquest(std::optional<Player> player, const ConquestNotice& notice);
    core::Result<void> dispatch_conquest_count(std::optional<Player> player);
    core::Result<void> dispatch_loss(std::optional<Player> player, const LossNotice& notice);
    core::Result<void> dispatch_travel(std::optional<Player> player, const TravelNotice& notice);
    core::Result<void> dispatch_move(std::optional<Player> player, const TravelNotice& notice);
    core::Result<void> dispatch_economy(std::optional<Player> player, const EconomyNotice& notice);
    // WST-33/34: the per-type countdown of STORY_CONSTRUCT and STORY_DESTROY: the matched type of the list counts down from
    // its entry of the count list (a short list leaves the rest at one); true when that count reaches zero.
    bool count_down_type(std::size_t s, std::size_t e, const std::vector<std::string>& types, const std::vector<std::string>& counts,
        std::string_view type);
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
    drop_counter(s, e); // a fired event counts no more (a perpetual one starts again from zero, WST-22)
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
core::Result<void> Engine::Impl::dispatch_click(std::optional<Player> player, std::string_view component, const Kind kind) {
    const auto wanted = upper(std::string(component));
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active || (player && defs[s].owner != *player)) continue; // WST-14
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != kind || !eligible(s, e)) continue;
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

template <class Step>
core::Result<void> Engine::Impl::dispatch_counting(std::optional<Player> player, Kind kind, Step&& step) {
    for (std::size_t s = 0; s < defs.size(); ++s) {
        if (!state.subplots[s].active || (player && defs[s].owner != *player)) continue; // WST-14
        for (std::size_t e = 0; e < defs[s].events.size(); ++e) {
            if (index[s][e].kind != kind || !eligible(s, e)) continue;
            if (!step(s, e)) continue;
            if (auto done = trigger(s, e, false, 0); !done) return done;
        }
    }
    return ok();
}

// WST-35: a battle result for a player. The notice kind picks the event type, the base kind (space or ground) must
// match, the count runs to Event_Param1 (default 1), and only then the planet list is read. The fleet-content test
// (Event_Param3: a content kind plus ship classes) needs per-side ship classes that the session does not supply yet:
// an event that lists classes is refused with a diagnostic and never matches (never a wrong match).
core::Result<void> Engine::Impl::dispatch_battle(std::optional<Player> player, const BattleNotice& notice) {
    if (notice.kind != Battle::won && notice.kind != Battle::lost) return ok(); // retreat, victory and mission-lost wait for part 2
    return dispatch_counting(player, notice.kind == Battle::won ? Kind::win : Kind::lose, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        if (!domain_matches(domain_of(p[1]), notice.base)) return false;
        if (std::any_of(p[2].begin(), p[2].end(), [](const std::string& token) { return token.starts_with("CLASS_"); })) {
            diagnose_once(s, e, defs[s].events[e].type, "the fleet-content filter lists ship classes but the session supplies no per-side class data yet: the event never matches");
            return false;
        }
        auto& values = counter(s, e, {0});
        ++values[0];
        if (values[0] < integer(p[0], 1)) return false;
        return planet_listed(p[3], notice.planet);
    });
}

// WST-32: a planet changing hands. The planet list (empty = any) and the owner filter (relative to the sub-plot's owner,
// applied to the new owner: unverified, WST-U16) must pass, then the countdown from Event_Param2 (default 1) runs down.
core::Result<void> Engine::Impl::dispatch_conquest(std::optional<Player> player, const ConquestNotice& notice) {
    return dispatch_counting(player, Kind::conquer, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        if (!planet_listed(p[0], notice.planet) || !filter_passes(p[2], relation(defs[s].owner, notice.new_owner))) return false;
        auto& values = counter(s, e, {std::max<std::int64_t>(integer(p[1], 1), 1)});
        return --values[0] <= 0;
    });
}

// WST-32: every conquest adds one to every conquer-count event; no filter, no planet; it fires at Event_Param1.
core::Result<void> Engine::Impl::dispatch_conquest_count(std::optional<Player> player) {
    return dispatch_counting(player, Kind::conquer_count, [&](std::size_t s, std::size_t e) {
        auto& values = counter(s, e, {0});
        ++values[0];
        return values[0] >= integer(defs[s].events[e].params[0], 1);
    });
}

// WST-34: STORY_DESTROY counts the loss of a listed type down from its entry of the count list (a short list leaves the
// rest at one). With a planet list the loss must happen at a listed planet and the destroyed object's owner must pass
// the filter. STORY_DESTROY_BASE fires for a starbase lost at a listed planet (empty = any) of the listed base kind
// whose owner passes the filter.
// WST-35: STORY_DEFEAT_HERO counts the defeat of a listed hero type down from its entry of the count list (parameter 2, a
// short list leaves the rest at one); the owner filter (parameter 3) is relative to the plot's owner and applied to the
// defeated hero's owner. There is no planet list.
core::Result<void> Engine::Impl::dispatch_loss(std::optional<Player> player, const LossNotice& notice) {
    if (notice.kind == Loss::hero_defeat)
        return dispatch_counting(player, Kind::defeat_hero, [&](std::size_t s, std::size_t e) {
            const auto& p = defs[s].events[e].params;
            if (!filter_passes(p[2], relation(defs[s].owner, notice.object_owner))) return false;
            return count_down_type(s, e, p[0], p[1], notice.type);
        });
    if (notice.kind == Loss::destroy_base)
        return dispatch_counting(player, Kind::destroy_base, [&](std::size_t s, std::size_t e) {
            const auto& p = defs[s].events[e].params;
            return planet_listed(p[0], notice.planet) && domain_matches(domain_of(p[1]), notice.base)
                && filter_passes(p[2], relation(defs[s].owner, notice.object_owner));
        });
    return dispatch_counting(player, Kind::destroy, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        const auto wanted = upper(notice.type);
        if (std::none_of(p[0].begin(), p[0].end(), [&](const std::string& type) { return upper(type) == wanted; })) return false;
        if (!p[1].empty() && (!planet_listed(p[1], notice.planet) || !filter_passes(p[3], relation(defs[s].owner, notice.object_owner)))) return false;
        return count_down_type(s, e, p[0], p[2], notice.type);
    });
}

bool Engine::Impl::count_down_type(std::size_t s, std::size_t e, const std::vector<std::string>& types,
    const std::vector<std::string>& counts, std::string_view type) {
    const auto wanted = upper(type);
    const auto hit = std::find_if(types.begin(), types.end(), [&](const std::string& listed) { return upper(listed) == wanted; });
    if (hit == types.end()) return false;
    std::vector<std::int64_t> initial;
    for (std::size_t i = 0; i < types.size(); ++i)
        initial.push_back(std::max<std::int64_t>(i < counts.size() ? integer(std::vector<std::string>{counts[i]}, 1) : 1, 1));
    auto& values = counter(s, e, initial);
    return --values[static_cast<std::size_t>(hit - types.begin())] <= 0;
}

// WST-33: a completed build (STORY_CONSTRUCT) counts its type down from the count list, after the owner filter
// (Event_Param3, relative to the sub-plot's owner, applied to the builder). A starbase completion also sends its base
// level (STORY_CONSTRUCT_LEVEL): the planet list (empty = any), a level at or above Event_Param2 (default 1) and a base
// kind that matches Event_Param3 (absent = either). The level event reaches only the builder's own plots (unverified,
// WST-U18); ground base levels are not announced yet.
core::Result<void> Engine::Impl::dispatch_economy(std::optional<Player> player, const EconomyNotice& notice) {
    if (notice.kind == Economy::construct_level)
        return dispatch_counting(player, Kind::construct_level, [&](std::size_t s, std::size_t e) {
            const auto& p = defs[s].events[e].params;
            return planet_listed(p[0], notice.planet) && domain_matches(domain_of(p[2]), notice.base) && notice.value >= integer(p[1], 1);
        });
    return dispatch_counting(player, Kind::construct, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        if (!filter_passes(p[2], relation(defs[s].owner, notice.object_owner))) return false;
        return count_down_type(s, e, p[0], p[1], notice.type);
    });
}

// WST-31: a fleet linking to a planet's orbit (STORY_ENTER), bounced off a planet (STORY_FLEET_BOUNCED) or a refused
// invasion (STORY_INVASION_BOUNCED) share one test: the planet list (empty = any), the owner filter (Event_Param2,
// relative to the sub-plot's owner, applied to the fleet's owner), every required type (Event_Param3) inside the fleet,
// exclusive (Event_Param6: the unflattened member count equals the number of required types), and, with no required
// types, a stealth fleet passes only when Event_Param5 allows stealth. The orbit-type list (Event_Param4) and the raid
// flag (Event_Param7) need data the session does not supply: an event that uses them is refused with one diagnostic
// and never matches.
// WST-31: STORY_MOVE is sent once per hero (named or generic) in an arriving fleet or in its transports: the hero list
// (Event_Param1) and the planet list (Event_Param2) both match, an empty list matching anything. There is no owner filter.
// A notice with no hero name matches only events with an empty hero list.
core::Result<void> Engine::Impl::dispatch_move(std::optional<Player> player, const TravelNotice& notice) {
    return dispatch_counting(player, Kind::move_hero, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        if (!planet_listed(p[1], notice.planet)) return false;
        if (p[0].empty()) return true;
        if (!notice.hero) return false;
        const auto hero = upper(*notice.hero);
        return std::any_of(p[0].begin(), p[0].end(), [&](const auto& name) { return upper(name) == hero; });
    });
}

core::Result<void> Engine::Impl::dispatch_travel(std::optional<Player> player, const TravelNotice& notice) {
    Kind kind{};
    switch (notice.kind) {
    case Travel::enter: kind = Kind::enter; break;
    case Travel::move_hero: return dispatch_move(player, notice);
    case Travel::fleet_bounced: kind = Kind::fleet_bounced; break;
    case Travel::invasion_bounced: kind = Kind::invasion_bounced; break;
    default: return ok(); // land, deploy and planet-destroyed wait for later batches
    }
    return dispatch_counting(player, kind, [&](std::size_t s, std::size_t e) {
        const auto& p = defs[s].events[e].params;
        if (!planet_listed(p[0], notice.planet) || !filter_passes(p[1], relation(defs[s].owner, notice.fleet.owner))) return false;
        if (!p[3].empty() || integer(p[6], 0) != 0) {
            diagnose_once(s, e, defs[s].events[e].type, "the orbit-type list or the raid flag needs data the session does not supply yet: the event never matches");
            return false;
        }
        std::vector<std::string> have;
        for (const auto& type : notice.fleet.types) have.push_back(upper(type));
        for (const auto& wanted : p[2])
            if (std::find(have.begin(), have.end(), upper(wanted)) == have.end()) return false;
        if (!p[2].empty() && integer(p[5], 0) != 0) {
            const auto direct = notice.fleet.direct_members ? notice.fleet.direct_members : notice.fleet.types.size();
            if (direct != p[2].size()) return false;
        }
        return !p[2].empty() || !notice.fleet.stealth || integer(p[4], 0) != 0;
    });
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
            else if (index.kind == Kind::construct_level && !def.params[2].empty() && domain_of(def.params[2]) == Domain::ground)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type,
                    "evaluated, but only starbase completions send a base level: a ground base level is not announced yet and the event cannot fire"});
            else if (index.kind == Kind::destroy_base)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type,
                    "evaluated, but the session emits no starbase-loss notice yet: the event cannot fire"});
            else if (index.kind == Kind::invasion_bounced)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type,
                    "evaluated, but the session emits no refused-invasion notice yet: the event cannot fire"});
            else if (index.kind == Kind::pending)
                impl->unavailable.push_back({defs[s].owner, defs[s].name, def.name, def.type,
                    "no evaluator yet (the story engine evaluates STORY_TRIGGER, STORY_ELAPSED, STORY_FLAG, STORY_TECH_LEVEL, STORY_CLICK_GUI, STORY_OPEN_CORRUPTION, STORY_AI_NOTIFICATION, STORY_WIN_BATTLES, STORY_LOSE_BATTLES, STORY_CONQUER, STORY_CONQUER_COUNT, STORY_DESTROY, STORY_DESTROY_BASE, STORY_DEFEAT_HERO, STORY_ENTER, STORY_MOVE, STORY_FLEET_BOUNCED, STORY_INVASION_BOUNCED, STORY_CONSTRUCT and STORY_CONSTRUCT_LEVEL)"});
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
    if (const auto* economy = std::get_if<EconomyNotice>(&payload);
        economy && (economy->kind == Economy::construct || economy->kind == Economy::construct_level)) // WST-33
        return impl_->dispatch_economy(player, *economy);
    if (const auto* text = std::get_if<TextNotice>(&payload); text && text->kind == Text::open_corruption)
        return impl_->dispatch_click(player, text->text, Kind::open_corruption);
    if (const auto* text = std::get_if<TextNotice>(&payload); text && text->kind == Text::click_gui) // WST-37
        return impl_->dispatch_click(player, text->text);
    if (const auto* text = std::get_if<TextNotice>(&payload); text && text->kind == Text::ai_notification) // WST-37, WST-55
        return impl_->dispatch_ai_notification(player, *text);
    if (const auto* battle = std::get_if<BattleNotice>(&payload)) return impl_->dispatch_battle(player, *battle); // WST-35
    if (const auto* conquest = std::get_if<ConquestNotice>(&payload)) return impl_->dispatch_conquest(player, *conquest); // WST-32
    if (std::holds_alternative<ConquestCountNotice>(payload)) return impl_->dispatch_conquest_count(player);
    if (const auto* loss = std::get_if<LossNotice>(&payload)) return impl_->dispatch_loss(player, *loss); // WST-34
    if (const auto* travel = std::get_if<TravelNotice>(&payload)) return impl_->dispatch_travel(player, *travel); // WST-31
    return ok(); // WST-31, 33 (except tech), 36..39 evaluators arrive with the next batches; the walk names each seam
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
    case Op::event_param: // a counting event starts again from zero: its count or lists may have changed (WST-56)
        row.event_params[command.index] = command.values; def.params[command.index] = command.values; ++impl.listener_revision;
        impl.drop_counter(*s, *e);
        break;
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

bool Engine::has_event_type(std::string_view type) const {
    for (const auto& sub : impl_->defs)
        for (const auto& event : sub.events) if (event.type == type) return true;
    return false;
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
    for (std::size_t i = 0; i < saved.counters.size(); ++i) {
        const auto& row = saved.counters[i];
        // Sorted and unique on the key (sub-plot, event): a second row for one event would come back as stale state
        // after the first is dropped when the event fires or re-arms.
        if (i > 0 && !(std::pair{std::string_view(saved.counters[i - 1].subplot), std::string_view(saved.counters[i - 1].event)}
            < std::pair{std::string_view(row.subplot), std::string_view(row.event)}))
            return invalid("saved event counters are not sorted and unique by (sub-plot, event)");
        const auto s = impl.find_subplot(row.subplot);
        const auto e = s ? impl.find_event(*s, row.event) : std::nullopt;
        if (!e || !counting(impl.index[*s][*e].kind)) return invalid("saved event counter names an event that does not count: " + row.subplot + " " + row.event);
        if (row.values.empty() || row.values.size() > token_slots_event * 64) return invalid("saved event counter has no values");
    }
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
        const bool counted = !kept.counters.empty();
        if (!kept.edits.empty() || !kept.ai_flags.empty() || counted) out(kept.edits); // an unedited game keeps its old digest
        if (!kept.ai_flags.empty() || counted) out(kept.ai_flags);
        if (counted) out(kept.counters);
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

#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic/command_press.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// #5857 PR 2b: the script-authored production filters, end to end on the shape the shipped filter mods use, with synthetic
// names. A HUD stand-in drives the real press path (CommandPresses: the code behind press_command_control and
// service_forced_clicks) against a real game session, a real plot engine and a real story script:
//   human press -> STORY_CLICK_GUI -> TRIGGER_AI -> the script's Check_Story_Flag poll -> Set_Reward_Parameter and Story_Event
//   (lock / unlock records) -> FORCE_CLICK_GUI (the domain tab, the pause button, the filter button) -> forced press.
// Nothing here names a mod, a faction of any game or a unit: all names are made up.
namespace {
namespace st = eawr::sim::galactic::story;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace cpn = eawr::campaign;
namespace vg = eawr::viewer::galactic;
using eawr::testing::take;
using galactic_test::expect;

std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
void write(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

st::EventDef event(std::string name, std::string type) {
    st::EventDef e;
    e.name = std::move(name); e.type = std::move(type); e.perpetual = true;
    return e;
}
// A STORY_CLICK_GUI event: a click on `control` raises `trigger` for the faction (TRIGGER_AI: 1 trigger, 2 faction).
st::EventDef click(std::string name, std::string control, std::string trigger = {}, bool seen_flag = false) {
    auto e = event(std::move(name), "STORY_CLICK_GUI");
    e.params[0] = {std::move(control)};
    // A reward-less event leaves no record to count, so the pause click sets a harmless engine flag.
    if (seen_flag) { e.reward_type = "SET_FLAG"; e.reward_params[0] = {"PAUSE_SEEN"}; e.reward_params[1] = {"1"}; }
    if (!trigger.empty()) { e.reward_type = "TRIGGER_AI"; e.reward_params[0] = {std::move(trigger)}; e.reward_params[1] = {"Alpha"}; }
    return e;
}
// A script notification (STORY_AI_NOTIFICATION: 1 planet list, 2 the string).
st::EventDef note(std::string name, std::string text, std::string reward = {}, std::vector<std::string> first = {}) {
    auto e = event(std::move(name), "STORY_AI_NOTIFICATION");
    e.params[1] = {std::move(text)};
    e.reward_type = std::move(reward); e.reward_params[0] = std::move(first);
    return e;
}
st::EventDef force_after(std::string name, std::vector<std::vector<std::string>> any_of, std::string control) {
    auto e = event(std::move(name), "STORY_TRIGGER");
    e.prereqs = std::move(any_of);
    e.reward_type = "FORCE_CLICK_GUI"; e.reward_params[0] = {std::move(control)};
    return e;
}

std::vector<st::SubPlotDef> plots() {
    st::SubPlotDef plot;
    plot.name = "Filters.xml"; plot.script = "Filter_Script"; plot.owner = 1;
    plot.events = {
        click("CLICK_FA1", "fa1", "Fa1"), click("CLICK_FA2", "fa2", "Fa2"),
        click("CLICK_FB1", "fb1", "Fb1"), click("CLICK_FB2", "fb2", "Fb2"),
        click("CLICK_TAB_A", "tab_a", "TabA"), click("CLICK_TAB_B", "tab_b", "TabB"),
        click("CLICK_PAUSE", "pausebtn", {}, true),
        force_after("FORCE_TAB_A", {{"CLICK_FA1"}, {"CLICK_FA2"}}, "tab_a"),
        force_after("FORCE_TAB_B", {{"CLICK_FB1"}, {"CLICK_FB2"}}, "tab_b"),
        note("FORCE_FA1", "Force_Fa1", "FORCE_CLICK_GUI", {"fa1"}), note("FORCE_FA2", "Force_Fa2", "FORCE_CLICK_GUI", {"fa2"}),
        note("FORCE_FB1", "Force_Fb1", "FORCE_CLICK_GUI", {"fb1"}), note("FORCE_FB2", "Force_Fb2", "FORCE_CLICK_GUI", {"fb2"}),
        note("PAUSE_NOTE", "Pause_Note", "FORCE_CLICK_GUI", {"pausebtn"}),
        note("MISSING_NOTE", "Force_Missing", "FORCE_CLICK_GUI", {"no_such_control"}),
        note("LOCK_UNIT", "Lock_Note", "LOCK_UNIT"), note("UNLOCK_UNIT", "Unlock_Note", "BUILDABLE_UNIT"),
    };
    return {plot};
}

constexpr std::string_view library = R"LUA(
function Base_Definitions() end
function Sleep(seconds)
    local started = GetCurrentTime()
    while GetCurrentTime() - started < seconds do coroutine.yield(true) end
end
)LUA";

// The shape of a filter script: a state callback per click, a polling thread on game time, and a main thread that reacts to
// the pause. Lua 5.0 level (no # operator).
constexpr std::string_view script = R"LUA(
require("StoryProbe")
ServiceRate = 0.1
Paused = false
PendingUnpause = false
NeedPause = false
Applied = 0
Active = 0
Units = {"Unit_One", "Unit_Two", "Unit_Three"}
Flags = {"Fa1", "Fa2", "Fb1", "Fb2"}
Forces = {"Force_Fa1", "Force_Fa2", "Force_Fb1", "Force_Fb2"}
Domain = {"A", "A", "B", "B"}

function Story_Event_Trigger(name)
    if name == "CLICK_PAUSE" then
        Paused = not Paused
    elseif name == "CLICK_FA1" or name == "CLICK_FA2" or name == "CLICK_FB1" or name == "CLICK_FB2" then
        if Paused then PendingUnpause = true end
    end
end

function Keep(lock, unlock, kept)
    for i = 1, 3 do
        if i == kept then
            unlock.Set_Reward_Parameter(0, Units[i])
            Story_Event("Unlock_Note")
        else
            lock.Set_Reward_Parameter(0, Units[i])
            Story_Event("Lock_Note")
        end
    end
end

function Choose(i, lock, unlock)
    if Active == i then
        Active = 0
        for k = 1, 3 do
            unlock.Set_Reward_Parameter(0, Units[k])
            Story_Event("Unlock_Note")
        end
    else
        Active = i
        Keep(lock, unlock, i)
    end
    Applied = Applied + 1
    if NeedPause then
        NeedPause = false
        Story_Event("Pause_Note")
    end
end

function PollThread()
    local player = Find_Player("Alpha")
    local plot = Get_Story_Plot("Filters.xml")
    local lock = plot.Get_Event("Lock_Unit")
    local unlock = plot.Get_Event("Unlock_Unit")
    while true do
        for i = 1, 4 do
            if Check_Story_Flag(player, Flags[i], nil, true) then Choose(i, lock, unlock) end
        end
        local tab = ""
        if Check_Story_Flag(player, "TabA", nil, true) then tab = "A" end
        if Check_Story_Flag(player, "TabB", nil, true) then tab = "B" end
        if tab ~= "" and Active ~= 0 and Domain[Active] == tab then
            local forced = Forces[Active]
            Active = 0
            for k = 1, 3 do
                unlock.Set_Reward_Parameter(0, Units[k])
                Story_Event("Unlock_Note")
            end
            Story_Event(forced)
        end
        Sleep(0.1)
    end
end

function main()
    Create_Thread("PollThread")
    while true do
        if PendingUnpause then
            PendingUnpause = false
            NeedPause = true
            Story_Event("Pause_Note")
        end
        coroutine.yield(true)
    end
end
)LUA";

// The HUD's side of the press path: controls with authored flags, the real CommandPresses, and the effects a press has.
struct Hud final : vg::CommandPresses::Surface, vg::CommandPresses::Effects {
    using Target = vg::CommandPresses::Target;
    cpn::GameSession& game;
    vg::ControlStates states;
    vg::CommandPresses presses;
    std::map<std::string, vg::CommandPresses::Target> controls;
    std::vector<std::string> listeners; // what the plots listen to, as LiveGameSession::story_listens_for_click reads it
    std::vector<std::string> log;       // every press, "human:<control>" or "forced:<control>"
    std::vector<std::string> drops;
    std::map<std::string, int> fired;   // triggered plot events by name
    std::uint64_t sequence{};
    int refreshes{};
    std::string view{"a"};

    explicit Hud(cpn::GameSession& session) : game(session) {
        const auto add = [&](const char* name, std::vector<std::string> groups, bool toggle, bool tab) {
            states.add(name, std::move(groups), toggle, tab);
            controls[name] = {true, false, true};
        };
        add("fa1", {"Pane_A"}, true, true); add("fa2", {"Pane_A"}, true, true);
        add("fb1", {"Pane_B"}, true, true); add("fb2", {"Pane_B"}, true, true);
        add("tab_a", {"Build_Tab"}, false, true); add("tab_b", {"Build_Tab"}, false, true);
        add("pausebtn", {}, false, false);
        listeners = game.story_engine()->click_listeners();
    }
    [[nodiscard]] bool story_listens(const std::string& name) const override {
        return std::binary_search(listeners.begin(), listeners.end(), upper(name));
    }
    void raise_click(const std::string& name) override {
        take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{name}}));
    }
    void action(const std::string& name) override {
        if (name == "tab_a") view = "a";
        else if (name == "tab_b") view = "b";
        else if (name == "pausebtn") {
            const bool is_paused = game.galaxy().clock().pause_depth != 0;
            if (is_paused) take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::ResumeGalaxy{}}));
            else take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::PauseGalaxy{}}));
        }
    }
    void refresh() override { ++refreshes; }
    [[nodiscard]] Target find(std::string_view component) const override {
        const auto at = controls.find(std::string(component));
        if (at != controls.end()) return at->second;
        for (const auto& [name, target] : controls) if (upper(name) == upper(std::string(component))) return target;
        return {};
    }
    void activate(std::string_view component) override { // the button's action: the same path as a human press
        log.push_back("forced:" + std::string(component));
        presses.press(states, std::string(component), *this);
    }
    void human(const std::string& name) {
        log.push_back("human:" + name);
        presses.press(states, name, *this);
    }
    // One frame: a game step, then the forced requests it published, the way the HUD's update does.
    void frame(const eawr::sim::PartitionExecutor& executor) {
        const auto tick = take(game.step(executor));
        for (const auto& row : tick.story_triggers) ++fired[row.event];
        std::vector<std::string> components;
        for (const auto& row : tick.story_presentation) {
            const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
            if (request && request->consumer == st::Consumer::gui && request->reward_name == "FORCE_CLICK_GUI" && row.owner == 1
                && !request->parameters.empty()) components.push_back(request->parameters.front());
        }
        presses.drain(components, *this, gc::max_gui_clicks_per_tick, [this](const vg::CommandPresses::Drop& dropped) {
            drops.push_back(dropped.component);
        });
    }
    void settle(const eawr::sim::PartitionExecutor& executor, int frames) { for (int i = 0; i < frames; ++i) frame(executor); }
    [[nodiscard]] int count(const std::string& event_name) const {
        const auto at = fired.find(event_name);
        return at == fired.end() ? 0 : at->second;
    }
    [[nodiscard]] bool paused() const { return game.galaxy().clock().pause_depth != 0; }
};

struct Outcome {
    std::string digest, trail;
};

std::int64_t global(cpn::GameSession& game, std::uint64_t instance, std::string_view name) {
    const auto value = take(game.galactic_scripts()->story_scheduler().read_global(instance, name));
    const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
    return n ? static_cast<std::int64_t>(static_cast<std::int32_t>(*n)) : -1;
}

Outcome flow(std::size_t workers, const std::filesystem::path& root) {
    gc::SyntheticGalaxyOptions shape; shape.players = 2; shape.planets = 4;
    const auto setup = gc::synthetic_galaxy(shape);
    const auto folder = root / ("w" + std::to_string(workers));
    std::filesystem::remove_all(folder);
    write(folder / "loose" / "scripts" / "library" / "storyprobe.lua", std::string(library));
    write(folder / "loose" / "scripts" / "story" / "Filter_Script.lua", std::string(script));
    eawr::vfs::MountSpec mount; mount.layer_id = "filter-flow-" + std::to_string(workers); mount.data_root = folder / "loose";
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    const auto subs = plots();
    const auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    expect(content->notes().empty(), "the filter script is a Story module of the mounted data");

    st::Config config;
    config.factions = {{"ALPHA", 1}, {"BETA", 2}};
    cpn::GalacticStoryPlayers players;
    players.factions = {"Alpha", "Beta"};
    players.players = {{1, "Alpha"}, {2, "Beta"}};
    auth::SessionConfig scheduler_config;
    scheduler_config.seed = setup.seed; scheduler_config.galactic_clock = true; scheduler_config.tick_duration = {1, 30};
    const auto link = std::make_shared<cpn::GalacticStoryLink>();
    auto ai = take(auth::ScriptScheduler::create(scheduler_config, {}));
    auto story = take(cpn::create_galactic_story_scheduler(*content, scheduler_config, false, players, {}, {}, link));
    std::vector<std::string> notes;
    take(cpn::start_galactic_story_scripts(story, *content, notes));
    expect(notes.empty(), "the filter script starts");

    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto game = take(cpn::GameSession::create(setup));
    take(game.install_story(config, {{1, "plot.xml"}}, subs));
    take(game.install_galactic_scripts(std::move(ai), std::move(story)));
    game.bind_story_scripts(content);
    take(game.connect_story_link(link));
    const auto instance = content->find("Filters.xml")->instance;
    Hud hud(game);
    expect(hud.listeners.size() == 7 && std::binary_search(hud.listeners.begin(), hud.listeners.end(), std::string("FA1")),
        "the plots' click listeners are the controls their events name");
    hud.settle(executor, 6); // the script's threads are up

    // A. a space category: one human press, one click; the forced domain tab press raises no click, so nothing loops.
    hud.human("fa1");
    hud.settle(executor, 14);
    expect(hud.count("CLICK_FA1") == 1 && hud.count("FORCE_TAB_A") == 1, "a human press raises one click and its force event");
    expect(hud.count("CLICK_TAB_A") == 0, "the forced press of the domain tab raises no click: no story loop (WST-75)");
    expect(global(game, instance, "Applied") == 1 && !game.story_engine()->ai_flag(1, 0, "Fa1"), "the script consumed the TRIGGER_AI flag once");
    expect(hud.count("LOCK_UNIT") == 2 && hud.count("UNLOCK_UNIT") == 1,
        "one Set_Reward_Parameter and Story_Event per unit: two locks and the category's unlock");
    expect(hud.states.selected("fa1") && hud.states.selected("tab_a") && hud.view == "a",
        "the filter stays selected and the forced tab press selected the tab");
    const auto presses_after_a = hud.log.size();
    hud.settle(executor, 30);
    expect(hud.log.size() == presses_after_a && hud.count("CLICK_FA1") == 1, "the flow is quiet afterwards");

    // B. another category of the group replaces the first.
    hud.human("fa2");
    hud.settle(executor, 14);
    expect(hud.states.selected("fa2") && !hud.states.selected("fa1"), "a second category replaces the first (group exclusion)");
    expect(global(game, instance, "Applied") == 2 && hud.count("LOCK_UNIT") == 4 && hud.count("UNLOCK_UNIT") == 2, "and applies its own lock list");

    // C. the domain tab resets: the script forces the active filter button off; that press raises no click either.
    hud.human("tab_a");
    hud.settle(executor, 14);
    expect(hud.count("CLICK_TAB_A") == 1 && hud.count("CLICK_FA2") == 1, "only the human press of the tab raised a click");
    expect(!hud.states.selected("fa2") && hud.states.selected("tab_a"), "the script's forced press cleared the active filter button");
    expect(hud.count("UNLOCK_UNIT") == 5, "and the offer list was restored by three unlocks");

    // D. the ground branch of the same script, on its own controls.
    hud.human("fb1");
    hud.settle(executor, 14);
    expect(hud.count("CLICK_FB1") == 1 && hud.count("FORCE_TAB_B") == 1 && hud.count("CLICK_TAB_B") == 0, "a ground category forces the ground tab, with no loop");
    expect(hud.view == "b" && hud.states.selected("tab_b") && !hud.states.selected("tab_a") && hud.states.selected("fb1"),
        "the ground tab replaces the space tab, the ground filter is selected");
    hud.human("fb1");
    hud.settle(executor, 14);
    expect(!hud.states.selected("fb1") && global(game, instance, "Applied") == 4, "the same category again turns it off");

    // E. the pause dance: a filter pressed while the game is paused. The script's main thread is pumped while the galaxy is
    // paused (the synchronizer clock), the polling thread sleeps on frozen game time; so the script unpauses through the
    // pause button, lets the thread apply the filter, and pauses again. Forced presses raise no pause click.
    hud.human("pausebtn");
    hud.settle(executor, 4);
    expect(hud.paused() && hud.count("CLICK_PAUSE") == 1, "the human pause is heard by the script");
    const auto applied = global(game, instance, "Applied");
    hud.human("fa1");
    hud.settle(executor, 40);
    expect(global(game, instance, "Applied") == applied + 1, "the filter pressed while paused was applied");
    expect(hud.paused(), "and the game is paused again afterwards");
    expect(hud.count("CLICK_PAUSE") == 1, "the script's own pause button presses raised no pause click");
    expect(std::count(hud.log.begin(), hud.log.end(), "forced:pausebtn") == 2, "the dance is one unpause and one pause");

    // F. a forced press of a control the HUD does not have is dropped, once, and the match goes on.
    const auto dropped_before = hud.drops.size();
    for (int round = 0; round < 2; ++round)
        hud.presses.drain(std::vector<std::string>{"no_such_control", "no_such_control"}, hud, 8, [&](const vg::CommandPresses::Drop& dropped) {
            hud.drops.push_back(dropped.component);
        });
    expect(hud.drops.size() == dropped_before + 1, "an absent control is reported once per reason, never an abort");

    Outcome out;
    out.digest = game.state_sha256();
    for (const auto& line : hud.log) out.trail += line + ";";
    for (const auto& [name, times] : hud.fired) out.trail += name + "=" + std::to_string(times) + ";";
    return out;
}

void filters(const std::filesystem::path& root) {
    Outcome reference;
    bool first = true;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const auto run = flow(workers, root);
        if (first) { reference = run; first = false; continue; }
        expect(run.digest == reference.digest, "the session digest is equal on " + std::to_string(workers) + " workers");
        expect(run.trail == reference.trail, "the presses and the fired events are equal on " + std::to_string(workers) + " workers");
    }
}
} // namespace

int case_galactic_story_filter_flow_tests(int argc, char** argv) {
    try { filters(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-filter-flow")); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

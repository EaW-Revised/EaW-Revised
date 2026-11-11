#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// #5857 PR 2a: the story scripts' bridge to the plot engine. TRIGGER_AI flags and the Lua Check_Story_Flag, the
// STORY_AI_NOTIFICATION evaluator and the Lua Story_Event, with the reward parameters captured at each call (WST-37, WST-49,
// WST-55, WST-56). Synthetic names only: no mod data, no faction, flag or unit name of any mod.
namespace {
namespace st = eawr::sim::galactic::story;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace cpn = eawr::campaign;
namespace cp = eawr::checkpoint;
using eawr::testing::take;
using galactic_test::expect;
using Names = std::vector<std::string>;

st::EventDef make(std::string name, std::string type, std::vector<std::string> p1 = {},
    std::vector<std::vector<std::string>> prereqs = {}) {
    st::EventDef e;
    e.name = std::move(name); e.type = std::move(type);
    e.params[0] = std::move(p1); e.prereqs = std::move(prereqs);
    return e;
}
st::EventDef trig(std::string name, std::vector<std::vector<std::string>> prereqs, std::string reward,
    std::vector<std::string> r1 = {}, std::vector<std::string> r2 = {}, std::vector<std::string> r3 = {}) {
    auto e = make(std::move(name), "STORY_TRIGGER", {}, std::move(prereqs));
    e.reward_type = std::move(reward);
    e.reward_params[0] = std::move(r1); e.reward_params[1] = std::move(r2); e.reward_params[2] = std::move(r3);
    return e;
}
// A perpetual STORY_AI_NOTIFICATION event: parameter 1 is the planet list, the rest are notification strings.
st::EventDef notified(std::string name, std::vector<std::string> planets, std::vector<std::string> texts, std::string reward = "SET_FLAG") {
    auto e = make(std::move(name), "STORY_AI_NOTIFICATION", std::move(planets));
    e.params[1] = std::move(texts);
    e.reward_type = std::move(reward);
    e.perpetual = true;
    return e;
}
st::SubPlotDef sub(std::string name, st::Player owner, std::vector<st::EventDef> events, std::string script = {}) {
    st::SubPlotDef s;
    s.name = std::move(name); s.owner = owner; s.script = std::move(script); s.events = std::move(events);
    return s;
}
st::Engine engine(std::vector<st::SubPlotDef> subs, st::Config config = {}) {
    std::vector<st::PlotRecord> plots;
    for (const auto& s : subs) if (std::none_of(plots.begin(), plots.end(), [&](const auto& p) { return p.owner == s.owner; }))
        plots.push_back({s.owner, "plot.xml"});
    return take(st::Engine::create(config, std::move(plots), std::move(subs)));
}
Names names(const std::vector<st::Triggered>& rows) {
    Names out;
    for (const auto& row : rows) out.push_back(row.event);
    return out;
}
st::TextNotice text(std::string value, std::optional<std::string> planet = std::nullopt) {
    return {st::Text::ai_notification, std::move(value), std::move(planet)};
}

// TRIGGER_AI: parameter 1 the trigger, 2 the faction, 3 the planet (the debug build's TRIGGER_AI reward, WST-49).
void trigger_ai() {
    st::Config config;
    config.factions = {{"ALPHA", 1}, {"BETA", 2}};
    config.planets = {{"HOMEWORLD", 7}};
    auto e = engine({sub("S.xml", 1, {
        make("START", "STORY_ELAPSED", {"0"}),
        trig("ARM_A", {{"START"}}, "TRIGGER_AI", {"Mode_One"}, {"Alpha"}),
        trig("ARM_B", {{"START"}}, "TRIGGER_AI", {"Mode_One"}, {"beta"}),
        trig("ARM_P", {{"START"}}, "TRIGGER_AI", {"Near"}, {"Alpha"}, {"Homeworld"}),
        trig("ARM_OWNER", {{"START"}}, "TRIGGER_AI", {"Owner_Only"}),
        trig("BAD_FACTION", {{"START"}}, "TRIGGER_AI", {"Mode_Two"}, {"Nobody"}),
        trig("BAD_PLANET", {{"START"}}, "TRIGGER_AI", {"Mode_Two"}, {"Alpha"}, {"Nowhere"}),
        trig("NO_NAME", {{"START"}}, "TRIGGER_AI"),
    })}, config);
    take(e.service(eawr::sim::math::Fixed{}, false));
    expect(e.ai_flag(1, 0, "MODE_ONE") && e.ai_flag(1, 0, "mode_one"), "WST-49 TRIGGER_AI sets the flag of the faction's player; names match any case");
    expect(e.ai_flag(2, 0, "Mode_One"), "a second faction gets its own flag");
    expect(e.ai_flag(1, 7, "Near") && !e.ai_flag(1, 0, "Near"), "the planet parameter keys the flag; nil and a planet are different keys");
    expect(e.ai_flag(1, 0, "Owner_Only") && !e.ai_flag(2, 0, "Owner_Only"), "without a faction the plot's owner is the target");
    expect(!e.ai_flag(1, 0, "Mode_Two") && !e.ai_flag(2, 0, "Mode_Two"), "an unknown faction or planet sets nothing");
    expect(e.diagnostics().size() == 3, "an unknown faction, an unknown planet and a missing name are each diagnosed once");
    const auto queued = e.take_triggered();
    expect(std::all_of(queued.begin(), queued.end(), [](const auto& row) { return row.reward_type != "TRIGGER_AI" || row.engine_applied; }),
        "the engine applies TRIGGER_AI itself: the reward consumers skip it");

    const auto before = e.digest();
    e.clear_ai_flag(1, 0, "mode_one");
    expect(!e.ai_flag(1, 0, "Mode_One") && e.ai_flag(2, 0, "Mode_One") && e.digest() != before, "a clear removes one flag and joins the digest");
    e.clear_ai_flag(1, 0, "Mode_One");
    auto saved = e.state();
    expect(saved.ai_flags.size() == 3, "the remaining flags are state");
    auto again = engine({sub("S.xml", 1, {make("START", "STORY_ELAPSED", {"0"})})}, config);
    expect(again.digest() != e.digest(), "a game without flags digests differently");
    auto fresh = engine({sub("S.xml", 1, {
        make("START", "STORY_ELAPSED", {"0"}),
        trig("ARM_A", {{"START"}}, "TRIGGER_AI", {"Mode_One"}, {"Alpha"}),
        trig("ARM_B", {{"START"}}, "TRIGGER_AI", {"Mode_One"}, {"beta"}),
        trig("ARM_P", {{"START"}}, "TRIGGER_AI", {"Near"}, {"Alpha"}, {"Homeworld"}),
        trig("ARM_OWNER", {{"START"}}, "TRIGGER_AI", {"Owner_Only"}),
        trig("BAD_FACTION", {{"START"}}, "TRIGGER_AI", {"Mode_Two"}, {"Nobody"}),
        trig("BAD_PLANET", {{"START"}}, "TRIGGER_AI", {"Mode_Two"}, {"Alpha"}, {"Nowhere"}),
        trig("NO_NAME", {{"START"}}, "TRIGGER_AI"),
    })}, config);
    take(fresh.restore(saved));
    expect(fresh.digest() == e.digest() && !fresh.ai_flag(1, 0, "Mode_One") && fresh.ai_flag(2, 0, "Mode_One"), "the flags restore with the state");
    auto bad = saved;
    std::reverse(bad.ai_flags.begin(), bad.ai_flags.end());
    expect(!fresh.restore(bad), "saved flags out of order are refused");
    bad = saved; bad.ai_flags.front().name = "lower";
    expect(!fresh.restore(bad), "a saved flag name must be upper-case");
}

// STORY_AI_NOTIFICATION (WST-37): parameter 1 the planet list, parameters 2..7 the strings, case-insensitive; with a planet
// the list must be non-empty and name it, without one it must be empty. Lua Story_Event reaches every plot's events.
void ai_notification() {
    auto e = engine({
        sub("S.xml", 1, {
            notified("PLAIN", {}, {"Hello"}),
            notified("ON_PLANET", {"Homeworld"}, {"hello"}),
            notified("NO_TEXT", {}, {}),
        }),
        sub("T.xml", 2, {notified("OTHER_OWNER", {}, {"HELLO"})}),
    });
    take(e.notify(std::nullopt, text("HELLO")));
    // The debug build's Lua Story_Event names no player, so the notice reaches every active sub-plot whoever owns it (WST-55).
    expect(names(e.take_triggered()) == Names{"PLAIN", "OTHER_OWNER"}, "WST-37 an object-less notification fires the events with an empty planet list, in every owner's plot (WST-55: no player named)");
    take(e.notify(std::nullopt, text("hello", std::string("HOMEWORLD"))));
    expect(names(e.take_triggered()) == Names{"ON_PLANET"}, "with a planet the list must name it (case-insensitive)");
    take(e.notify(std::nullopt, text("hello", std::string("Elsewhere"))));
    expect(e.take_triggered().empty(), "a planet outside the list fires nothing");
    take(e.notify(std::nullopt, text("Nothing")));
    expect(e.take_triggered().empty(), "an unknown string fires nothing");
    take(e.notify(std::nullopt, text("hello")));
    take(e.notify(std::nullopt, text("hello")));
    expect(names(e.take_triggered()) == Names{"PLAIN", "OTHER_OWNER", "PLAIN", "OTHER_OWNER"}, "a perpetual event answers every notification");
    take(e.notify(2, text("hello")));
    expect(names(e.take_triggered()) == Names{"OTHER_OWNER"}, "a notification addressed to one player reaches only that player's plots (WST-14)");
    expect(e.unavailable().empty(), "STORY_AI_NOTIFICATION has an evaluator");

    // Parameters 3..7 are notification strings too.
    auto wide = notified("WIDE", {}, {});
    wide.params[3] = {"Fourth"};
    auto w = engine({sub("S.xml", 1, {wide})});
    take(w.notify(std::nullopt, text("fourth")));
    expect(names(w.take_triggered()) == Names{"WIDE"}, "WST-37 every parameter after the planet list holds notification strings");
    take(w.notify(std::nullopt, text("HOMEWORLD")));
    expect(w.take_triggered().empty(), "the planet list is not a string list");
}

// The reward parameters a record carries are the ones set before the notification that fired it (the debug build's
// the debug build dispatches to the sub-plots at the call, WST-55): N edits and N notifications make N records.
void call_time_parameters() {
    auto e = engine({sub("S.xml", 1, {notified("LOCK", {}, {"Lock_Note"}, "LOCK_UNIT")})});
    for (const auto* unit : {"Alpha", "Beta", "Gamma"}) {
        st::PlotCommand set;
        set.op = st::PlotCommand::Op::reward_param; set.subplot = "S.xml"; set.event = "LOCK"; set.index = 0; set.values = {unit};
        take(e.apply(set));
        take(e.notify(std::nullopt, text("LOCK_NOTE")));
    }
    const auto rows = e.take_triggered();
    Names units;
    for (const auto& row : rows) units.push_back(row.reward_params[0].empty() ? std::string{} : row.reward_params[0].front());
    expect(units == Names{"Alpha", "Beta", "Gamma"} && rows.size() == 3,
        "WST-55 each notification carries the reward parameter set just before it, not the last one set");
    expect(rows[0].sequence < rows[1].sequence && rows[1].sequence < rows[2].sequence, "the records keep call order");
}

// ---- the same, driven by Lua through a game session ----------------------------------------------------------------

void write(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

constexpr std::string_view library = R"LUA(
function Base_Definitions() end
function Story_Event_Trigger(name) end
)LUA";

// The script polls its flag, consumes it, and for each of three units sets the reward parameter then sends the event.
constexpr std::string_view script = R"LUA(
require("StoryProbe")
Seen = 0
Wrong = 0
function main()
    local player = Find_Player("Alpha")
    local other = Find_Player("Beta")
    local locker = Get_Story_Plot("S.xml").Get_Event("LOCK")
    local units = {"Unit_One", "Unit_Two", "Unit_Three"}
    while true do
        if Check_Story_Flag(other, "Mode_One", nil, true) then Wrong = Wrong + 1 end
        if Check_Story_Flag(player, "Mode_One", nil, true) then
            Seen = Seen + 1
            for i = 1, 3 do
                locker.Set_Reward_Parameter(0, units[i])
                Story_Event("Lock_Note")
            end
        end
        coroutine.yield(true)
    end
end
)LUA";

struct Run {
    Names locked;
    std::string hash;
    std::int64_t seen{-1}, wrong{-1};
    bool flag_left{true};
    std::size_t locks_after{};
};

Run session(std::size_t workers, const std::filesystem::path& root) {
    gc::SyntheticGalaxyOptions shape; shape.players = 2; shape.planets = 4;
    const auto setup = gc::synthetic_galaxy(shape);
    const auto data = root / ("w" + std::to_string(workers)) / "loose";
    std::filesystem::remove_all(root / ("w" + std::to_string(workers)));
    write(data / "scripts" / "library" / "storyprobe.lua", std::string(library));
    write(data / "scripts" / "story" / "Flag_Script.lua", std::string(script));
    eawr::vfs::MountSpec mount; mount.layer_id = "story-bridge-" + std::to_string(workers); mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));

    auto lock = notified("LOCK", {}, {"Lock_Note"}, "LOCK_UNIT");
    const std::vector<st::SubPlotDef> subs{sub("S.xml", 1, {
        make("START", "STORY_ELAPSED", {"0"}),
        trig("ARM", {{"START"}}, "TRIGGER_AI", {"Mode_One"}, {"Alpha"}),
        lock}, "Flag_Script")};
    st::Config config;
    config.factions = {{"ALPHA", 1}, {"BETA", 2}};
    const auto content = take(cpn::GalacticStoryScripts::load(files, subs));
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
    expect(notes.empty(), "the bridge script starts");

    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto game = take(cpn::GameSession::create(setup));
    take(game.install_story(config, {{1, "plot.xml"}}, subs));
    take(game.install_galactic_scripts(std::move(ai), std::move(story)));
    game.bind_story_scripts(content);
    expect(!game.connect_story_link(nullptr), "a missing link is refused");
    take(game.connect_story_link(link));

    Run run;
    for (int step = 0; step < 8; ++step) {
        const auto tick = take(game.step(executor));
        for (const auto& row : tick.story_triggers)
            if (row.event == "LOCK") run.locked.push_back(row.reward_params[0].empty() ? std::string{} : row.reward_params[0].front());
        for (const auto& row : tick.diagnostics)
            if (row.code.find("SCRIPT") != std::string::npos && row.message.find("Check_Story_Flag") != std::string::npos)
                std::cerr << "script diagnostic: " << row.message << '\n';
    }
    run.flag_left = game.story_engine()->ai_flag(1, 0, "Mode_One");
    const auto read = [&](std::string_view name) {
        const auto value = take(game.galactic_scripts()->story_scheduler().read_global(content->find("S.xml")->instance, name));
        const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
        return n ? static_cast<std::int64_t>(static_cast<std::int32_t>(*n)) : std::int64_t{-1};
    };
    run.seen = read("Seen"); run.wrong = read("Wrong");
    const auto locks_before = run.locked.size();
    for (int step = 0; step < 4; ++step) run.locks_after += take(game.step(executor)).story_triggers.size();
    expect(run.locked.size() == locks_before, "a consumed flag fires nothing again");
    run.hash = game.state_sha256();
    return run;
}

void lua_bridge(const std::filesystem::path& root) {
    std::string reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const auto run = session(workers, root);
        expect(run.locked == Names{"Unit_One", "Unit_Two", "Unit_Three"},
            "Check_Story_Flag reads a TRIGGER_AI flag; Story_Event after each Set_Reward_Parameter fires one record per unit, in order");
        expect(run.seen == 1 && !run.flag_left, "the flag is consumed by one reset check, once");
        expect(run.wrong == 0, "another player's flag of the same name is not read");
        if (reference.empty()) reference = run.hash;
        expect(run.hash == reference, "the session digest is equal on " + std::to_string(workers) + " workers");
    }
}
} // namespace

int case_galactic_story_ai_bridge_tests(int argc, char** argv) {
    try {
        trigger_ai();
        ai_notification();
        call_time_parameters();
        lua_bridge(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-ai-bridge"));
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

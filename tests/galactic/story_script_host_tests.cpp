#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The galactic story-script host (WST-20 (3), WST-53..55): the plot engine's per-event Lua callback request, the
// script content bound to sub-plots, the instances' start, the forwarded Story_Event_Trigger calls, the named
// refusals and the save round trip. Synthetic module names and bodies only.
namespace {
namespace st = eawr::sim::galactic::story;
namespace auth = eawr::script::authoritative;
namespace cpn = eawr::campaign;
using eawr::sim::math::Fixed;
using eawr::testing::take;
using galactic_test::expect;

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

st::SubPlotDef sub(std::string name, std::string script, std::vector<st::EventDef> events) {
    st::SubPlotDef out;
    out.name = std::move(name); out.script = std::move(script); out.owner = 1; out.events = std::move(events);
    return out;
}
st::EventDef start_event(std::string authored) {
    st::EventDef e;
    e.name = st::upper(authored); e.authored_name = std::move(authored); e.type = "STORY_ELAPSED"; e.params[0] = {"0"};
    return e;
}

void engine_requests_callbacks() {
    std::vector<st::SubPlotDef> subs{
        sub("Scripted.xml", "Probe_Script", {start_event("Mixed_Case_Start")}),
        sub("Plain.xml", "", {start_event("Plain_Start")}),
    };
    auto engine = take(st::Engine::create({}, {{1, "plot.xml"}}, subs));
    take(engine.service(Fixed::from_raw(0), false));
    const auto calls = engine.take_script_calls();
    expect(calls.size() == 1 && calls[0].subplot == 0 && calls[0].event == "Mixed_Case_Start",
        "WST-20 (3)/55 a scripted sub-plot's triggered event requests Story_Event_Trigger in its authored spelling; an unscripted one asks nothing");
    expect(engine.take_script_calls().empty(), "the requests are taken once");
    expect(engine.triggered("Scripted.xml", "MIXED_CASE_START") && engine.triggered("Plain.xml", "PLAIN_START"), "the events trigger as before");
    auto again = take(st::Engine::create({}, {{1, "plot.xml"}}, subs));
    take(again.service(Fixed::from_raw(0), false));
    auto saved = again.state();
    auto restored = take(st::Engine::create({}, {{1, "plot.xml"}}, subs));
    take(restored.restore(saved));
    expect(restored.take_script_calls().empty(), "a restored engine owes no callbacks");
}

constexpr std::string_view library = R"LUA(
Hits = 0
Defined = 0
Last = "none"
function Base_Definitions() Defined = Defined + 1 end
function Story_Event_Trigger(name) Last = name; Hits = Hits + 1 end
function main()
    while true do coroutine.yield(true) end
end
)LUA";

void host(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    const auto data = root / "loose";
    write(data / "scripts" / "library" / "storyprobe.lua", std::string(library));
    write(data / "scripts" / "story" / "Probe_Script.lua", "require(\"StoryProbe\")\n");
    write(data / "scripts" / "story" / "Other_Script.lua", "require(\"StoryProbe\")\n");
    write(data / "scripts" / "story" / "Free_Script.lua", "require(\"StoryProbe\")\nfunction main() GameRandom.Free_Random(1, 2) end\n");
    write(data / "scripts" / "story" / "Dist_Script.lua", "require(\"StoryProbe\")\nfunction main() DiscreteDistribution.Create().Insert(1, 1) end\n");
    write(data / "scripts" / "story" / "Fault_Script.lua", "require(\"StoryProbe\")\nfunction main() Find_First_Object(\"Empire\") end\n");
    write(data / "xml" / "unrelated.xml", "<Unrelated/>");
    eawr::vfs::MountSpec mount; mount.layer_id = "story-host"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));

    const std::vector<st::SubPlotDef> subs{
        sub("A.xml", "Probe_Script", {}), sub("B.xml", "", {}), sub("C.xml", "Missing_Script", {}),
        sub("D.xml", "Other_Script", {}), sub("E.xml", "Free_Script", {}), sub("F.xml", "Dist_Script", {}), sub("G.xml", "Fault_Script", {})};
    const auto none = take(cpn::GalacticStoryScripts::load(files, std::vector<st::SubPlotDef>{subs[1]}));
    expect(none->entries().empty() && none->modules().empty(), "plots naming no script pin nothing");

    const auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    expect(content->entries().size() == 6, "every scripted sub-plot has an entry, the unscripted one has none");
    const auto* a = content->find("A.xml");
    const auto* c = content->find("C.xml");
    expect(a && a->instance == cpn::first_story_script_instance + 1 && !a->module.empty(), "an entry is bound to its Story module and the sub-plot's table position");
    expect(c && c->module.empty() && content->notes().size() == 1 && content->notes()[0].find("Missing_Script") != std::string::npos,
        "a script that names no Story module runs no Lua and is listed in the notes");
    expect(content->modules().size() == 6 && !content->modules().contains("data/xml/unrelated.xml"), "only Library and Story modules are pinned");
    expect(content->fingerprint().size() == 64 && content->fingerprint() == take(cpn::GalacticStoryScripts::load(files, subs))->fingerprint(),
        "the content identity is stable");

    auth::SessionConfig config;
    config.seed = 77; config.galactic_clock = true; config.tick_duration = {1, 30};
    const auto make = [&] {
        auto scripts = take(cpn::create_galactic_story_scheduler(*content, config));
        std::vector<std::string> notes;
        take(cpn::start_galactic_story_scripts(scripts, *content, notes));
        expect(notes.empty(), "every Story module of the mounted data starts");
        return scripts;
    };
    auto scripts = make();
    const eawr::sim::InlineExecutor executor;
    auth::ServiceOptions options; options.time_tick = 1;
    auto first = take(scripts.service(executor, options));
    const auto number = [&](std::uint64_t instance, std::string_view name) {
        const auto value = take(scripts.read_global(instance, name));
        const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
        return n ? n->repr : ~std::uint64_t{};
    };
    expect(number(a->instance, "Defined") == eawr::script::numeric::LuaNumber(1).repr, "Base_Definitions runs once before the first pump");
    const auto codes_of = [&](const auth::ServiceReport& report, std::string_view code) {
        std::size_t n{};
        for (const auto& row : report.diagnostics) if (row.code == code) ++n;
        return n;
    };
    bool only_find_player = true;
    for (const auto& row : first.diagnostics)
        if (row.code == auth::codes::script_error && row.message.find("Find_First_Object") == std::string::npos) only_find_player = false;
    expect(codes_of(first, auth::codes::forbidden_capability) == 0 && only_find_player,
        "GameRandom.Free_Random and DiscreteDistribution.Create run (RGC-190..199); only the unbound Find_First_Object ends a script");
    bool named_fault = false;
    for (const auto& row : first.diagnostics)
        named_fault = named_fault || (row.message.find(std::string(auth::codes::missing_api)) != std::string::npos && row.message.find("Find_First_Object") != std::string::npos);
    expect(named_fault, "an unbound engine function faults with its own name");

    // The calls: the scripted sub-plot is called; the unscripted, the unbound and the unknown ones are dropped.
    std::uint64_t sequence = 0;
    const std::vector<st::ScriptCall> calls{{0, "Probe_Event"}, {1, "Ignored"}, {2, "Ignored"}, {0, "Second_Event"}};
    take(cpn::forward_galactic_story_calls(scripts, *content, subs, calls, sequence));
    expect(sequence == 2, "only calls for running scripts take an event sequence");
    options.time_tick = 2;
    const auto second = take(scripts.service(executor, options));
    expect(second.diagnostics.empty() || codes_of(second, auth::codes::script_error) == 0, "the calls raise no script error");
    expect(number(a->instance, "Hits") == eawr::script::numeric::LuaNumber(2).repr, "each forwarded call reaches Story_Event_Trigger once, in order");
    const auto last = take(scripts.read_global(a->instance, "Last"));
    const auto* text = last ? std::get_if<std::string>(&last->data) : nullptr;
    expect(text && *text == "Second_Event", "the callback receives the authored event name");
    expect(number(content->find("D.xml")->instance, "Hits") == eawr::script::numeric::LuaNumber(0).repr, "another sub-plot's script is not called");
    const std::vector<st::ScriptCall> outside{{99, "X"}};
    expect(!cpn::forward_galactic_story_calls(scripts, *content, subs, outside, sequence), "a call outside the plot table is refused");

    // Persistence: the saved scripts restore into a fresh scheduler of the same content with the same state.
    const auto saved = take(scripts.save());
    auto reloaded = take(cpn::create_galactic_story_scheduler(*content, config));
    take(reloaded.load(saved));
    expect(take(reloaded.state_hash()) == take(scripts.state_hash()), "a restored story scheduler has the saved state");
    options.time_tick = 3; take(scripts.service(executor, options));
    take(reloaded.service(executor, options));
    expect(take(reloaded.state_hash()) == take(scripts.state_hash()), "the restored scheduler services like the original");

    // Worker equality: the same start gives the same state on 1, 2, 4 and 8 workers.
    std::string reference;
    for (const std::size_t workers : std::vector<std::size_t>{1, 2, 4, 8}) {
        auto one = make();
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        auth::ServiceOptions step; step.time_tick = 1;
        take(one.service(threaded, step));
        std::uint64_t counter = 0;
        take(cpn::forward_galactic_story_calls(one, *content, subs, calls, counter));
        step.time_tick = 2;
        take(one.service(threaded, step));
        const auto hash = take(one.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "story script state is equal on " + std::to_string(workers) + " workers");
    }
}
// One sub-plot whose script body is `body`: the scheduler of the story host after its first service, the report of that
// service and the instance. Every call of one test builds the same sub-plot position, so the instance is the same and two
// runs differ only in the script (RGC-192..196 compare stream positions this way).
struct Scripted {
    std::shared_ptr<const cpn::GalacticStoryScripts> content;
    std::vector<st::SubPlotDef> subs;
    auth::ScriptScheduler scripts;
    auth::ServiceReport first;
    std::uint64_t instance{};
};

auth::SessionConfig story_config(std::uint64_t seed) {
    auth::SessionConfig config;
    config.seed = seed; config.galactic_clock = true; config.tick_duration = {1, 30};
    return config;
}

Scripted run_scripted(const std::filesystem::path& root, const std::string& name, const std::string& body, bool multiplayer = false,
    std::uint64_t seed = 4242, const eawr::sim::PartitionExecutor& executor = eawr::sim::InlineExecutor{},
    const cpn::GalacticStoryPlayers& players = {}) {
    const auto data = root / name / "loose";
    std::filesystem::remove_all(root / name);
    write(data / "scripts" / "story" / "Rand_Script.lua", body);
    eawr::vfs::MountSpec mount; mount.layer_id = "story-random-" + name; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    auto subs = std::vector<st::SubPlotDef>{sub("R.xml", "Rand_Script", {})};
    auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    auto scripts = take(cpn::create_galactic_story_scheduler(*content, story_config(seed), multiplayer, players));
    std::vector<std::string> notes;
    take(cpn::start_galactic_story_scripts(scripts, *content, notes));
    expect(notes.empty(), "the random-test script starts");
    auth::ServiceOptions options; options.time_tick = 1;
    auto report = take(scripts.service(executor, options));
    const auto instance = content->find("R.xml")->instance;
    return Scripted{std::move(content), std::move(subs), std::move(scripts), std::move(report), instance};
}

std::optional<eawr::script::numeric::LuaNumber> number_global(Scripted& run, std::string_view name) {
    const auto value = take(run.scripts.read_global(run.instance, name));
    const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
    return n ? std::optional(*n) : std::nullopt;
}
bool flag_global(Scripted& run, std::string_view name) {
    const auto value = take(run.scripts.read_global(run.instance, name));
    const auto* b = value ? std::get_if<bool>(&value->data) : nullptr;
    return b && *b;
}
std::int64_t integer_global(Scripted& run, std::string_view name) {
    const auto n = number_global(run, name);
    return n ? static_cast<std::int64_t>(static_cast<std::int32_t>(*n)) : -1;
}
std::size_t count_code(const auth::ServiceReport& report, std::string_view code) {
    std::size_t n{};
    for (const auto& row : report.diagnostics) if (row.code == code) ++n;
    return n;
}

void pending_call_checkpoint(const std::filesystem::path& root) {
    namespace cp = eawr::checkpoint;
    namespace gc = eawr::sim::galactic;
    const auto data = root / "pending-call" / "loose";
    write(data / "scripts" / "story" / "Checkpoint_Probe.lua", std::string(library));
    eawr::vfs::MountSpec mount; mount.layer_id = "pending-call"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    auto repeat = start_event("Repeat"); repeat.perpetual = true;
    const std::vector<st::SubPlotDef> subs{sub("Pending.xml", "Checkpoint_Probe", {repeat})};
    const auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    gc::SyntheticGalaxyOptions shape; shape.players = 2; shape.planets = 4;
    const auto setup = gc::synthetic_galaxy(shape);
    const auto ids = cp::game_identities(setup);
    const auto config = story_config(setup.seed);
    const auto schedulers = [&]() {
        return cp::GameLoadOptions::GalacticSchedulers{
            take(auth::ScriptScheduler::create(config, {})), take(cpn::create_galactic_story_scheduler(*content, config))};
    };
    const auto configure = [&](cpn::GameSession& game) {
        game.bind_story_scripts(content);
        return game.install_story({}, {{1, "plot.xml"}}, subs);
    };
    const auto hits = [&](cpn::GameSession& game) {
        const auto value = take(game.galactic_scripts()->story_scheduler().read_global(content->find("Pending.xml")->instance, "Hits"));
        const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
        return n ? static_cast<std::int64_t>(static_cast<std::int32_t>(*n)) : -1;
    };
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(cpn::GameSession::create(setup));
        take(configure(game));
        auto fresh = schedulers();
        std::vector<std::string> notes;
        take(cpn::start_galactic_story_scripts(fresh.second, *content, notes));
        take(game.install_galactic_scripts(std::move(fresh.first), std::move(fresh.second)));
        take(game.step(executor));
        expect(hits(game) == 0, "the first plot callback is pending until the next script service");
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        cp::GameLoadOptions options;
        options.configure_session = configure;
        options.galactic_scripts = [&]() { return eawr::core::Result<cp::GameLoadOptions::GalacticSchedulers>::success(schedulers()); };
        auto restored = take(cp::load_game_checkpoint(saved, ids, options, executor));
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved,
            "the pending call and its owner sequence round-trip byte for byte");
        galactic_test::mark_saved(game);
        for (int tick = 0; tick < 3; ++tick) {
            take(game.step(executor)); take(restored.session.step(executor));
            expect(hits(game) == tick + 1 && hits(restored.session) == hits(game), "pending and subsequent callbacks execute once after restore");
            expect(game.state_sha256() == restored.session.state_sha256(), "restored callback event keys preserve the session digest");
        }
        const auto hash = take(game.galactic_scripts()->story_scheduler().state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "pending story calls restore equally on every worker count");
    }
}

// RGC-192: a sample consumes exactly the same 15-bit step as no-argument GameRandom.
void distribution_draw_bits(const std::filesystem::path& root) {
    auto draws = run_scripted(root, "draw_words", R"LUA(
function main()
    Picks = ""
    for i = 1, 128 do
        local word = GameRandom()
        if word <= 16383 then Picks = Picks .. "a" else Picks = Picks .. "b" end
    end
    Next = GameRandom()
end
)LUA");
    auto samples = run_scripted(root, "draw_samples", R"LUA(
function main()
    local list = DiscreteDistribution.Create()
    list.Insert("a", 1) list.Insert("b", 1)
    Picks = ""
    for i = 1, 128 do Picks = Picks .. list.Sample() end
    Next = GameRandom()
end
)LUA");
    const auto expected = take(draws.scripts.read_global(draws.instance, "Picks"));
    const auto actual = take(samples.scripts.read_global(samples.instance, "Picks"));
    const auto* expected_text = expected ? std::get_if<std::string>(&expected->data) : nullptr;
    const auto* actual_text = actual ? std::get_if<std::string>(&actual->data) : nullptr;
    expect(expected_text && actual_text && *expected_text == *actual_text,
        "RGC-192 equal-weight samples use the same draw bits as GameRandom");
    expect(integer_global(draws, "Next") == integer_global(samples, "Next"), "each non-empty sample consumes exactly one game word");
}

// RGC-190..194: the weighted list. Synthetic values only.
void distribution_contract(const std::filesystem::path& root) {
    auto run = run_scripted(root, "dist", R"LUA(
function main()
    local a = DiscreteDistribution.Create(1, 2, 3)
    local b = DiscreteDistribution.Create()
    a.Insert("x", 1)
    Independent = (b.Sample() == nil) and (a.Sample() == "x")

    local c = DiscreteDistribution.Create()
    local o = {}
    local ok1 = pcall(c.Insert, o)
    local ok2 = pcall(c.Insert, o, "w")
    local ok3 = pcall(c.Insert, o, 1, 2)
    Bad_Calls_Rejected = (not ok1) and (not ok2) and (not ok3) and (c.Sample() == nil)

    local d = DiscreteDistribution.Create()
    local keep = {}
    d.Insert({}, 0)
    d.Insert({}, -3)
    d.Insert({}, 0 / 0)
    d.Insert(keep, 2)
    Only_Valid_Stored = true
    for i = 1, 20 do
        if d.Sample() ~= keep then Only_Valid_Stored = false end
    end

    local e = DiscreteDistribution.Create()
    e.Insert("a", 1)
    e.Insert("b", 3)
    A_Count = 0
    for i = 1, 400 do
        if e.Sample() == "a" then A_Count = A_Count + 1 end
    end

    local f = DiscreteDistribution.Create()
    f.Insert("t", 1)
    f.Insert("t", 1)
    f.Insert("u", 2)
    T_Count = 0
    for i = 1, 400 do
        if f.Sample() == "t" then T_Count = T_Count + 1 end
    end
    Done = true
end
)LUA");
    expect(count_code(run.first, auth::codes::script_error) == 0 && count_code(run.first, auth::codes::missing_api) == 0, "the distribution script runs without a script fault");
    expect(flag_global(run, "Done"), "the distribution script ran to its end");
    expect(flag_global(run, "Independent"), "RGC-190 two lists share nothing; RGC-193 an empty list samples nothing");
    expect(flag_global(run, "Bad_Calls_Rejected"), "RGC-191 Insert with one, a non-number weight or three arguments is an error and stores nothing");
    expect(flag_global(run, "Only_Valid_Stored"), "RGC-191/192 zero, negative and NaN weights store nothing; Sample returns the very object inserted");
    const auto a_count = integer_global(run, "A_Count");
    expect(a_count > 50 && a_count < 150, "RGC-192 weights 1 and 3 pick the first about a quarter of the time (" + std::to_string(a_count) + " of 400)");
    const auto t_count = integer_global(run, "T_Count");
    expect(t_count > 130 && t_count < 270, "RGC-191 equal values inserted twice are two entries whose weights add, half the total (" + std::to_string(t_count) + " of 400)");
}

// NEW-L22: the debug sinks have no effect and fault nothing.
void debug_sinks(const std::filesystem::path& root) {
    auto run = run_scripted(root, "sinks", "function main()\n_ScriptMessage(\"a\")\n_OuputDebug(\"b\")\n_CustomScriptMessage(\"f\", \"c\")\n_MessagePopup(\"d\")\nDumpCallStack()\nObject = nil\nTarget = nil\nBudget = nil\nFreeStore = nil\nPlayerObject = nil\nDone = true\nend\n");
    expect(flag_global(run, "Done") && count_code(run.first, auth::codes::script_error) == 0 && count_code(run.first, auth::codes::missing_api) == 0,
        "the debug sinks run without effect or fault");
}

// RGC-192..196: which stream every call draws from, told by where the game stream stands after it.
void random_streams(const std::filesystem::path& root) {
    const auto probe = [&](const std::string& name, const std::string& before, bool multiplayer = false) {
        auto run = run_scripted(root, name, "function main()\n" + before + "\nProbe = GameRandom(1, 30000)\nSeven = GameRandom.Free_Random(7, 7)\nend\n", multiplayer);
        expect(count_code(run.first, auth::codes::script_error) == 0, name + " runs without a script error");
        return std::pair(integer_global(run, "Probe"), integer_global(run, "Seven"));
    };
    const auto base = probe("base", "");
    expect(base.first >= 1 && base.first <= 30000, "GameRandom stays inside its range");
    expect(base.second == 7, "RGC-195 equal bounds return the bound");
    expect(probe("again", "").first == base.first, "the same start draws the same numbers");
    expect(probe("empty", "DiscreteDistribution.Create().Sample()").first == base.first, "RGC-193 sampling an empty list draws nothing");
    expect(probe("one", "local l = DiscreteDistribution.Create() l.Insert(1, 5) l.Sample()").first != base.first,
        "RGC-192/194 a sample of a one-entry list still takes a draw of the game stream");
    expect(probe("free", "GameRandom.Free_Random(500, 750)").first == base.first,
        "RGC-195 the free stream is separate: a free draw leaves the game stream where it was");
    expect(probe("free_none", "GameRandom.Free_Random()").first != base.first, "RGC-195 the no-argument form draws from the game stream");

    auto range = run_scripted(root, "range", R"LUA(
function main()
    Low = 1000000
    High = -1
    for i = 1, 60 do
        local v = GameRandom.Free_Random(500, 750)
        local w = GameRandom.Free_Random(750, 500)
        if v < Low then Low = v end
        if w < Low then Low = w end
        if v > High then High = v end
        if w > High then High = w end
    end
    Whole = GameRandom.Free_Random()
    Bad_First = GameRandom.Free_Random("x", 9)
    Bad_Second = GameRandom.Free_Random(4, "y")
end
)LUA");
    expect(integer_global(range, "Low") >= 500 && integer_global(range, "High") <= 750 && integer_global(range, "High") > integer_global(range, "Low") + 50,
        "RGC-195 Free_Random(a, b) covers a..b in either order");
    expect(integer_global(range, "Whole") >= 0 && integer_global(range, "Whole") <= 32767, "RGC-195 the no-argument draw is 15 bits");
    expect(integer_global(range, "Bad_First") >= 0 && integer_global(range, "Bad_First") <= 32767, "a non-number first argument still draws over the default range");
    expect(integer_global(range, "Bad_Second") >= 0 && integer_global(range, "Bad_Second") <= 4, "a non-number second argument still draws over 0..first");
    expect(count_code(range.first, auth::codes::invalid_request) == 2, "RGC-195 each bad argument is reported once");
    expect(count_code(range.first, auth::codes::script_warning) == 0, "single-player Free_Random raises no warning");

    // RGC-196: in a multiplayer game Free_Random warns and is the plain GameRandom call on the game stream.
    auto sp = run_scripted(root, "sp_draw", "function main() Value = GameRandom(500, 750) end\n");
    auto mp = run_scripted(root, "mp_draw", "function main() Value = GameRandom.Free_Random(500, 750) end\n", true);
    auto solo = run_scripted(root, "solo_draw", "function main() Value = GameRandom.Free_Random(500, 750) end\n", false);
    expect(integer_global(mp, "Value") == integer_global(sp, "Value"), "RGC-196 multiplayer Free_Random is GameRandom on the game stream");
    expect(count_code(mp.first, auth::codes::script_warning) == 1, "RGC-196 multiplayer Free_Random raises one warning");
    expect(count_code(solo.first, auth::codes::script_warning) == 0, "single-player Free_Random does not");
    const auto seeded = [&](std::uint64_t seed) {
        auto run = run_scripted(root, "seeded", "function main() Free = GameRandom.Free_Random(1, 30000) Game = GameRandom(1, 30000) end\n", false, seed);
        return std::pair(integer_global(run, "Free"), integer_global(run, "Game"));
    };
    expect(seeded(1).first == seeded(2).first && seeded(1).second != seeded(2).second,
        "RGC-197 the free stream has a fixed seed: it does not follow the game seed, the game stream does");
}

// RGC-190, #4434: a list lives in the script's state, so it is saved, restored, and equal on every worker count.
void distribution_state(const std::filesystem::path& root) {
    const std::string body = R"LUA(
function main()
    List = DiscreteDistribution.Create()
    List.Insert("a", 1)
    List.Insert("b", 2)
    List.Insert("c", 4)
    Taken = ""
    Free = GameRandom.Free_Random(1, 30000)
end
function Story_Event_Trigger(name)
    for i = 1, 30 do Taken = Taken .. List.Sample() end
    Free = GameRandom.Free_Random(1, 30000)
end
)LUA";
    const auto step = [&](Scripted& run, const eawr::sim::PartitionExecutor& executor, std::uint64_t tick, std::uint64_t& sequence) {
        const std::vector<st::ScriptCall> calls{{0, "Pick"}};
        take(cpn::forward_galactic_story_calls(run.scripts, *run.content, run.subs, calls, sequence));
        auth::ServiceOptions options; options.time_tick = tick;
        take(run.scripts.service(executor, options));
    };
    const auto taken = [&](Scripted& run) {
        const auto value = take(run.scripts.read_global(run.instance, "Taken"));
        const auto* text = value ? std::get_if<std::string>(&value->data) : nullptr;
        return text ? *text : std::string("<none>");
    };
    auto original = run_scripted(root, "state", body);
    std::uint64_t sequence = 0;
    const auto saved = take(original.scripts.save());
    auto restored = run_scripted(root, "state_restored", body);
    take(restored.scripts.load(saved));
    expect(take(restored.scripts.state_hash()) == take(original.scripts.state_hash()), "a list and its closures restore into the saved state");
    std::uint64_t restored_sequence = 0;
    const eawr::sim::InlineExecutor inline_executor;
    step(original, inline_executor, 2, sequence);
    step(restored, inline_executor, 2, restored_sequence);
    expect(taken(original).size() == 30 && taken(original) == taken(restored), "a restored list samples the same entries the original does");
    expect(take(restored.scripts.state_hash()) == take(original.scripts.state_hash()), "the restored scheduler keeps the original's state, free stream included");
    const auto after_save = take(original.scripts.save());
    auto again = run_scripted(root, "state_again", body);
    take(again.scripts.load(after_save));
    expect(take(again.scripts.state_hash()) == take(original.scripts.state_hash()), "the state after the draws restores too");

    std::string reference_hash, reference_taken;
    for (const std::size_t workers : std::vector<std::size_t>{1, 2, 4, 8}) {
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        auto run = run_scripted(root, "state_workers", body, false, 4242, threaded);
        std::uint64_t counter = 0;
        step(run, threaded, 2, counter);
        const auto hash = take(run.scripts.state_hash());
        if (reference_hash.empty()) { reference_hash = hash; reference_taken = taken(run); }
        expect(hash == reference_hash && taken(run) == reference_taken, "the sampled entries and the state are equal on " + std::to_string(workers) + " workers");
    }
}

// WST-90..94: full faction lookup, registered-player order, local alias and per-script wrapper identity.
void player_lookup(const std::filesystem::path& root) {
    // Synthetic names, including a loaded faction with no player; deliberately unsorted player IDs.
    const cpn::GalacticStoryPlayers roster{{"Copper", "Indigo", "Unseated", "Minor"},
        {{9, "Copper"}, {7, "Indigo"}, {2, "Copper"}, {11, "Minor"}}, 7};
    const eawr::sim::InlineExecutor inline_executor;
    const auto run = [&](const std::string& name, const std::string& body, const cpn::GalacticStoryPlayers& players) {
        return run_scripted(root, name, "function Count_Values(...) return arg.n end\n" + body, false, 4242, inline_executor, players);
    };
    const std::string body = R"LUA(
function main()
    First = Find_Player("cOpPeR")
    Again = Find_Player("COPPER")
    Local = Find_Player("LoCaL")
    LocalValues = Count_Values(Find_Player("local"))
    Minor = Find_Player("minor")
    Same = rawequal(First, Again)
    NoPlayer = Find_Player("Unseated") == nil
    NoValues = Count_Values(Find_Player("Unseated"))
    Before = GameRandom()
    for i = 1, 32 do Find_Player("Copper") end
    After = GameRandom()
end
function Story_Event_Trigger(name)
    Same = Same and rawequal(First, Find_Player("Copper")) and rawequal(Local, Find_Player("local"))
end
)LUA";
    auto found = run("players", body, roster);
    expect(found.first.diagnostics.empty(), "known faction and local lookups succeed without diagnostics");
    const auto handle_id = [&](Scripted& value, std::string_view name) {
        const auto result = take(value.scripts.read_global(value.instance, name));
        const auto* handle = result ? std::get_if<auth::Handle>(&result->data) : nullptr;
        return handle ? handle->id : ~std::uint64_t{};
    };
    expect(handle_id(found, "First") == 2, "case-insensitive lookup chooses the first matching player-table ID");
    expect(handle_id(found, "Local") == 7, "local follows the configured observer, independently of script owner 1");
    expect(handle_id(found, "Minor") == 11, "a non-major loaded faction is available");
    expect(flag_global(found, "Same"), "repeated lookups reuse the wrapper identity within the script");
    auto multiplayer = run_scripted(root, "players_multiplayer", "function Count_Values(...) return arg.n end\n" + body,
        true, 4242, inline_executor, roster);
    expect(multiplayer.first.diagnostics.empty() && integer_global(multiplayer, "LocalValues") == 0 && handle_id(multiplayer, "First") == 2,
        "multiplayer deliberately has no per-peer local identity, even if the caller supplies a local player");
    expect(flag_global(found, "NoPlayer") && integer_global(found, "NoValues") == 0,
        "a known faction without a registered player returns no values, without an error");
    auto control = run("player_draw_control", "function main() Before = GameRandom() After = GameRandom() end", roster);
    expect(number_global(found, "Before") == number_global(control, "Before") && number_global(found, "After") == number_global(control, "After"),
        "player lookup consumes no random draws");
    auto without_local = roster;
    without_local.local_player.reset();
    auto no_local = run("no_local", "function main() Missing = Count_Values(Find_Player('local')) end", without_local);
    expect(no_local.first.diagnostics.empty() && integer_global(no_local, "Missing") == 0, "headless local has no fallback to a human or script owner");
    auto multiplayer_absent = run_scripted(root, "players_multiplayer_absent", "function Count_Values(...) return arg.n end\n" + body,
        true, 4242, inline_executor, without_local);
    expect(multiplayer_absent.first.diagnostics.empty() && integer_global(multiplayer_absent, "LocalValues") == 0,
        "multiplayer's absent canonical local player returns no values without diagnostics");
    without_local.local_player = 99;
    auto invalid_local = run("invalid_local", "function main() Missing = Count_Values(Find_Player('LOCAL')) end", without_local);
    expect(invalid_local.first.diagnostics.empty() && integer_global(invalid_local, "Missing") == 0, "an absent configured local player returns no wrapper");

    const std::vector<std::string> invalid{
        "return Find_Player()", "return Find_Player('Copper', 'Indigo')", "return Find_Player('Copper', nil)",
        "return Find_Player(1)", "return Find_Player(nil)", "return Find_Player(false)", "return Find_Player(function() end)",
        "local t = {} t.self = t return Find_Player(t)", "return Find_Player('Unknown')", "return Find_Player(' Copper')", "return Find_Player('')"};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        auto failed = run("invalid_player_" + std::to_string(i),
            "function main() local ok, n = pcall(function() return Count_Values((function() " + invalid[i] +
            " end)()) end) if not ok or n ~= 0 then error('Expected zero return values') end end", roster);
        bool named = false;
        for (const auto& diagnostic : failed.first.diagnostics)
            named |= diagnostic.message.find("Find_Player") != std::string::npos;
        expect(named && failed.scripts.instances().empty() && count_code(failed.first, auth::codes::invalid_request) == 1 &&
            count_code(failed.first, auth::codes::script_error) == 0,
            "invalid player lookup returns no values and requests script exit even through pcall, case " + std::to_string(i));
    }

    const auto saved = take(found.scripts.save());
    std::string reference;
    for (const std::size_t workers : std::vector<std::size_t>{1, 2, 4, 8}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto restored = run("players_restore", body, roster);
        take(restored.scripts.load(saved));
        const auto before = take(restored.scripts.state_hash());
        expect(before == take(found.scripts.state_hash()), "saved player wrappers restore with their script graph");
        std::uint64_t sequence = 0;
        const std::vector<st::ScriptCall> calls{{0, "Again"}};
        take(cpn::forward_galactic_story_calls(restored.scripts, *restored.content, restored.subs, calls, sequence));
        auth::ServiceOptions options; options.time_tick = 2;
        const auto report = take(restored.scripts.service(executor, options));
        expect(report.diagnostics.empty() && flag_global(restored, "Same"), "restored wrappers compare equal to fresh lookups");
        const auto hash = take(restored.scripts.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "player lookup continuation is equal on " + std::to_string(workers) + " workers");
    }
}
} // namespace

int case_galactic_story_script_host_tests(int argc, char** argv) {
    try {
        engine_requests_callbacks();
        const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-script-host");
        host(root);
        pending_call_checkpoint(root);
        distribution_draw_bits(root / "random");
        distribution_contract(root / "random");
        debug_sinks(root / "random");
        random_streams(root / "random");
        distribution_state(root / "random");
        player_lookup(root / "players");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

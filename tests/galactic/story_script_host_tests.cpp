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
function Fault_Callback() error('injected callback failure') end
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
    write(data / "scripts" / "story" / "Fault_Script.lua", "require(\"StoryProbe\")\nfunction main() Find_Hint(\"Empire\") end\n");
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
        if (row.code == auth::codes::script_error && row.message.find("Find_Hint") == std::string::npos) only_find_player = false;
    expect(codes_of(first, auth::codes::forbidden_capability) == 0 && only_find_player,
        "GameRandom.Free_Random and DiscreteDistribution.Create run (RGC-190..199); only the unbound Find_Hint ends a script");
    bool named_fault = false;
    for (const auto& row : first.diagnostics)
        named_fault = named_fault || (row.message.find(std::string(auth::codes::missing_api)) != std::string::npos && row.message.find("Find_Hint") != std::string::npos);
    expect(named_fault, "an unbound engine function faults with its own name");
    const auto retired = content->find("G.xml")->instance;
    expect(scripts.retired_instances() == std::vector<std::uint64_t>{retired}
        && first.load_notes.size() == 1 && first.load_notes[0].code == "EAWR-LOAD-1400"
        && first.load_notes[0].object == std::to_string(retired), "the unbound story provider retires with one runtime LoadNote");

    // The calls: the scripted sub-plot is called; the unscripted, the unbound and the unknown ones are dropped.
    std::uint64_t sequence = 0;
    const std::vector<st::ScriptCall> calls{{0, "Probe_Event"}, {1, "Ignored"}, {2, "Ignored"}, {6, "Ignored_Retired"}, {0, "Second_Event"}};
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
    expect(reloaded.retired_instances() == std::vector<std::uint64_t>{retired}
        && !reloaded.create_instance(retired, content->find("G.xml")->module), "story restore retains retirement and refuses restart");
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
    auto callback = make();
    options.time_tick = 1; take(callback.service(executor, options));
    auth::ScriptEvent fault;
    fault.key = {2, auth::first_simulation_producer, a->instance, 0};
    fault.target = a->instance; fault.kind = auth::ScriptEvent::Kind::call; fault.name = "Fault_Callback";
    take(callback.submit_event(std::move(fault)));
    options.time_tick = 2;
    const auto faulted = take(callback.service(executor, options));
    const auto survivors = callback.instances();
    expect(faulted.load_notes.size() == 1 && faulted.load_notes[0].object == std::to_string(a->instance)
        && !std::binary_search(survivors.begin(), survivors.end(), a->instance)
        && std::binary_search(survivors.begin(), survivors.end(), content->find("D.xml")->instance),
        "uncaught story callback failure retires only its provider");
    std::uint64_t after_fault = 0;
    take(cpn::forward_galactic_story_calls(callback, *content, subs, calls, after_fault));
    expect(after_fault == 0, "later story requests for retired providers are dropped");
    options.time_tick = 3;
    expect(take(callback.service(executor, options)).load_notes.empty(), "story service continues without repeated retirement notes");
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
    const cpn::GalacticStoryPlayers& players = {}, auth::GalacticSnapshotReader reader = {}) {
    const auto data = root / name / "loose";
    std::filesystem::remove_all(root / name);
    write(data / "scripts" / "story" / "Rand_Script.lua", body);
    eawr::vfs::MountSpec mount; mount.layer_id = "story-random-" + name; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    auto subs = std::vector<st::SubPlotDef>{sub("R.xml", "Rand_Script", {})};
    auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    auto scripts = take(cpn::create_galactic_story_scheduler(*content, story_config(seed), multiplayer, players, std::move(reader)));
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
    expect(range.scripts.retired_instances().empty() && range.first.load_notes.empty(),
        "RGC-195 argument fallback diagnostics do not retire the running story provider");
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
        {{9, "Copper"}, {7, "Indigo"}, {2, "Copper"}, {11, "Minor"}}, 7, {}};
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
    // Co-op deviation: a shared local player is exposed in multiplayer, and only that one; the per-peer one stays hidden.
    auto shared = roster;
    shared.shared_local_player = shared.local_player;
    shared.local_player = 99;
    auto shared_run = run_scripted(root, "players_shared_local", "function Count_Values(...) return arg.n end\n" + body,
        true, 4242, inline_executor, shared);
    expect(shared_run.first.diagnostics.empty() && integer_global(shared_run, "LocalValues") == 1 && handle_id(shared_run, "Local") == 7,
        "co-op: multiplayer Find_Player('local') returns the shared faction, never the per-peer local player");
    auto shared_unregistered = roster;
    shared_unregistered.shared_local_player = 98;
    auto shared_missing = run_scripted(root, "players_shared_unregistered", "function Count_Values(...) return arg.n end\n" + body,
        true, 4242, inline_executor, shared_unregistered);
    expect(shared_missing.first.diagnostics.empty() && integer_global(shared_missing, "LocalValues") == 0,
        "an unregistered shared local player returns no values");
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
void live_object_and_difficulty(const std::filesystem::path& root) {
    namespace gc = eawr::sim::galactic;
    gc::SyntheticGalaxyOptions shape; shape.players = 2; shape.planets = 4;
    auto world = take(gc::GalacticSession::create(gc::synthetic_galaxy(shape)));
    const auto base = world.snapshot();
    std::shared_ptr<const gc::GalacticSnapshot> published;
    const auto publish = [&](std::vector<gc::CensusObject> actors) {
        gc::NativeSnapshotChanges changes;
        changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::move(actors));
        published = gc::GalacticSnapshot::native_view(base, std::move(changes));
    };
    gc::CensusObject older{}; older.id = 40; older.original_type = 70; older.current_type = 70;
    gc::CensusObject newer = older; newer.id = 41;
    gc::CensusObject dead = older; dead.id = 38; dead.dead = true;
    gc::CensusObject pending = older; pending.id = 39; pending.delete_pending = true;
    publish({newer, dead, pending, older});
    cpn::GalacticStoryPlayers players{};
    players.factions = {"Copper", "Indigo"};
    players.players = {{1, "Copper", "HARD"}, {2, "Indigo", "Normal"}};
    players.types = {{"Hero_Probe", 70}};
    players.planets = {{"Planet_Probe", base->planets().front().id}};
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        publish({newer, dead, pending, older});
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto run = run_scripted(root, "live_" + std::to_string(workers), R"LUA(
function main()
    First = Find_First_Object("hero_probe")
    Same = rawequal(First, Find_First_Object("HERO_PROBE", "ignored"))
    Missing = Find_First_Object("Absent") == nil
    Planet = Find_First_Object("planet_probe")
    Player = Find_Player("Copper")
    Difficulty = Player.Get_Difficulty()
    HumanDifficulty = Find_Player("Indigo").Get_Difficulty()
    coroutine.yield(true)
    Next = Find_First_Object("Hero_Probe")
    DifficultyAfter = Player.Get_Difficulty()
end
)LUA", false, 4242, executor, players, [&] { return published; });
        const auto object_id = [&](auth::ScriptScheduler& scripts, std::string_view name) {
            const auto value = take(scripts.read_global(run.instance, name));
            const auto* object = value ? std::get_if<auth::Handle>(&value->data) : nullptr;
            return object ? object->id : 0;
        };
        expect(run.first.diagnostics.empty() && object_id(run.scripts, "First") == 40,
            "first-object chooses the lowest live matching identity independent of publication order");
        expect(flag_global(run, "Same") && flag_global(run, "Missing")
            && object_id(run.scripts, "Planet") == base->planets().front().id,
            "first-object interns wrappers, returns nil for missing types, and resolves planet names");
        const auto difficulty = take(run.scripts.read_global(run.instance, "Difficulty"));
        const auto human = take(run.scripts.read_global(run.instance, "HumanDifficulty"));
        expect(difficulty && std::get<std::string>(difficulty->data) == "Hard"
            && human && std::get<std::string>(human->data) == "Normal", "player methods expose configured difficulty and the non-AI default");
        const auto saved = take(run.scripts.save());
        auto restored = take(cpn::create_galactic_story_scheduler(*run.content, story_config(4242), false, players, [&] { return published; }));
        take(restored.load(saved));
        newer.id = 42;
        publish({newer});
        auth::ServiceOptions options; options.time_tick = 2;
        const auto next = take(run.scripts.service(executor, options));
        const auto loaded = take(restored.service(executor, options));
        const auto after = take(restored.read_global(run.instance, "DifficultyAfter"));
        expect(next.diagnostics.empty() && loaded.diagnostics.empty() && object_id(run.scripts, "Next") == 42
            && object_id(restored, "Next") == 42 && after && std::get<std::string>(after->data) == "Hard",
            "lookup follows births/removals and restored player handles retain difficulty");
        expect(take(restored.state_hash()) == take(run.scripts.state_hash()), "restored live lookup continuation matches");
        const auto hash = take(run.scripts.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "live object lookup and player difficulty agree across worker counts");
        newer.id = 41;
    }
}
void advisor_hints_stub(const std::filesystem::path& root) {
    cpn::GalacticStoryPlayers players{}; players.factions = {"Copper"}; players.players = {{1, "Copper", "Normal"}};
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto run = run_scripted(root, "hints_" + std::to_string(workers), R"LUA(
function main()
    local player = Find_Player("Copper")
    player.Enable_Advisor_Hints("Space", false)
    player.Enable_Advisor_Hints("Land", false)
    Continued = true
    coroutine.yield(true)
end
)LUA", true, 4242, executor, players);
        expect(flag_global(run, "Continued") && run.scripts.retired_instances().empty()
            && count_code(run.first, auth::codes::unsupported_api) == 2 && run.first.load_notes.empty(),
            "warned advisor presentation stub preserves co-op story script instead of retiring it on a nil method");
        const auto hash = take(run.scripts.state_hash());
        if (workers == 1) reference = hash; else expect(hash == reference, "advisor presentation degradation is equal on 1/2/4/8");
    }
}

void planet_lookup(const std::filesystem::path& root) {
    cpn::GalacticStoryPlayers input{};
    input.planets = {{"Azure", 9}, {"aZure", 7}, {"Amber", 2}};
    auto run = run_scripted(root, "planets", R"LUA(
function main()
    First = FindPlanet("aZuRe")
    Same = rawequal(First, FindPlanet("AZURE", "ignored"))
    Missing = FindPlanet("Absent") == nil
end
)LUA", false, 4242, eawr::sim::InlineExecutor{}, input);
    const auto result = take(run.scripts.read_global(run.instance, "First"));
    const auto* handle = result ? std::get_if<auth::Handle>(&result->data) : nullptr;
    expect(run.first.diagnostics.empty() && handle && handle->id == 7, "named planet resolves to its object handle");
    expect(flag_global(run, "Same") && flag_global(run, "Missing"), "lookup ignores extra arguments, reuses wrappers, and returns nil for unknown names");
    auto invalid = run_scripted(root, "invalid_planet", "function main() FindPlanet(false) end",
        false, 4242, eawr::sim::InlineExecutor{}, input);
    expect(count_code(invalid.first, auth::codes::invalid_request) == 1 && invalid.scripts.instances().empty(),
        "non-string planet lookup diagnoses and ends the script");
}
// RGC-176..178: the three creation bindings against a recording stand-in for the ordered native shadow. The native
// creation itself (identities, journal, free-store hand-off) is covered by the installed-game story case.
struct SpawnLog {
    std::vector<std::string> actions;
    std::uint64_t next{500};
    bool null_creation{};
    bool ai_owner{true};
    bool missing_actor{};
};
struct SpawnState final : auth::ImmediateEffectState {
    std::shared_ptr<SpawnLog> log;
    explicit SpawnState(std::shared_ptr<SpawnLog> shared) : log(std::move(shared)) {}
    std::unique_ptr<auth::ImmediateEffectState> clone() const override { return std::make_unique<SpawnState>(log); }
    eawr::core::Result<auth::ValueList> apply(std::string_view action, const auth::ValueList& args) override {
        std::string row(action);
        for (const auto& value : args)
            if (const auto* handle = std::get_if<auth::Handle>(&value.data)) row += ":" + std::to_string(handle->kind) + "/" + std::to_string(handle->id);
        log->actions.push_back(row);
        if (action == "galactic.spawn_unit") {
            if (log->null_creation) return eawr::core::Result<auth::ValueList>::success({});
            return eawr::core::Result<auth::ValueList>::success({auth::Value{std::vector<auth::Value>{auth::Value{auth::Handle{1, log->next++}}}}});
        }
        if (action == "galactic.story_prevent_ai_usage") {
            if (log->missing_actor) return eawr::core::Result<auth::ValueList>::success({auth::Value{false}, auth::Value{std::string("actor")}});
            if (!log->ai_owner) return eawr::core::Result<auth::ValueList>::success({auth::Value{false}, auth::Value{std::string("pool")}});
            return eawr::core::Result<auth::ValueList>::success({auth::Value{true}});
        }
        return eawr::core::Result<auth::ValueList>::success({});
    }
    eawr::core::Result<auth::ValueList> query(std::string_view, const auth::ValueList&) const override {
        return eawr::core::Result<auth::ValueList>::success({});
    }
};
struct SpawnHost final : auth::ImmediateEffectHost {
    std::shared_ptr<SpawnLog> log;
    explicit SpawnHost(std::shared_ptr<SpawnLog> shared) : log(std::move(shared)) {}
    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
        return eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>>::success(std::make_unique<SpawnState>(log));
    }
    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState>) override { return eawr::core::Result<void>::success(); }
};

cpn::GalacticStoryPlayers spawn_roster() {
    cpn::GalacticStoryPlayers players{};
    players.factions = {"Copper", "Indigo"};
    players.players = {{1, "Copper", "Normal"}, {2, "Indigo", "Normal"}};
    players.types = {{"Hero_Probe", 70}, {"Other_Probe", 71}};
    players.planets = {{"Planet_Probe", 9}};
    return players;
}

Scripted run_spawn(const std::filesystem::path& root, const std::string& name, const std::string& body,
    const std::shared_ptr<SpawnLog>& log, const eawr::sim::PartitionExecutor& executor = eawr::sim::InlineExecutor{}) {
    const auto data = root / name / "loose";
    std::filesystem::remove_all(root / name);
    write(data / "scripts" / "story" / "Rand_Script.lua", body);
    eawr::vfs::MountSpec mount; mount.layer_id = "story-spawn-" + name; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    auto subs = std::vector<st::SubPlotDef>{sub("R.xml", "Rand_Script", {})};
    auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    auto scripts = take(cpn::create_galactic_story_scheduler(*content, story_config(4242), true, spawn_roster()));
    if (log) take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(log), 16));
    std::vector<std::string> notes;
    take(cpn::start_galactic_story_scripts(scripts, *content, notes));
    auth::ServiceOptions options; options.time_tick = 1;
    auto report = take(scripts.service(executor, options));
    const auto instance = content->find("R.xml")->instance;
    return Scripted{std::move(content), std::move(subs), std::move(scripts), std::move(report), instance};
}
std::uint64_t handle_id(Scripted& run, std::string_view name) {
    const auto value = take(run.scripts.read_global(run.instance, name));
    const auto* handle = value ? std::get_if<auth::Handle>(&value->data) : nullptr;
    return handle ? handle->id : 0;
}
std::size_t spawn_count(const SpawnLog& log) {
    return static_cast<std::size_t>(std::count_if(log.actions.begin(), log.actions.end(),
        [](const auto& row) { return row.rfind("galactic.spawn_unit", 0) == 0; }));
}

void spawn_bindings(const std::filesystem::path& root) {
    // Find_Object_Type: case-insensitive original type names; nil for an unknown name; a non-string ends the script.
    auto lookup = run_spawn(root, "find_type", R"LUA(
function main()
    A = Find_Object_Type("hero_probe")
    Same = rawequal(A, Find_Object_Type("HERO_PROBE", "ignored"))
    Missing = Find_Object_Type("Absent") == nil
end
)LUA", nullptr);
    expect(lookup.first.diagnostics.empty() && handle_id(lookup, "A") == 70 && flag_global(lookup, "Same") && flag_global(lookup, "Missing"),
        "Find_Object_Type resolves original type names case-insensitively and returns nil for an unknown one");
    auto bad_name = run_spawn(root, "find_type_bad", "function main() Find_Object_Type(false) end", nullptr);
    expect(count_code(bad_name.first, auth::codes::invalid_request) == 1 && bad_name.scripts.instances().empty(),
        "Find_Object_Type with a non-string diagnoses and ends the script");

    // Spawn_Unit + Prevent_AI_Usage: the native action sees (player, planet, type); the script gets a one-element table.
    const std::string good = R"LUA(
function main()
    local list = Spawn_Unit(Find_Object_Type("Hero_Probe"), FindPlanet("Planet_Probe"), Find_Player("Copper"))
    Count = table.getn(list)
    Unit = list[1]
    Unit.Prevent_AI_Usage(true)
    Done = true
end
)LUA";
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto log = std::make_shared<SpawnLog>();
        auto run = run_spawn(root, "spawn_" + std::to_string(workers), good, log, executor);
        expect(run.first.diagnostics.empty() && flag_global(run, "Done") && integer_global(run, "Count") == 1 && handle_id(run, "Unit") == 500,
            "RGC-178 Spawn_Unit returns a one-element table holding the new object");
        expect(log->actions == (std::vector<std::string>{"galactic.spawn_unit:2/1:1/9:3/70", "galactic.story_prevent_ai_usage:1/500"}),
            "RGC-176 the creation takes the given player, the given planet and the type; Prevent_AI_Usage targets the new object");
        const auto hash = take(run.scripts.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "Spawn_Unit scripts agree across worker counts");
    }

    // A human owner has no AI pool: Prevent_AI_Usage warns and the script goes on.
    auto human_log = std::make_shared<SpawnLog>(); human_log->ai_owner = false;
    auto human = run_spawn(root, "spawn_human", good, human_log);
    expect(flag_global(human, "Done") && count_code(human.first, auth::codes::script_warning) == 1 && human.scripts.instances().size() == 1,
        "Prevent_AI_Usage for an owner without an AI pool is a warning, not a script error");

    // An object the free store does not know is a different warning from a missing pool.
    auto stray_log = std::make_shared<SpawnLog>(); stray_log->missing_actor = true;
    auto stray = run_spawn(root, "spawn_stray_actor", good, stray_log);
    expect(flag_global(stray, "Done") && count_code(stray.first, auth::codes::script_warning) == 1
        && stray.first.diagnostics.front().message.find("free store") != std::string::npos
        && human.first.diagnostics.front().message.find("AI pool") != std::string::npos,
        "Prevent_AI_Usage on an object unknown to the free store warns with its own reason");

    // Hash scope: only content that names Spawn_Unit or Prevent_AI_Usage asks for the ordered native service.
    const auto scope_content = [&](const std::string& name, const std::string& body) {
        const auto data = root / name / "loose";
        std::filesystem::remove_all(root / name);
        write(data / "scripts" / "story" / "Rand_Script.lua", body);
        eawr::vfs::MountSpec mount; mount.layer_id = "story-scope-" + name; mount.data_root = data;
        const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
        const std::vector<st::SubPlotDef> plots{sub("R.xml", "Rand_Script", {})};
        return take(cpn::GalacticStoryScripts::load(files, plots));
    };
    expect(!scope_content("scope_plain", "function main() Plain = FindPlanet('Planet_Probe') end")->uses_immediate_effects()
        && !scope_content("scope_lookalike", "function main() My_Spawn_Unit_Count = 1 end")->uses_immediate_effects(),
        "a story without the creation bindings keeps the partitioned service (negative control)");
    expect(scope_content("scope_spawn", good)->uses_immediate_effects()
        && scope_content("scope_prevent", "function main() local o = Find_First_Object('X') o.Prevent_AI_Usage(true) end")->uses_immediate_effects(),
        "a story naming Spawn_Unit or Prevent_AI_Usage asks for the ordered native service");
    // The instance bound follows the story's own count (no AI-sized bound): three scripted sub-plots bind with a limit of three.
    {
        const auto data = root / "scope_three" / "loose";
        std::filesystem::remove_all(root / "scope_three");
        for (const char* name : {"A_Script", "B_Script", "C_Script"}) write(data / "scripts" / "story" / (std::string(name) + ".lua"), good);
        eawr::vfs::MountSpec mount; mount.layer_id = "story-scope-three"; mount.data_root = data;
        const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
        const std::vector<st::SubPlotDef> plots{sub("A.xml", "A_Script", {}), sub("B.xml", "B_Script", {}), sub("C.xml", "C_Script", {})};
        auto content = take(cpn::GalacticStoryScripts::load(files, plots));
        auto scripts = take(cpn::create_galactic_story_scheduler(*content, story_config(4242), true, spawn_roster()));
        take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(std::make_shared<SpawnLog>()), content->entries().size()));
        std::vector<std::string> notes;
        take(cpn::start_galactic_story_scripts(scripts, *content, notes));
        auth::ServiceOptions options; options.time_tick = 1;
        take(scripts.service(eawr::sim::InlineExecutor{}, options));
        expect(content->entries().size() == 3 && scripts.instances().size() == 3, "all three story instances load and run under the story-sized bound");
    }

    // RGC-176 error paths: nothing is created, the script ends with a named diagnostic.
    const std::vector<std::pair<std::string, std::string>> refusals{
        {"no_type", "Spawn_Unit(Find_Object_Type('Absent'), FindPlanet('Planet_Probe'), Find_Player('Copper'))"},
        {"no_planet", "Spawn_Unit(Find_Object_Type('Hero_Probe'), FindPlanet('Absent'), Find_Player('Copper'))"},
        {"no_player", "Spawn_Unit(Find_Object_Type('Hero_Probe'), FindPlanet('Planet_Probe'), nil)"},
        {"two_arguments", "Spawn_Unit(Find_Object_Type('Hero_Probe'), FindPlanet('Planet_Probe'))"},
        {"swapped", "Spawn_Unit(Find_Object_Type('Hero_Probe'), Find_Player('Copper'), FindPlanet('Planet_Probe'))"},
        {"string_type", "Spawn_Unit('Hero_Probe', FindPlanet('Planet_Probe'), Find_Player('Copper'))"},
    };
    for (const auto& [name, call] : refusals) {
        auto log = std::make_shared<SpawnLog>();
        auto run = run_spawn(root, "refuse_" + name, "function main()\n" + call + "\nReached = true\nend\n", log);
        expect(count_code(run.first, auth::codes::invalid_request) == 1 && spawn_count(*log) == 0 && run.scripts.instances().empty(),
            "RGC-176 " + name + " raises a named script error and creates nothing");
    }
    // The planet must be a planet: a created object is not one.
    auto not_planet_log = std::make_shared<SpawnLog>();
    auto not_planet = run_spawn(root, "refuse_object_planet", R"LUA(
function main()
    local t = Find_Object_Type("Hero_Probe")
    local list = Spawn_Unit(t, FindPlanet("Planet_Probe"), Find_Player("Copper"))
    Spawn_Unit(t, list[1], Find_Player("Copper"))
    Reached = true
end
)LUA", not_planet_log);
    expect(count_code(not_planet.first, auth::codes::invalid_request) == 1 && not_planet.scripts.instances().empty() && spawn_count(*not_planet_log) == 1,
        "RGC-176 a non-planet object as the second argument creates nothing");

    // RGC-178: a null creation is a warning and no table.
    auto null_log = std::make_shared<SpawnLog>(); null_log->null_creation = true;
    auto null_run = run_spawn(root, "spawn_null", R"LUA(
function main()
    Result = Spawn_Unit(Find_Object_Type("Hero_Probe"), FindPlanet("Planet_Probe"), Find_Player("Copper"))
    Done = true
end
)LUA", null_log);
    expect(flag_global(null_run, "Done") && count_code(null_run.first, auth::codes::script_warning) == 1
        && handle_id(null_run, "Result") == 0 && integer_global(null_run, "Result") == -1,
        "RGC-178 a null creation warns and returns no table");

    // L-57: Prevent_AI_Usage needs an object and a Boolean.
    auto prevent_log = std::make_shared<SpawnLog>();
    auto prevent = run_spawn(root, "prevent_bad", R"LUA(
function main()
    local list = Spawn_Unit(Find_Object_Type("Hero_Probe"), FindPlanet("Planet_Probe"), Find_Player("Copper"))
    list[1].Prevent_AI_Usage("yes")
    Reached = true
end
)LUA", prevent_log);
    expect(!prevent.first.diagnostics.empty() && prevent.scripts.instances().empty()
        && std::none_of(prevent_log->actions.begin(), prevent_log->actions.end(), [](const auto& row) { return row.rfind("galactic.story_prevent", 0) == 0; }),
        "Prevent_AI_Usage without a Boolean is a script error and changes nothing");

    // Without the ordered native shadow (no AI runtime) creation faults by name instead of pretending.
    auto bare = run_spawn(root, "spawn_no_host", good, nullptr);
    expect(!bare.first.diagnostics.empty() && bare.scripts.instances().empty(), "Spawn_Unit without a native shadow host faults and creates nothing");
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
        planet_lookup(root / "planets");
        live_object_and_difficulty(root / "objects");
        advisor_hints_stub(root / "hints");
        spawn_bindings(root / "spawn");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

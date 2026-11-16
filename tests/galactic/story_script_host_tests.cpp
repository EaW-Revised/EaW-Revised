#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../../src/script/foc/mode_time.hpp"

#include <bit>
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

void object_modules(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    const auto data = root / "loose";
    write(data / "scripts" / "library" / "helper.lua", "function helper() return 1 end\n");
    write(data / "scripts" / "gameobject" / "beacon.lua", R"LUA(
require('helper')
Count = 0
function Base_Definitions() ServiceRate = 1 end
function main() while true do Count = Count + helper(); coroutine.yield(true) end end
)LUA");
    write(data / "scripts" / "gameobject" / "sleeping.lua", R"LUA(
function Base_Definitions() ServiceRate = 0 end
function Sleep(seconds)
    local start = GetCurrentTime()
    while GetCurrentTime() - start < seconds do coroutine.yield(true) end
end
function main() Count = 1; Sleep(0.1); Count = 2; while true do coroutine.yield(true) end end
)LUA");
    write(data / "scripts" / "gameobject" / "broken.lua", "error('injected object chunk failure')\n");
    write(data / "scripts" / "gameobject" / "rate_later.lua",
        "function Base_Definitions() end\nfunction main() Count = 1; coroutine.yield(true) end\n");
    write(data / "scripts" / "story" / "unrelated.lua", "error('unrelated')\n");
    eawr::vfs::MountSpec mount; mount.layer_id = "object-host"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> types{{7, " Beacon "}, {8, "Missing"}};
    const auto content = take(cpn::GalacticObjectScripts::load(files, types));
    expect(content->types().size() == 1 && content->types().at(7) == "data/scripts/gameobject/beacon.lua",
        "object modules resolve authored names without depending on a story plot");
    expect(content->modules().size() == 5 && content->notes().size() == 1,
        "the object host pins mounted GameObject and library modules and reports a missing script");
    expect(content->fingerprint() == take(cpn::GalacticObjectScripts::load(files, types))->fingerprint(),
        "object module identity is deterministic");
    std::optional<std::uint64_t> first_stamp_bits;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auth::SessionConfig config; config.galactic_clock = true;
        auto scheduler = take(content->create_scheduler(config));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        eawr::sim::galactic::CensusObject actor{};
        actor.id = 42; actor.original_type = actor.current_type = 7;
        const std::vector actors{actor};
        expect(take(content->reconcile(scheduler, actors, executor)).empty()
            && scheduler.object_scripts().size() == 1, "a live scripted actor attaches once");
        expect(take(content->reconcile(scheduler, actors, executor)).empty()
            && scheduler.object_scripts().size() == 1, "reconciliation retains the existing attachment");
        expect(take(content->service(scheduler, 1, executor)).diagnostics.empty(), "object definitions and thread start");
        expect(take(content->service(scheduler, 2, executor)).diagnostics.empty(), "new object thread pumps");
        const auto id = scheduler.object_scripts().front().instance;
        const auto count = take(scheduler.read_global(id, "Count"));
        expect(count && std::get<eawr::script::numeric::LuaNumber>(count->data) == eawr::script::numeric::LuaNumber(1),
            "object thread sees its mounted library");
        const auto first_stamp = take(scheduler.read_global(id, "LastService"));
        const auto* stamp = first_stamp ? std::get_if<eawr::script::numeric::LuaNumber>(&first_stamp->data) : nullptr;
        expect(stamp && *stamp <= eawr::script::numeric::LuaNumber(0) && *stamp >= eawr::script::numeric::LuaNumber(-1),
            "the first continuing object pump stamps a synchronized negative offset within its service rate");
        if (stamp) {
            if (!first_stamp_bits) first_stamp_bits = stamp->repr;
            expect(*first_stamp_bits == stamp->repr, "the first service offset agrees across worker counts");
        }
        const auto saved = take(scheduler.save());
        auto restored = take(content->create_scheduler(config));
        take(restored.load(saved));
        expect(take(content->reconcile(restored, actors, executor)).empty()
            && restored.object_scripts() == scheduler.object_scripts(), "restore keeps the object attachment");
        expect(take(content->service(scheduler, 3, executor)).diagnostics.empty(), "throttled object service succeeds");
        expect(take(content->service(restored, 3, executor)).diagnostics.empty()
            && take(restored.state_hash()) == take(scheduler.state_hash()), "restored object service continues identically");
        const auto throttled = take(scheduler.read_global(id, "Count"));
        const bool due_again = stamp && eawr::script::foc::tactical_ai_detail::mode_seconds(3) - *stamp
            > eawr::script::numeric::LuaNumber(1);
        expect(throttled && std::get<eawr::script::numeric::LuaNumber>(throttled->data)
                == eawr::script::numeric::LuaNumber(due_again ? 2 : 1),
            "object service honors its authored rate and first random offset");
        auto moved = actor; moved.owner = 2; moved.planet = 19; moved.container = 25;
        expect(take(content->reconcile(scheduler, std::vector{moved}, executor)).empty()
            && scheduler.object_scripts().front().instance == id, "a change of owner or parent retains the object script");
        const auto retained = take(scheduler.read_global(id, "Count"));
        expect(retained && std::get<eawr::script::numeric::LuaNumber>(retained->data)
                == eawr::script::numeric::LuaNumber(due_again ? 2 : 1),
            "a parent or owner change retains the script state");
        actor.delete_pending = true;
        expect(take(content->reconcile(scheduler, std::vector{actor}, executor)).empty()
            && scheduler.object_scripts().empty(), "a deleting actor detaches");
    }
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> sleeping_type{{9, "Sleeping"}};
    const auto sleeping = take(cpn::GalacticObjectScripts::load(files, sleeping_type));
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30};
        auto live = take(sleeping->create_scheduler(config));
        eawr::sim::galactic::CensusObject actor{}; actor.id = 99; actor.original_type = actor.current_type = 9;
        expect(take(sleeping->reconcile(live, std::vector{actor}, executor)).empty(), "sleeping object attaches");
        expect(take(sleeping->service(live, 1, executor)).diagnostics.empty()
            && take(sleeping->service(live, 2, executor)).diagnostics.empty(), "sleeping object starts");
        const auto id = live.object_scripts().front().instance;
        const auto sleeping_count = take(live.read_global(id, "Count"));
        expect(sleeping_count && std::get<eawr::script::numeric::LuaNumber>(sleeping_count->data)
            == eawr::script::numeric::LuaNumber(1), "sleeping object is suspended before its continuation");
        const auto saved = take(live.save());
        auto restored = take(sleeping->create_scheduler(config));
        take(restored.load(saved));
        for (std::uint64_t frame = 3; frame <= 8; ++frame) {
            expect(take(sleeping->service(live, frame, executor)).diagnostics.empty()
                && take(sleeping->service(restored, frame, executor)).diagnostics.empty(), "sleeping object resumes cleanly");
            expect(take(live.state_hash()) == take(restored.state_hash()), "sleeping object restore follows the same state");
        }
        const auto resumed = take(restored.read_global(id, "Count"));
        expect(resumed && std::get<eawr::script::numeric::LuaNumber>(resumed->data)
            == eawr::script::numeric::LuaNumber(2), "sleeping object resumes after the saved wait");
    }
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> late_rate_type{{11, "Rate_Later"}};
    const auto late_rate = take(cpn::GalacticObjectScripts::load(files, late_rate_type));
    auth::SessionConfig late_rate_config; late_rate_config.galactic_clock = true;
    auto late = take(late_rate->create_scheduler(late_rate_config));
    eawr::sim::galactic::CensusObject late_actor{}; late_actor.id = 102; late_actor.original_type = late_actor.current_type = 11;
    const eawr::sim::InlineExecutor late_executor;
    expect(take(late_rate->reconcile(late, std::vector{late_actor}, late_executor)).empty(), "late-rate object attaches");
    expect(take(late_rate->service(late, 1, late_executor)).diagnostics.empty()
        && take(late_rate->service(late, 2, late_executor)).diagnostics.empty(), "unrated object initializes");
    const auto late_id = late.object_scripts().front().instance;
    const auto before_rate = take(late.read_global(late_id, "Count"));
    expect(before_rate && std::holds_alternative<std::monostate>(before_rate->data),
        "an object with no numeric ServiceRate does not pump");
    auth::ScriptEvent set_rate;
    set_rate.key = {3, auth::first_simulation_producer, late_id, 10}; set_rate.target = late_id;
    set_rate.kind = auth::ScriptEvent::Kind::assign; set_rate.name = "ServiceRate";
    set_rate.parameter = auth::Value::text("not numeric");
    take(late.submit_event(set_rate));
    expect(take(late_rate->service(late, 3, late_executor)).diagnostics.empty(), "non-numeric rate assignment is delivered");
    const auto during_rate = take(late.read_global(late_id, "Count"));
    expect(during_rate && std::holds_alternative<std::monostate>(during_rate->data), "non-numeric rate does not start the thread");
    set_rate.key.tick = 4; set_rate.parameter = auth::Value::number(eawr::script::numeric::LuaNumber(0));
    take(late.submit_event(std::move(set_rate)));
    expect(take(late_rate->service(late, 4, late_executor)).diagnostics.empty(), "numeric rate assignment is delivered");
    const auto assigned = take(late.read_global(late_id, "Count"));
    expect(assigned && std::holds_alternative<std::monostate>(assigned->data), "a new rate is read at the next visit");
    expect(take(late_rate->service(late, 5, late_executor)).diagnostics.empty(), "new numeric rate admits a pump");
    const auto late_count = take(late.read_global(late_id, "Count"));
    expect(late_count && std::get<eawr::script::numeric::LuaNumber>(late_count->data)
        == eawr::script::numeric::LuaNumber(1), "the newly admitted object thread runs once");
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> broken_type{{10, "Broken"}};
    const auto broken = take(cpn::GalacticObjectScripts::load(files, broken_type));
    auth::SessionConfig broken_config; broken_config.galactic_clock = true;
    auto failed = take(broken->create_scheduler(broken_config));
    eawr::sim::galactic::CensusObject failed_actor{}; failed_actor.id = 101; failed_actor.original_type = failed_actor.current_type = 10;
    const eawr::sim::InlineExecutor broken_executor;
    expect(take(broken->reconcile(failed, std::vector{failed_actor}, broken_executor)).size() == 1
        && failed.instances().empty() && failed.object_scripts().size() == 1,
        "a failed object chunk is diagnosed while its attachment remains known");
    expect(take(broken->reconcile(failed, std::vector{failed_actor}, broken_executor)).empty(),
        "an unchanged failed attachment is not retried each service");
    const auto failed_save = take(failed.save());
    auto failed_restored = take(broken->create_scheduler(broken_config));
    take(failed_restored.load(failed_save));
    expect(take(broken->reconcile(failed_restored, std::vector{failed_actor}, broken_executor)).empty()
        && failed_restored.instances().empty(), "a failed attachment is not retried on restore");
    auto duplicate = types; duplicate.push_back({8, "Beacon"});
    expect(!cpn::GalacticObjectScripts::load(files, duplicate), "duplicate scripted type identities are rejected");
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> no_scripts{{7, ""}};
    expect(take(cpn::GalacticObjectScripts::load(files, no_scripts))->modules().empty(), "empty object inventories pin no scripts");
    cpn::GalacticObjectQueryCatalog query;
    eawr::units::UnitTables vocabulary;
    eawr::units::UnitType loaded;
    loaded.id = "BEACON"; loaded.category_bits = 2; loaded.property_bits = 4;
    vocabulary.units.push_back(loaded);
    vocabulary.categories.push_back({"vehicle", 8});
    vocabulary.properties.push_back({"rare", 16});
    const std::vector<std::pair<std::string, eawr::sim::galactic::TypeId>> names{{"BEACON", 7}};
    const auto built = take(cpn::make_galactic_object_query_catalog(vocabulary, names));
    expect(built.types.at(7).categories == 2 && built.types.at(7).properties == 4
        && built.categories.at("VEHICLE") == 8 && built.properties.at("RARE") == 16,
        "object query vocabulary uses loaded type and enum masks");
    const std::vector<std::pair<std::string, eawr::sim::galactic::TypeId>> missing{{"ABSENT", 8}};
    const auto unqualified = take(cpn::make_galactic_object_query_catalog(vocabulary, missing));
    expect(unqualified.names.at("ABSENT") == 8 && !unqualified.types.at(8).qualified,
        "object query retains known identities without inventing missing type flags");
    const std::vector<std::pair<std::string, eawr::sim::galactic::TypeId>> aliases{{"BEACON", 9}, {"beacon", 7}};
    const auto duplicate_names = take(cpn::make_galactic_object_query_catalog(vocabulary, aliases));
    expect(duplicate_names.names.at("BEACON") == 7 && duplicate_names.types.size() == 2,
        "repeated authored names keep all type identities and resolve the first identity deterministically");
    query.types.emplace(7, cpn::GalacticObjectQueryType{7, "BEACON", 2, 4});
    query.types.emplace(8, cpn::GalacticObjectQueryType{8, "OTHER", 8, 16});
    query.names = {{"BEACON", 7}, {"OTHER", 8}};
    query.categories = {{"VEHICLE", 8}}; query.properties = {{"RARE", 16}};
    const auth::ValueList filters{auth::Value{auth::Handle{2, 1}}, auth::Value{auth::Handle{3, 7}},
        auth::Value::text("vehicle"), auth::Value::text("RARE")};
    const auto filter = take(cpn::parse_galactic_object_filter(filters, query));
    eawr::sim::galactic::CensusObject actor{}; actor.id = 42; actor.owner = 1;
    actor.original_type = 7; actor.current_type = 8;
    expect(take(cpn::matches_galactic_object_filter(filter, actor, &query.types.at(8))),
        "object query intersects owner, original type and qualified flags");
    actor.owner = 2;
    expect(!take(cpn::matches_galactic_object_filter(filter, actor, &query.types.at(8))), "object query uses exact ownership");
    expect(!cpn::parse_galactic_object_filter({}, query)
        && !cpn::parse_galactic_object_filter({auth::Value::text("unknown")}, query),
        "empty or unknown object filters are rejected");
    query.properties.emplace("BEACON", 16);
    const auto ambiguous = take(cpn::parse_galactic_object_filter({auth::Value::text("beacon")}, query));
    expect(ambiguous.properties == 16 && !ambiguous.type, "property names take precedence over type names");
    expect(!cpn::matches_galactic_object_filter(ambiguous, actor, nullptr),
        "missing qualified flags cannot fabricate a category or property match");
    actor.owner = 1; actor.planet = 12;
    const std::vector view{actor};
    const eawr::sim::InlineExecutor query_executor;
    const auto handle = auth::Value{auth::Handle{1, 42}};
    const auto found = take(cpn::query_galactic_object_view("galactic.object_find_all", filters, view, query, query_executor));
    expect(std::get<auth::ValueList>(found.front().data).size() == 1, "object view enumerates matching live actors");
    const auto original = take(cpn::query_galactic_object_view("galactic.object_type", {handle}, view, query, query_executor));
    expect(std::get<auth::Handle>(original.front().data).id == 7, "Get_Type returns original type (WGA-312)");
    const auto location = take(cpn::query_galactic_object_view("galactic.object_planet", {handle}, view, query, query_executor));
    expect(std::get<auth::Handle>(location.front().data).id == 12, "object view resolves its containing planet");
    const auto absent = take(cpn::query_galactic_object_view("galactic.object_valid", {auth::Value{auth::Handle{1, 99}}}, view, query, query_executor));
    expect(!std::get<bool>(absent.front().data), "removed objects are invalid");
    auto second = actor; second.id = 44;
    auto deleting = actor; deleting.id = 45; deleting.delete_pending = true;
    const std::vector ordered_view{actor, second, deleting};
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        const auto result = take(cpn::query_galactic_object_view("galactic.object_find_all", filters, ordered_view, query, threaded));
        const auto& matches = std::get<auth::ValueList>(result.front().data);
        expect(matches.size() == 2 && std::get<auth::Handle>(matches[0].data).id == 42
            && std::get<auth::Handle>(matches[1].data).id == 44,
            "object enumeration excludes pending deletion and keeps identity order across worker counts");
    }
}

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
    // An uncaught error ends the failing thread, not the story script: nothing is retired.
    const auto failed_script = content->find("G.xml")->instance;
    expect(content->subplots_of(std::vector<std::uint64_t>{failed_script, 987654}) == std::vector<std::string>{"G.xml"}
        && content->subplots_of({}).empty(), "a failed script instance maps back to its sub-plot, unknown instances to nothing");
    const auto running = scripts.instances();
    expect(scripts.retired_instances().empty() && first.load_notes.empty()
        && std::binary_search(running.begin(), running.end(), failed_script),
        "the unbound story function ends its thread; the script is not retired and no runtime LoadNote is written");

    // The calls: the scripted sub-plot is called; the unscripted, the unbound and the unknown ones are dropped.
    std::uint64_t sequence = 0;
    const std::vector<st::ScriptCall> calls{{0, "Probe_Event"}, {1, "Ignored"}, {2, "Ignored"}, {6, "Ignored_Retired"}, {0, "Second_Event"}};
    take(cpn::forward_galactic_story_calls(scripts, *content, subs, calls, sequence));
    expect(sequence == 3, "only calls for running scripts take an event sequence (the script whose thread failed still runs)");
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
    expect(reloaded.retired_instances().empty() && reloaded.instances() == scripts.instances(),
        "story restore keeps the scripts that had a failed thread running");
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
    expect(faulted.load_notes.empty() && callback.retired_instances().empty()
        && std::binary_search(survivors.begin(), survivors.end(), a->instance)
        && std::binary_search(survivors.begin(), survivors.end(), content->find("D.xml")->instance),
        "uncaught story callback failure ends that call only; no script is retired");
    std::uint64_t after_fault = 0;
    take(cpn::forward_galactic_story_calls(callback, *content, subs, calls, after_fault));
    expect(after_fault == 3, "later story requests still reach the scripts that are running");
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

// #7073 (FS-44, WGS-140): the authored scoring module's Base_Definitions runs once, first, in an instance of its own; what it sets through
// GlobalValue.Set is what a plot's script reads from its first line on (a chunk's load, its Base_Definitions, its main). Synthetic names only.
struct ScoringFixture {
    std::shared_ptr<const cpn::GalacticStoryScripts> content;
    std::vector<st::SubPlotDef> subs;
};
ScoringFixture scoring_fixture(const std::filesystem::path& root, const std::string& name, const std::string& constants,
    const std::string& scoring_body, bool sibling = true, const std::string& reader_body = {}) {
    const auto data = root / name / "loose";
    std::filesystem::remove_all(root / name);
    write(data / "scripts" / "library" / "storyprobe.lua", std::string(library));
    if (!reader_body.empty()) write(data / "scripts" / "story" / "Reader_Script.lua", reader_body);
    else write(data / "scripts" / "story" / "Reader_Script.lua",
        "require(\"StoryProbe\")\n"
        "SeenAtLoad = GlobalValue.Get(\"SCORE_MARK\")\n"
        "function Base_Definitions() SeenInBase = GlobalValue.Get(\"SCORE_MARK\"); SeenText = GlobalValue.Get(\"SCORE_TEXT\"); Leak = Score_Private; LeakSibling = Sibling_Loaded end\n"
        "function main() SeenInMain = GlobalValue.Get(\"SCORE_MARK\") end\n");
    if (!constants.empty()) write(data / "xml" / "gameconstants.xml", constants);
    if (!scoring_body.empty()) write(data / "scripts" / "miscellaneous" / "MyScoring.lua", scoring_body);
    if (sibling) write(data / "scripts" / "miscellaneous" / "ScoreSibling.lua", "Sibling_Loaded = 1\n");
    eawr::vfs::MountSpec mount; mount.layer_id = "scoring-" + name; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    std::vector<st::SubPlotDef> subs{sub("Reader.xml", "Reader_Script", {})};
    auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    return {std::move(content), std::move(subs)};
}
constexpr std::string_view scoring_constants = "<GameConstants><Game_Scoring_Script_Name>MyScoring</Game_Scoring_Script_Name></GameConstants>";
constexpr std::string_view scoring_module =
    "require(\"ScoreSibling\")\n"
    "Score_Private = 5\n"
    "function Base_Definitions() GlobalValue.Set(\"SCORE_MARK\", 7); GlobalValue.Set(\"SCORE_TEXT\", \"mod\"); Scoring_Only = 1 end\n"
    "function main() GlobalValue.Set(\"SCORE_MAIN_RAN\", 1) end\n";

std::optional<std::uint64_t> number_of(auth::ScriptScheduler& scripts, std::uint64_t instance, std::string_view name) {
    const auto value = take(scripts.read_global(instance, name));
    const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
    return n ? std::optional(n->repr) : std::nullopt;
}
bool nil_global(auth::ScriptScheduler& scripts, std::uint64_t instance, std::string_view name) {
    const auto value = take(scripts.read_global(instance, name));
    return !value || std::holds_alternative<std::monostate>(value->data);
}
const auto n7 = eawr::script::numeric::LuaNumber(7).repr;
const auto n11 = eawr::script::numeric::LuaNumber(11).repr;
const auto n9 = eawr::script::numeric::LuaNumber(9).repr;

void scoring_definitions(const std::filesystem::path& root) {
    auth::SessionConfig config;
    config.seed = 77; config.galactic_clock = true; config.tick_duration = {1, 30};
    const auto fixture = scoring_fixture(root, "scored", std::string(scoring_constants), std::string(scoring_module));
    const auto& content = fixture.content;
    expect(content->notes().empty() && content->entries().size() == 1 && content->initial_globals().empty(),
        "loading pins the scoring module and runs nothing: no value yet, no note, the plot's entry unchanged");
    const auto run = [&](const eawr::sim::PartitionExecutor& executor, auth::GlobalValueReader globals = {}) {
        auto scripts = take(cpn::create_galactic_story_scheduler(*content, config, false, {}, {}, std::move(globals)));
        std::vector<std::string> notes;
        take(cpn::start_galactic_story_scripts(scripts, *content, notes));
        expect(notes.empty(), "the plot's script starts although it reads the scoring definitions' value while its chunk loads");
        auth::ServiceOptions options; options.time_tick = 1;
        take(scripts.service(executor, options));
        return scripts;
    };
    const eawr::sim::InlineExecutor inline_executor;
    auto scripts = run(inline_executor);
    expect(content->notes().empty() && content->entries().size() == 1 && content->initial_globals().size() == 2,
        "the named scoring module's definitions ran once when the scheduler was made: two values, no note");
    const auto mark = content->initial_globals().find("SCORE_MARK");
    expect(mark != content->initial_globals().end() && std::get_if<eawr::script::numeric::LuaNumber>(&mark->second.data)
        && std::get_if<eawr::script::numeric::LuaNumber>(&mark->second.data)->repr == n7,
        "a number the definitions set is kept as it was set");
    expect(!content->initial_globals().contains("SCORE_MAIN_RAN"), "the scoring module's main is not started (#7075)");

    const auto instance = content->find("Reader.xml")->instance;
    const auto text = take(scripts.read_global(instance, "SeenText"));
    expect(number_of(scripts, instance, "SeenAtLoad") == n7 && number_of(scripts, instance, "SeenInBase") == n7
        && number_of(scripts, instance, "SeenInMain") == n7 && text && std::get_if<std::string>(&text->data)
        && *std::get_if<std::string>(&text->data) == "mod",
        "the scoring definitions' values are visible to a plot script in the same fresh-load pass: its chunk, its Base_Definitions and its main");
    expect(nil_global(scripts, instance, "Leak") && nil_global(scripts, instance, "LeakSibling"),
        "the scoring module's own globals and the modules it required stay out of the plot's environment");
    const auto running = scripts.instances();
    expect(running == std::vector<std::uint64_t>{instance}, "the scoring module has no instance in the story scheduler");

    // A value the registry already holds replaces the definitions' starting value.
    auto replaced = run(inline_executor, [](const std::string& key) -> std::optional<auth::Value> {
        if (key == "SCORE_MARK") return auth::Value::number(eawr::script::numeric::LuaNumber(11));
        return std::nullopt;
    });
    expect(number_of(replaced, instance, "SeenAtLoad") == n11 && number_of(replaced, instance, "SeenInMain") == n11
        && !nil_global(replaced, instance, "SeenText"), "a registry value wins over the scoring definitions' starting value; a key it lacks keeps the starting one");

    // 1/2/4/8 workers and a save/restore into a fresh scheduler of the same content.
    std::string reference;
    for (const std::size_t workers : std::vector<std::size_t>{1, 2, 4, 8}) {
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        auto one = run(threaded);
        const auto hash = take(one.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "the scored story scheduler is equal on " + std::to_string(workers) + " workers");
    }
    const auto saved = take(scripts.save());
    auto reloaded = take(cpn::create_galactic_story_scheduler(*content, config));
    take(reloaded.load(saved));
    expect(take(reloaded.state_hash()) == take(scripts.state_hash()), "a restored story scheduler has the saved state");
    auth::ServiceOptions next; next.time_tick = 2;
    take(scripts.service(inline_executor, next)); take(reloaded.service(inline_executor, next));
    expect(take(reloaded.state_hash()) == take(scripts.state_hash()) && number_of(reloaded, instance, "SeenInMain") == n7,
        "the restored scheduler services like the original and its plot keeps what it read");

    // No constant, or a constant naming a module the data does not author: nothing runs, nothing changes (same identity as no scoring at all).
    const auto plain = scoring_fixture(root, "plain", "", "");
    const auto unauthored = scoring_fixture(root, "unauthored", std::string(scoring_constants), "");
    const auto made = [&](const ScoringFixture& f) { take(cpn::create_galactic_story_scheduler(*f.content, config)); };
    made(plain); made(unauthored);
    expect(plain.content->initial_globals().empty() && plain.content->scoring_notes().empty() && unauthored.content->initial_globals().empty()
        && unauthored.content->scoring_notes().empty() && plain.content->fingerprint() == unauthored.content->fingerprint(),
        "no scoring module, or one the data does not author: no starting values, no note, the content identity is the same");
    expect(plain.content->fingerprint() != content->fingerprint(), "a pinned scoring module is part of the content identity");

    // A module that sets nothing starts nothing (its bytes are still part of the identity: it runs at every start).
    const auto silent = scoring_fixture(root, "silent", std::string(scoring_constants), "function Base_Definitions() Quiet = 1 end\n");
    made(silent);
    expect(silent.content->initial_globals().empty() && silent.content->scoring_notes().empty(), "a scoring module that sets no global starts no value");

    // The definitions look up the campaign's object types (a mod's MOD_ID comes from a dummy type), so the values depend on the players handed to the factory.
    const auto typed = scoring_fixture(root, "typed", std::string(scoring_constants),
        "function Base_Definitions() local d = Find_Object_Type(\"Mod_Dummy\"); if d then GlobalValue.Set(\"MOD_ID\", \"mymod\") else GlobalValue.Set(\"MOD_ID\", nil) end end\n");
    made(typed);
    expect(typed.content->initial_globals().empty() && typed.content->scoring_notes().empty(), "a type the campaign lacks leaves the value unset: a nil write starts nothing");
    cpn::GalacticStoryPlayers with_types;
    with_types.types = {{"MOD_DUMMY", 5}};
    take(cpn::create_galactic_story_scheduler(*typed.content, config, false, with_types));
    const auto owner = typed.content->initial_globals().find("MOD_ID");
    expect(owner != typed.content->initial_globals().end() && std::get_if<std::string>(&owner->second.data)
        && *std::get_if<std::string>(&owner->second.data) == "mymod", "the definitions find the campaign's dummy type and set the mod's identity");

    // A fault in the definitions is a named note, never an abort; the plots start as before.
    const auto broken = scoring_fixture(root, "broken", std::string(scoring_constants), "function Base_Definitions() Find_Hint(\"x\") end\n");
    made(broken);
    bool named = false;
    for (const auto& note : broken.content->scoring_notes()) named = named || note.find("scoring definitions: data/scripts/miscellaneous/myscoring.lua") != std::string::npos;
    expect(named && broken.content->initial_globals().empty() && broken.content->entries().size() == 1,
        "a scoring module whose definitions fault is listed by name and sets nothing; the plot's entry is unchanged");
    const auto bad_name = scoring_fixture(root, "badname", "<GameConstants><Game_Scoring_Script_Name>../x</Game_Scoring_Script_Name></GameConstants>", "");
    expect(bad_name.content->initial_globals().empty() && !bad_name.content->notes().empty(),
        "a constant that is not a module name is listed and runs nothing");
}

// GC review of #7100: a restore builds a new scheduler and so runs the scoring definitions again; what they set must never overwrite a
// registry value a plot changed before the save. The registry here is the host's: a map the test fills from the plot's GlobalValue.Set commands.
void scoring_restore(const std::filesystem::path& root) {
    auth::SessionConfig config;
    config.seed = 77; config.galactic_clock = true; config.tick_duration = {1, 30};
    const auto fixture = scoring_fixture(root, "restore", std::string(scoring_constants), std::string(scoring_module),
        true, "require(\"StoryProbe\")\nfunction main() GlobalValue.Set(\"SCORE_MARK\", 9) end\nfunction Probe_Read() Seen = GlobalValue.Get(\"SCORE_MARK\") end\n");
    const auto& content = fixture.content;
    std::map<std::string, auth::Value> registry;
    const auto reader = [&registry](const std::string& key) -> std::optional<auth::Value> {
        const auto found = registry.find(key);
        return found == registry.end() ? std::nullopt : std::optional<auth::Value>(found->second);
    };
    const auto apply = [&registry](const auth::ServiceReport& report) {
        for (const auto& command : report.commands) {
            if (command.verb != auth::global_value_command || command.arguments.size() != 2) continue;
            const auto* name = std::get_if<std::string>(&command.arguments[0].data);
            if (name) registry.insert_or_assign(*name, command.arguments[1]);
        }
    };
    const eawr::sim::InlineExecutor executor;
    auto before = take(cpn::create_galactic_story_scheduler(*content, config, false, {}, {}, reader));
    std::vector<std::string> notes;
    take(cpn::start_galactic_story_scripts(before, *content, notes));
    auth::ServiceOptions options; options.time_tick = 1;
    apply(take(before.service(executor, options)));
    const auto instance = content->find("Reader.xml")->instance;
    const auto mark = registry.find("SCORE_MARK");
    expect(notes.empty() && mark != registry.end() && std::get_if<eawr::script::numeric::LuaNumber>(&mark->second.data)
        && std::get_if<eawr::script::numeric::LuaNumber>(&mark->second.data)->repr == n9,
        "the plot changed SCORE_MARK from the definitions' 7 to 9 in the registry");
    const auto read = [&](auth::ScriptScheduler& scripts, std::uint64_t tick, std::uint64_t sequence) {
        auth::ScriptEvent event;
        event.key = {tick, auth::first_simulation_producer, instance, sequence};
        event.target = instance; event.kind = auth::ScriptEvent::Kind::call; event.name = "Probe_Read";
        take(scripts.submit_event(std::move(event)));
        auth::ServiceOptions step; step.time_tick = tick;
        apply(take(scripts.service(executor, step)));
        return number_of(scripts, instance, "Seen");
    };
    expect(read(before, 2, 100) == n9, "before the save the plot reads its own value");
    const auto saved = take(before.save());

    // The restore: a new scheduler (its definitions run again), the saved scripts, the host's saved registry.
    auto after = take(cpn::create_galactic_story_scheduler(*content, config, false, {}, {}, reader));
    take(after.load(saved));
    const auto start = content->initial_globals().find("SCORE_MARK");
    expect(start != content->initial_globals().end() && std::get_if<eawr::script::numeric::LuaNumber>(&start->second.data)
        && std::get_if<eawr::script::numeric::LuaNumber>(&start->second.data)->repr == n7,
        "the restore ran the definitions again and they still start SCORE_MARK at 7");
    expect(read(after, 3, 101) == n9, "the restored registry's 9 wins over the re-run definitions' 7");

    // A key the registry lacks answers the starting value, as at a fresh start.
    registry.erase("SCORE_MARK");
    expect(read(after, 4, 102) == n7, "a key the registry lacks answers the starting value");
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
// #7101/#7102: Get_Game_Mode answers the galactic mode's name in the story host (L-51), and Player.Is_Human answers whether the registered
// player's seat is human-controlled. The mods call both from their first story pass (StoryUtil.GetPlayerAgnosticPlot, FTGULibrary, GCUtility*).
void game_mode_and_human(const std::filesystem::path& root) {
    cpn::GalacticStoryPlayers roster;
    roster.factions = {"Copper", "Indigo"};
    roster.players = {{9, "Copper", "Normal", true}, {7, "Indigo", "Normal", false}};
    const std::string body = R"LUA(
function main()
    Mode = Get_Game_Mode()
    CopperHuman = Find_Player("Copper").Is_Human()
    IndigoHuman = Find_Player("Indigo").Is_Human()
    IndigoKind = type(Find_Player("Indigo").Is_Human())
    Alike = Find_Player("copper").Is_Human() == CopperHuman
end
)LUA";
    std::string reference;
    for (const std::size_t workers : std::vector<std::size_t>{1, 2, 4, 8}) {
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        auto run = run_scripted(root, "human_mode", body, false, 4242, threaded, roster);
        expect(run.first.diagnostics.empty(), "Get_Game_Mode and Is_Human raise no diagnostic");
        const auto mode = take(run.scripts.read_global(run.instance, "Mode"));
        const auto kind = take(run.scripts.read_global(run.instance, "IndigoKind"));
        expect(mode && std::get_if<std::string>(&mode->data) && *std::get_if<std::string>(&mode->data) == "Galactic",
            "Get_Game_Mode answers Galactic in the galactic story host");
        expect(flag_global(run, "CopperHuman") && !flag_global(run, "IndigoHuman") && flag_global(run, "Alike")
            && kind && std::get_if<std::string>(&kind->data) && *std::get_if<std::string>(&kind->data) == "boolean",
            "Is_Human is true for the human seat's player and a boolean false for an AI seat's, whichever way the faction is spelled");
        const auto hash = take(run.scripts.state_hash());
        if (reference.empty()) reference = hash;
        expect(hash == reference, "the mode and human answers are equal on " + std::to_string(workers) + " workers");
    }
}

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
    eawr::core::Result<auth::ValueList> query(std::string_view name, const auth::ValueList&) const override {
        if (name == "galactic.object_find_all")
            return eawr::core::Result<auth::ValueList>::success({auth::Value{std::vector<auth::Value>{auth::Value{auth::Handle{1, 42}}}}});
        if (name == "galactic.object_valid") return eawr::core::Result<auth::ValueList>::success({auth::Value{true}});
        if (name == "galactic.object_planet") return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{1, 9}}});
        if (name == "galactic.object_type") return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{3, 70}}});
        if (name == "galactic.object_owner") return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{2, 1}}});
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

void object_registration_order(const std::filesystem::path& root) {
    const auto data = root / "registration" / "loose";
    std::filesystem::remove_all(root / "registration");
    write(data / "scripts" / "gameobject" / "order.lua", R"LUA(
function Base_Definitions() ServiceRate = 0 end
function main() Object.Despawn(); coroutine.yield(true) end
)LUA");
    eawr::vfs::MountSpec mount; mount.layer_id = "object-registration"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> types{{7, "Order"}};
    const auto content = take(cpn::GalacticObjectScripts::load(files, types));
    auth::SessionConfig config; config.galactic_clock = true;
    auto scripts = take(content->create_scheduler(config));
    take(cpn::register_galactic_object_bindings(scripts, {}));
    auto log = std::make_shared<SpawnLog>();
    take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(log), 4));
    eawr::sim::galactic::CensusObject first{}; first.id = 40; first.original_type = first.current_type = 7;
    auto second = first; second.id = 99;
    const eawr::sim::InlineExecutor executor;
    take(content->reconcile(scripts, std::vector{first}, executor));
    take(content->reconcile(scripts, std::vector{first, second}, executor));
    take(content->service(scripts, 1, executor));
    const auto saved = take(scripts.save());
    auto restored = take(content->create_scheduler(config));
    take(cpn::register_galactic_object_bindings(restored, {}));
    auto restored_log = std::make_shared<SpawnLog>();
    take(restored.set_immediate_effect_host(std::make_shared<SpawnHost>(restored_log), 4));
    take(restored.load(saved));
    take(content->service(scripts, 2, executor));
    take(content->service(restored, 2, executor));
    expect(log->actions.size() == 2 && log->actions[0].ends_with("/99") && log->actions[1].ends_with("/40")
        && restored_log->actions == log->actions && take(restored.state_hash()) == take(scripts.state_hash()),
        "object scripts run in reverse registration order before and after restore");
}

void object_binding_primitives() {
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest modules;
    take(modules.add("Object.lua", R"LUA(
-- The data's own base library defines TestValid in Lua; a native binding of that name would refuse this load.
function TestValid(wrapper)
    if wrapper == nil then return false end
    if wrapper.Is_Valid == nil then return false end
    return wrapper.Is_Valid()
end
function main()
    local t = Find_Object_Type('hero_probe')
    TypeValid = t.Is_Valid()
    GlobalTypeValid = TestValid(t)
    NilValid = TestValid(nil)
    Name = t.Get_Name()
    Mode = Get_Game_Mode()
    Count = 0
    for _, object in pairs(Find_All_Objects_Of_Type(t)) do
        if TestValid(object) and object.Get_Planet_Location() == Planet()
            and object.Get_Type() == t and object.Get_Owner() == Player() then Count = Count + 1 end
    end
    local result = Spawn_Unit(t, Planet(), Player())
    result[1].Prevent_AI_Usage(true)
    result[1].Despawn()
    Story_Event('Probe', Planet())
    Flag = Check_Story_Flag(Player(), 'missing')
end
Create_Thread('main')
)LUA"));
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(modules)));
    cpn::GalacticStoryPlayers players{}; players.types = {{"Hero_Probe", 70}};
    take(cpn::register_galactic_object_bindings(scripts, players, std::make_shared<cpn::GalacticStoryLink>()));
    take(scripts.register_binding("Planet", [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{1, 9}}});
    }));
    take(scripts.register_binding("Player", [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{2, 1}}});
    }));
    auto log = std::make_shared<SpawnLog>();
    take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(log), 1));
    take(scripts.create_instance(41, "Object.lua"));
    auth::ServiceOptions options; options.time_tick = 1;
    const auto report = take(scripts.service(eawr::sim::InlineExecutor{}, options));
    expect(report.diagnostics.empty() && log->actions.size() == 3
        && log->actions[0].starts_with("galactic.spawn_unit")
        && log->actions[1].starts_with("galactic.story_prevent_ai_usage")
        && log->actions[2].starts_with("galactic.object_despawn"),
        "object bindings route placement, AI usage and despawn to the native owner");
    expect(report.commands.size() == 1 && report.commands.front().verb == "galactic.story_event",
        "object story events enter the world story command stream");
    const auto mode = take(scripts.read_global(41, "Mode"));
    const auto name = take(scripts.read_global(41, "Name"));
    const auto count = take(scripts.read_global(41, "Count"));
    const auto flag = take(scripts.read_global(41, "Flag"));
    const auto type_valid = take(scripts.read_global(41, "TypeValid"));
    const auto global_type_valid = take(scripts.read_global(41, "GlobalTypeValid"));
    const auto nil_valid = take(scripts.read_global(41, "NilValid"));
    expect(mode && name && std::get<std::string>(mode->data) == "Galactic"
        && std::get<std::string>(name->data) == "Hero_Probe", "object bindings preserve mode and authored type names");
    expect(count && std::get<eawr::script::numeric::LuaNumber>(count->data) == eawr::script::numeric::LuaNumber(1),
        "object enumeration and location reads reach the native owner");
    expect(flag && !std::get<bool>(flag->data), "object scripts read story flags from the world story link");
    expect(type_valid && std::get<bool>(type_valid->data), "loaded type wrappers satisfy TestValid");
    expect(global_type_valid && std::get<bool>(global_type_valid->data) && nil_valid && !std::get<bool>(nil_valid->data),
        "TestValid checks types and nil using the object host");
}

void object_perception_unavailable() {
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest modules;
    take(modules.add("Unavailable.lua", "function main() EvaluatePerception('x', nil) end\nCreate_Thread('main')\n"));
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(modules)));
    take(cpn::register_galactic_object_bindings(scripts, {}));
    take(scripts.create_instance(41, "Unavailable.lua"));
    auth::ServiceOptions options; options.time_tick = 1;
    const auto report = take(scripts.service(eawr::sim::InlineExecutor{}, options));
    expect(!report.load_notes.empty() && report.load_notes.front().message.find("EvaluatePerception is not available") != std::string::npos
        && !report.removed_instances.empty(), "unavailable world perception faults and retires the object script");
}

void object_sale_bindings() {
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest modules;
    take(modules.add("Sale.lua", R"LUA(
function main()
    local cost = Find_Object_Type('Hero_Probe').Get_Build_Cost()
    Refund = cost / 3 + 1
    Player().Give_Money(Refund)
end
Create_Thread('main')
)LUA"));
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(modules)));
    cpn::GalacticStoryPlayers players{}; players.types = {{"Hero_Probe", 70}};
    players.type_build_costs.emplace(70, take(eawr::sim::math::Fixed::from_integer(900)));
    take(cpn::register_galactic_object_bindings(scripts, players, std::make_shared<cpn::GalacticStoryLink>()));
    take(scripts.register_binding("Player", [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{2, 1}}});
    }));
    auto log = std::make_shared<SpawnLog>();
    take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(log), 1));
    take(scripts.create_instance(41, "Sale.lua"));
    auth::ServiceOptions options; options.time_tick = 1;
    const auto report = take(scripts.service(eawr::sim::InlineExecutor{}, options));
    const auto refund = take(scripts.read_global(41, "Refund"));
    expect(report.diagnostics.empty() && refund
        && std::get<eawr::script::numeric::LuaNumber>(refund->data) == eawr::script::numeric::LuaNumber(301)
        && log->actions.size() == 1 && log->actions.front().starts_with("galactic.give_money"),
        "object scripts compute the authored sale refund and send it to the world credit owner");
}

void attached_object_calls_native(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    const auto data = root / "loose";
    write(data / "scripts" / "library" / "helper.lua", R"LUA(
function CountAt(type_name, planet)
    local count = 0
    for _, object in pairs(Find_All_Objects_Of_Type(Find_Object_Type(type_name))) do
        if object.Is_Valid() and object.Get_Planet_Location() == planet then count = count + 1 end
    end
    return count
end
)LUA");
    write(data / "scripts" / "gameobject" / "beacon.lua", R"LUA(
require('helper')
function Base_Definitions() Object = nil; _ScriptMessage('start'); ServiceRate = 0 end
function main()
    local planet = Object.Get_Planet_Location()
    if CountAt('Hero_Probe', planet) == 1 then
        local created = Spawn_Unit(Find_Object_Type('Hero_Probe'), planet, Object.Get_Owner())
        created[1].Prevent_AI_Usage(true)
    end
    Object.Service_Wrapper()
    coroutine.yield(true)
end
)LUA");
    eawr::vfs::MountSpec mount; mount.layer_id = "attached-object"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    const std::vector<std::pair<eawr::sim::galactic::TypeId, std::string>> types{{7, "Beacon"}};
    const auto content = take(cpn::GalacticObjectScripts::load(files, types));
    auth::SessionConfig config; config.galactic_clock = true;
    auto scripts = take(content->create_scheduler(config));
    cpn::GalacticStoryPlayers players{}; players.types = {{"Hero_Probe", 70}}; players.planets = {{"ProbePlanet", 9}};
    take(cpn::register_galactic_object_bindings(scripts, players, std::make_shared<cpn::GalacticStoryLink>()));
    auto log = std::make_shared<SpawnLog>();
    take(scripts.set_immediate_effect_host(std::make_shared<SpawnHost>(log), 8));
    eawr::sim::galactic::CensusObject actor{}; actor.id = 42; actor.original_type = actor.current_type = 7;
    const eawr::sim::InlineExecutor executor;
    expect(take(content->reconcile(scripts, std::vector{actor}, executor)).empty(), "attach a scripted orbital fixture");
    expect(take(content->service(scripts, 1, executor)).diagnostics.empty(), "prepare the attached object script");
    expect(take(content->service(scripts, 2, executor)).diagnostics.empty(), "pump the attached object script");
    expect(log->actions.size() == 2 && log->actions[0].starts_with("galactic.spawn_unit")
        && log->actions[1].starts_with("galactic.story_prevent_ai_usage"),
        "a data-authored object script reaches the placement and AI-usage callbacks");
}

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
    const auto thread_ended = [](const Scripted& run) {
        const auto slots = run.scripts.thread_slots(run.instance);
        return slots && std::none_of(slots.value().begin(), slots.value().end(), [](bool live) { return live; });
    };
    expect(!prevent.first.diagnostics.empty() && prevent.scripts.retired_instances().empty() && thread_ended(prevent)
        && prevent.first.diagnostics.front().message.find("thread ") == 0
        && std::none_of(prevent_log->actions.begin(), prevent_log->actions.end(), [](const auto& row) { return row.rfind("galactic.story_prevent", 0) == 0; }),
        "Prevent_AI_Usage without a Boolean is a script error and changes nothing");

    // Without the ordered native shadow (no AI runtime) creation faults by name instead of pretending.
    auto bare = run_spawn(root, "spawn_no_host", good, nullptr);
    expect(!bare.first.diagnostics.empty() && bare.scripts.retired_instances().empty() && thread_ended(bare)
        && bare.first.diagnostics.front().message.find("native shadow") != std::string::npos,
        "Spawn_Unit without a native shadow host is a script error naming the missing host: its thread ends before anything is created");
}
} // namespace

int case_galactic_story_script_host_tests(int argc, char** argv) {
    try {
        engine_requests_callbacks();
        const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-script-host");
        object_modules(root / "object-modules");
        host(root);
        pending_call_checkpoint(root);
        scoring_definitions(root / "scoring");
        scoring_restore(root / "scoring-restore");
        distribution_draw_bits(root / "random");
        distribution_contract(root / "random");
        debug_sinks(root / "random");
        random_streams(root / "random");
        distribution_state(root / "random");
        player_lookup(root / "players");
        game_mode_and_human(root / "human-mode");
        planet_lookup(root / "planets");
        live_object_and_difficulty(root / "objects");
        advisor_hints_stub(root / "hints");
        spawn_bindings(root / "spawn");
        object_binding_primitives();
        object_perception_unavailable();
        object_sale_bindings();
        object_registration_order(root);
        attached_object_calls_native(root / "attached-object");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

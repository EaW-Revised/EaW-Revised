// #5891: bounded script coverage, deliberately independent of goal desirability.
#include "../../src/script/foc/tactical_ai_internal.hpp"
#include "../../src/script/foc/tactical_ai_world.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace ai = foc::ai;
namespace td = foc::tactical_ai_detail;
namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;

template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(result).value();
}

// Existing engine contract access: all forced admission and retained fixture
// objects live here, in the test. Runtime selection and scheduling are untouched.
namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static void seed(Engine& engine, const std::vector<std::vector<sim::EntityId>>& members) {
        PlayerAi owner; owner.player = 1;
        Plan plan; plan.id = 1; plan.instance = first_plan_instance; plan.player = 1; plan.target = 1;
        for (std::size_t i = 0; i < engine.plans_[0].taskforces.size(); ++i) {
            TaskForce force; force.id = i + 1; force.plan = 1; force.definition = i;
            force.name = engine.plans_[0].taskforces[i].name;
            force.units = members[i]; force.had_units = !force.units.empty();
            // Already assembled: Produce_Force uses the normal empty build list.
            plan.taskforces.push_back(force.id);
            owner.taskforces.emplace(force.id, std::move(force));
        }
        owner.plan_of_instance.emplace(plan.instance, plan.id);
        owner.running.emplace(plan.id, std::move(plan));
        engine.players_.push_back(std::move(owner));
        Target target; target.id = 1; target.object = 1; target.name = "AUDIT_TARGET";
        engine.targets_.emplace(target.id, std::move(target));
        engine.rebuild_runtime_index();
    }
    static void barrier(Engine& engine, auth::ScriptScheduler& scripts, std::uint64_t tick) {
        engine.scripts_ = &scripts; engine.frame_ = static_cast<std::int64_t>(tick);
        engine.event_tick_ = tick;
        engine.service_blocks();
        for (auto& event : engine.events_) {
            event.key.tick = tick; event.key.sequence = engine.sequence_++;
            take(scripts.submit_event(std::move(event)));
        }
        engine.events_.clear();
    }
};
}

namespace {
struct Counter final : auth::MissingCallSink {
    std::map<std::string, std::size_t> calls;
    std::set<std::string> script_names;
    void scan(const std::string& source) {
        static const std::regex names(R"(\b(?:function\s+([A-Za-z_][A-Za-z_0-9]*)|([A-Za-z_][A-Za-z_0-9]*)\s*=(?!=)))");
        for (std::sregex_iterator it(source.begin(), source.end(), names), end; it != end; ++it)
            script_names.insert((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());
    }
    bool stub_global(std::string_view name) const override { return !script_names.contains(std::string(name)); }
    void called(std::uint64_t, std::string_view name) override { ++calls[std::string(name)]; }
};
struct Row {
    std::string path, state{"not-started"};
    std::map<std::string, std::size_t> missing, orders;
    std::vector<std::string> errors;
};
constexpr std::size_t zero_missing_floor = 46; // Measured on the stock land plan audit.
constexpr std::uint64_t observation_ticks = 420;

void event(auth::ScriptScheduler& scripts, std::uint64_t tick, std::uint64_t sequence,
    auth::ScriptEvent::Kind kind, std::string name, std::optional<auth::Value> value = {}) {
    auth::ScriptEvent out;
    // Engine callbacks keep their own sequence counter. A distinct producer
    // keeps fixture assignments/pumps from colliding with those event keys.
    out.key = {tick, foc::producer_foc_engine + 1, ai::first_plan_instance, sequence};
    out.target = ai::first_plan_instance; out.kind = kind; out.name = std::move(name); out.parameter = std::move(value);
    take(scripts.submit_event(std::move(out)));
}
void diagnostics(Row& row, const auth::ServiceReport& report) {
    for (const auto& error : report.diagnostics) row.errors.push_back(error.code + " " + error.message);
}

// This witness catches silent lookup-only counting and both userdata routes.
void counter_contract() {
    const auto run = [](Counter* counter, std::string source) {
        auth::SessionConfig config; config.missing_call_sink = counter;
        auth::ModuleManifest manifest; take(manifest.add("probe.lua", std::move(source)));
        auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
        take(scripts.register_binding("Known.Member", [](auto&, const auto&) {
            return eawr::core::Result<auth::ValueList>::success({});
        }));
        take(scripts.register_binding("GetHandle", [](auto&, const auto&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{42, 1}}});
        }));
        take(scripts.create_instance(1, "probe.lua"));
    };
    Counter counter;
    counter.scan("V = nil; function OptionalHook() end; OptionalHook = nil");
    run(&counter, "local probe = NotCalled; MissingGlobal(); Known.MissingMember(); GetHandle().MissingMethod()");
    run(&counter, "V = nil; if V then error('nil variable was stubbed') end; function OptionalHook() end; OptionalHook = nil; if OptionalHook then OptionalHook() end");
    if (counter.calls != std::map<std::string, std::size_t>{{"MissingGlobal", 1}, {"Known.MissingMember", 1}, {"handle[42].MissingMethod", 1}})
        throw std::runtime_error("missing-call instrumentation contract failed");
    run(nullptr, "assert(MissingGlobal == nil); assert(Known.MissingMember == nil)");
}

Row audit(const std::string& path, const foc::AiSetup& setup, const std::map<std::string, std::string>& modules,
    const std::set<std::string>& script_names) {
    Row row; row.path = path;
    Counter counter;
    counter.script_names = script_names;
    auto host = std::make_shared<foc::detail::Host>(); host->setup = setup;
    for (const auto& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
    }
    for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
    host->view = std::make_shared<foc::detail::WorldView>();
    auth::ModuleManifest manifest;
    for (const auto& [name, bytes] : modules) take(manifest.add(name, bytes));
    // Capture the definition through a binding: read_global only supports scalars,
    // while TaskForce is a nested Lua table. Use rawget for optional fields so
    // audit missing-call stubs do not become definition values.
    const std::string loader = "require(\"" + path.substr(0, path.rfind('.')) + "\")\n"
        "PlanDefinitionLoad = true\nBase_Definitions()\n"
        "_AuditPlan(rawget(_G, 'Category'), rawget(_G, 'TaskForce'), rawget(_G, 'IgnoreTarget'), "
        "rawget(_G, 'MagicPlan'), rawget(_G, 'AllowFreeStoreUnits'), rawget(_G, 'AllowEngagedUnits'), "
        "rawget(_G, 'PerFailureContrastAdjust'), rawget(_G, 'MinContrastScale'), "
        "rawget(_G, 'MaxContrastScale'), rawget(_G, 'RequiredCategories'))\n";
    take(manifest.add("EAWR/AUDIT_LOADER.LUA", loader));
    auth::SessionConfig config; config.seed = 5891; config.missing_call_sink = &counter;
    config.tick_duration = {tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/AI/LandMode/"};
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
    std::vector<eawr::core::Diagnostic> binding_errors;
    td::register_globals(scripts, host, binding_errors); td::register_methods(scripts, host, binding_errors);
    auth::ValueList globals;
    bool captured = false;
    take(scripts.register_binding("_AuditPlan", [&](auto&, const auth::ValueList& values) {
        globals = values; captured = true;
        return eawr::core::Result<auth::ValueList>::success({});
    }));
    if (!binding_errors.empty()) throw std::runtime_error(binding_errors.front().message);
    const auto finish = [&] { row.missing = counter.calls; return row; };
    auto created = scripts.create_instance(ai::first_plan_instance, "EAWR/AUDIT_LOADER.LUA");
    if (!created) {
        row.state = "load-error"; row.errors.push_back(created.error().code + " " + created.error().message); return finish();
    }
    const eawr::sim::InlineExecutor executor;
    auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
    if (!captured) { row.state = "definition-error"; row.errors.push_back("definition capture was not called"); return finish(); }
    std::vector<std::string> notes;
    auto parsed = ai::build_plan(*host, path, path, globals, notes);
    if (!parsed) {
        row.state = "definition-error"; row.errors.push_back(parsed.error().code + " " + parsed.error().message); return finish();
    }
    auto definition = std::move(parsed).value();
    // Synthetic, already assembled forces from each authored team's first candidate.
    // This bypasses staffing and desirability so every plan receives one start.
    // Passive object/economy storage, never stepped. The land simulation does
    // not expose an economy yet; the host's LAND mode selects scripts/bindings.
    // Using the generic container keeps the fixture's credits/cap observable.
    tactical::TacticalSetup world_setup;
    world_setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    const auto add_unit = [&](tactical::TypeId type, tactical::PlayerId owner) {
        tactical::UnitState unit; unit.entity_id = world_setup.units.size() + 1;
        unit.type_id = type; unit.owner = owner; world_setup.units.push_back(unit); return unit.entity_id;
    };
    if (setup.content.types.empty()) throw std::runtime_error("stock land type inventory is empty");
    add_unit(setup.content.types.front().type_id, 2);
    std::vector<std::vector<eawr::sim::EntityId>> members;
    for (const auto& force : definition.taskforces) {
        members.emplace_back();
        for (const auto& team : force.teams) {
            if (team.types.empty()) continue;
            const auto count = std::max<std::int32_t>(1, team.min_count);
            for (std::int32_t i = 0; i < count; ++i) members.back().push_back(add_unit(team.types.front(), 1));
        }
    }
    tactical::TacticalContent content;
    for (const auto id : {1U, 2U}) {
        tactical::EconomyPlayer account; account.player = id; account.credits = Fixed::from_integer(500000).value();
        account.population_cap = 10000; account.ai = true; content.economy.players.push_back(account);
    }
    auto world = take(tactical::TacticalSession::create(world_setup, content));
    host->world = &world; host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot);
    host->ai_world = std::make_shared<ai::TacticalWorld>(*host);
    ai::Engine engine(host, {}, {definition}); host->engine = &engine;
    ai::EngineContractAccess::seed(engine, members);
    // The definition loader runs at instance creation, without advancing the
    // scheduler. Advance tick 1 before submitting and inspecting tick 2 starts.
    diagnostics(row, take(scripts.service(executor, options)));
    std::uint64_t sequence = 0;
    event(scripts, 2, sequence++, auth::ScriptEvent::Kind::assign, "PlayerObject", auth::Value{auth::Handle{foc::handle_player, 1}});
    event(scripts, 2, sequence++, auth::ScriptEvent::Kind::assign, "Target", auth::Value{auth::Handle{foc::handle_game_object, 1}});
    event(scripts, 2, sequence++, auth::ScriptEvent::Kind::assign, "AITarget", auth::Value{auth::Handle{ai::handle_ai_target, 1}});
    for (std::size_t i = 0; i < definition.taskforces.size(); ++i) {
        event(scripts, 2, sequence++, auth::ScriptEvent::Kind::assign, definition.taskforces[i].name,
            auth::Value{auth::Handle{ai::handle_taskforce, i + 1}});
    }
    for (const auto& force : definition.taskforces)
        event(scripts, 2, sequence++, auth::ScriptEvent::Kind::start_thread, force.name + "_Thread");
    row.state = "running";
    for (std::uint64_t tick = 2; tick <= observation_ticks + 1; ++tick) {
        ai::EngineContractAccess::barrier(engine, scripts, tick);
        event(scripts, tick, sequence++, auth::ScriptEvent::Kind::pump, "");
        auto report = take(scripts.service(executor, options));
        diagnostics(row, report);
        for (const auto& command : report.commands) {
            std::string name = command.verb;
            if (name == ai::verb_ai && !command.arguments.empty())
                if (const auto* operation = std::get_if<std::string>(&command.arguments.front().data)) name += ":" + *operation;
            ++row.orders[name];
        }
        take(engine.after_service(report));
        if (!report.removed_instances.empty()) { row.state = "removed"; break; }
        const auto slots = scripts.thread_slots(ai::first_plan_instance);
        if (!slots) { row.state = "exited"; break; }
        if (std::none_of(slots.value().begin(), slots.value().end(), [](bool active) { return active; })) {
            row.state = "threads-ended"; break;
        }
    }
    if (const auto* plan = engine.plan(1); plan && plan->exited) row.state = plan->result ? "succeeded" : "failed";
    return finish();
}

int run(const std::filesystem::path& report_path) {
    counter_contract();
    const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the stock land plan audit\n"; return 0; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"foc", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    auto files = take(eawr::vfs::Vfs::mount(mounts));
    auto catalog = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    eawr::units::LoadInput input; input.domain = tactical::BattleDomain::land;
    input.catalog = &catalog.catalog; input.filesystem = &files;
    for (const auto& definition : catalog.catalog.definitions())
        if (definition.namespace_winner && definition.category == eawr::data::Category::game_object) input.types.push_back(definition.id);
    auto tables = take(eawr::units::load_unit_tables(input));
    auto setup = eawr::skirmish::ai_setup({}, {}, tables);
    setup.domain = tactical::BattleDomain::land; setup.seed = 5891;
    setup.players = {{1, "EMPIRE", false, true, ""}, {2, "REBEL", false, true, ""}};
    take(eawr::skirmish::enable_goal_system(files, setup));
    auto modules = take(eawr::skirmish::ai_modules(files, setup, true));
    auto paths = foc::selected_plans(modules, setup.domain);
    // The shared inventory also admits story/intervention roots. This audit's
    // cohort is the mode's land plan directory; no stock filenames are pinned.
    std::erase_if(paths, [](const auto& path) { return !foc::detail::upper(path).starts_with("DATA/SCRIPTS/AI/LANDMODE/"); });
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) throw std::runtime_error("stock land plan inventory is empty");
    Counter inventory;
    for (const auto& [name, source] : modules) { (void)name; inventory.scan(source); }
    std::vector<Row> rows;
    for (const auto& path : paths) rows.push_back(audit(path, setup, modules, inventory.script_names));
    std::map<std::string, std::size_t> counts;
    std::size_t zero{}, inert{}, definition_errors{};
    for (const auto& row : rows) {
        zero += row.missing.empty(); inert += row.orders.empty();
        definition_errors += row.state == "definition-error" || row.state == "load-error";
        for (const auto& [name, calls] : row.missing) { (void)calls; ++counts[name]; }
    }
    std::vector<std::pair<std::string, std::size_t>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::ostringstream text;
    text << "plans_with_zero_missing_calls / total: " << zero << " / " << rows.size() << '\n'
         << "inert_plans: " << inert << " / " << rows.size() << '\n'
         << "observation_ticks: " << observation_ticks << "\nmissing calls ranked by plan count:\n";
    for (const auto& [name, count] : ranked) text << count << '\t' << name << '\n';
    text << "plans (sorted):\n";
    for (const auto& row : rows) {
        text << row.path << "\tstate=" << row.state << "\tinert=" << row.orders.empty() << '\n';
        for (const auto& [name, count] : row.orders) text << "  order\t" << name << '\t' << count << '\n';
        for (const auto& [name, count] : row.missing) text << "  missing\t" << name << '\t' << count << '\n';
        for (const auto& error : row.errors) text << "  script-error\t" << error << '\n';
    }
    std::cout << text.str();
    std::ofstream output(report_path); output << text.str(); output.close();
    if (!output) throw std::runtime_error("cannot write audit report under build tree");
    if (definition_errors > rows.size() / 2) throw std::runtime_error("most plans failed before starting");
    if (inert >= rows.size() - rows.size() / 10)
        throw std::runtime_error("nearly all plans ended without acting");
    if (zero_missing_floor > zero) throw std::runtime_error("zero-missing plan coverage fell below the measured floor");
    return 0;
}
}

int main(int argc, char** argv) {
    try { return run(argc > 1 ? argv[1] : "land-ai-plan-audit.txt"); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

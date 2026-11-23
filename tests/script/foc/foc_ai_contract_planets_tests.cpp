// #7977 battle Lua hosts, first family: FindPlanet, FindPlanet.Get_All_Planets and DiscreteDistribution.Create in the battle
// script host. FindPlanet answers the first battle object of the named type (nil when there is none), Get_All_Planets the
// campaign's planets (empty in a skirmish), DiscreteDistribution is the one weighted list the galactic story host also binds.

#include "foc_ai_contract_fixtures.hpp"
#include "discrete_distribution.hpp"
#include "tactical_ai_internal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace foc_contract {

namespace {

struct PlanetRun {
    bool loaded{};
    std::vector<std::string> reports;  // one text per Report(...) call, in order
    std::vector<std::string> codes;
    std::vector<std::string> messages;
};

std::string describe(const auth::Value& value) {
    if (std::holds_alternative<std::monostate>(value.data)) return "nil";
    if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? "true" : "false";
    if (const auto* text = std::get_if<std::string>(&value.data)) return "s:" + *text;
    if (const auto* number = std::get_if<eawr::script::numeric::LuaNumber>(&value.data))
        return "n:" + std::to_string(static_cast<long long>(*number));
    if (const auto* handle = std::get_if<auth::Handle>(&value.data))
        return "h" + std::to_string(handle->kind) + ":" + std::to_string(handle->id);
    if (const auto* list = std::get_if<std::vector<auth::Value>>(&value.data)) return "list" + std::to_string(list->size());
    return "other";
}

PlanetRun planet_run(const std::string& body, const std::vector<std::uint64_t>& planets, bool galactic = false) {
    PlanetRun out;
    auto host = std::make_shared<foc::detail::Host>();
    auto view = std::make_shared<foc::detail::WorldView>();
    foc::detail::ViewUnit unit;
    unit.id = 10; unit.owner = 1; unit.type = 100;
    view->units.push_back(unit);
    view->players = {{1, 1, 0}};
    host->view = view;
    if (galactic) host->ai_world = std::make_shared<ModeWorld>();
    foc::AiType kuat;
    kuat.type_id = 100;
    host->types_by_name.emplace("KUAT", &kuat);
    host->setup.galactic_planets = planets;
    auth::ModuleManifest manifest;
    if (!manifest.add("PLANETS.LUA", "function R(v) if v == nil then Report(\"nil\") else Report(v) end end\nfunction Run()\n" + body + "\nend\n").has_value()) return out;
    if (!manifest.add(std::string(foc::distribution::distribution_module), std::string(foc::distribution::distribution_source)).has_value())
        return out;
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) return out;
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
    foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
    foc::tactical_ai_detail::register_battle_object_bindings(scripts.value(), host, errors);
    foc::tactical_ai_detail::register_battle_distribution(scripts.value(), host, errors);
    if (!scripts.value().register_binding("Report", [&out](auth::BindingContext&, const auth::ValueList& args) -> eawr::core::Result<auth::ValueList> {
            std::string text;
            for (const auto& arg : args) text += (text.empty() ? "" : ",") + describe(arg);
            out.reports.push_back(text);
            return eawr::core::Result<auth::ValueList>::success({});
        }).has_value()) errors.push_back({});
    if (!errors.empty() || !scripts.value().create_instance(1, "PLANETS.LUA").has_value()) return out;
    auth::ScriptEvent event;
    event.key = {1, auth::first_simulation_producer, 1, 0};
    event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Run";
    if (!scripts.value().submit_event(std::move(event)).has_value()) return out;
    const auto report = scripts.value().service(eawr::sim::InlineExecutor{});
    if (!report) return out;
    out.loaded = true;
    for (const auto& diagnostic : report.value().diagnostics) {
        out.codes.push_back(diagnostic.code);
        out.messages.push_back(diagnostic.message);
    }
    return out;
}

} // namespace

void battle_planet_family_contracts() {
    // FindPlanet: the first battle object of the named type; nil for an unknown name or a type with no object here.
    auto run = planet_run("R(FindPlanet('Kuat')) R(FindPlanet('kuat')) R(FindPlanet('Nowhere')) R(FindPlanet('Kuat', 7))", {});
    expect(run.loaded && run.codes.empty()
            && run.reports == std::vector<std::string>{"h1:10", "h1:10", "s:nil", "h1:10"},
        "family 1: FindPlanet answers the first battle object of the type, case-insensitive, nil when unknown, extra arguments ignored");
    // A missing or non-string argument is a script error that ends the thread.
    for (const char* call : {"FindPlanet()", "FindPlanet(3)", "FindPlanet(nil)"}) {
        run = planet_run(std::string(call) + " Report('after')", {});
        expect(run.loaded && run.reports.empty() && !run.codes.empty(), std::string("family 1: ") + call + " is a script error");
    }
    // Get_All_Planets: the campaign's planets in ascending order as galactic-planet handles; empty in a skirmish.
    run = planet_run("local all = FindPlanet.Get_All_Planets() Report(all) Report(all[1], all[3])", {3, 5, 9});
    expect(run.loaded && run.codes.empty() && run.reports == std::vector<std::string>{"list3", "h10:3,h10:9"},
        "family 1: Get_All_Planets lists the campaign planets as planet handles, in order");
    run = planet_run("local all = FindPlanet.Get_All_Planets() Report(all) Report(table.getn(all))", {});
    expect(run.loaded && run.codes.empty() && run.reports == std::vector<std::string>{"list0", "n:0"},
        "family 1: Get_All_Planets is an empty list in a skirmish");

    // DiscreteDistribution.Create: Insert(value, weight) and Sample() with the galactic host's definition.
    run = planet_run(
        "local d = DiscreteDistribution.Create() R(d.Sample())"
        " d.Insert('a', 1) d.Insert('z', 0) d.Insert('n', -2) Report(d.Sample(), d.Sample())"
        " local e = DiscreteDistribution.Create(1, 2) e.Insert('x', 1) Report(e.Sample())"
        " local c = DiscreteDistribution.Create() Report((pcall(c.Insert, 'v'))) Report((pcall(c.Insert, 'v', 'w'))) R(c.Sample())"
        " local w = DiscreteDistribution.Create() w.Insert('lo', 1) w.Insert('hi', 7) local hi = 0"
        " for i = 1, 200 do if w.Sample() == 'hi' then hi = hi + 1 end end Report(hi > 100 and hi < 200)",
        {});
    expect(run.loaded && run.codes.empty()
            && run.reports == std::vector<std::string>{"s:nil", "s:a,s:a", "s:x", "false", "false", "s:nil", "true"},
        "family 2: DiscreteDistribution.Create samples by weight, stores only positive weights, rejects bad Insert calls");
    run = planet_run("Report(DiscreteDistribution == nil, FindPlanet == nil)", {}, true);
    expect(run.loaded && run.codes.empty() && run.reports == std::vector<std::string>{"true,true"},
        "galactic goal mode does not expose battle DiscreteDistribution or FindPlanet");
    // The same seed gives the same sequence.
    const std::string draw = "local d = DiscreteDistribution.Create() d.Insert(1, 1) d.Insert(2, 1) d.Insert(3, 1)"
        " for i = 1, 24 do R(d.Sample()) end";
    const auto first = planet_run(draw, {});
    const auto second = planet_run(draw, {});
    expect(first.loaded && first.reports.size() == 24 && first.reports == second.reports,
        "family 2: DiscreteDistribution draws repeat for a repeated seed");
}

} // namespace foc_contract

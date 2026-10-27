// Generic Script_* perception evaluators (#1702, WGA-453, RDP-E09/E10): the synthetic evaluator
// modules below are written for this test; the host knows no module name. They cover a numeric
// result, a non-numeric result, an error, state kept across calls, the cleanup after success and
// failure, the bound player/target, the helper calls a distance evaluator reaches, a module that
// does not load, and the goal pass: a numeric evaluator proposes, a failing one is rejected.

#include "foc_ai_contract_fixtures.hpp"
#include "ai_script_evaluators.hpp"

#include <cctype>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace foc_contract {

namespace {

using eawr::script::foc::tactical_ai_detail::ScriptEvaluators;

std::map<std::string, std::string> corpus() {
    const std::string base = "require(\"PGBaseDefinitions\")\n";
    return {
        // The library chain the evaluators require: the cleanup entry point the engine calls.
        {"Data/Scripts/Library/PGBaseDefinitions.lua",
            "function Evaluator_Clean_Up()\n  Target = nil\n  PlayerObject = nil\n  if Clean_Up then Clean_Up() end\nend\n"},
        {"Data/Scripts/Evaluators/Sum.lua", base + "function Evaluate(a, b) return a + b end\n"},
        {"Data/Scripts/Evaluators/Words.lua", base + "function Evaluate(word) return word end\n"},
        {"Data/Scripts/Evaluators/Boom.lua", base + "function Evaluate() error('boom') end\n"},
        {"Data/Scripts/Evaluators/Echo.lua", base +
            "function Evaluate(v)\n  if type(v) == 'number' then return 1 end\n  return 2\nend\n"},
        {"Data/Scripts/Evaluators/Zero.lua", base + "function Evaluate() return 0 end\n"},
        {"Data/Scripts/Evaluators/Counter.lua", base +
            "calls = 0\ncleaned = 0\n"
            "function Evaluate(tag)\n  if tag == 'FAIL' or tag == 'fail' then error('counter failure') end\n  calls = calls + 1\n  return calls * 100 + cleaned\nend\n"
            "function Clean_Up() cleaned = cleaned + 1 end\n"},
        {"Data/Scripts/Evaluators/Bound.lua", base +
            "function Evaluate() local flag = 0\n  if Target then flag = 1 end\n  return PlayerObject.Get_ID() * 1000 + flag\nend\n"},
        {"Data/Scripts/Evaluators/Reach.lua", base +
            "function Evaluate()\n  local station = PlayerObject.Get_Space_Station()\n  return Target.Get_Distance(station.Get_Position())\nend\n"},
        {"Data/Scripts/Evaluators/Broken.lua", "this is not lua (\n"},
        // Not directly under the evaluator directory, and not Lua: never evaluators.
        {"Data/Scripts/Evaluators/Sub/Deep.lua", "function Evaluate() return 1 end\n"},
        {"Data/Scripts/Evaluators/Notes.txt", "not a module\n"},
    };
}

constexpr char equations_xml[] =
    "<Root>"
    "<SumEq>Script_Sum.Evaluate{Parameter_Script_Number = 2, Parameter_Script_Number = 3}</SumEq>"
    "<WordsEq>Script_Words.Evaluate{Parameter_Script_String = \"hello\"}</WordsEq>"
    "<BoomEq>Script_Boom.Evaluate</BoomEq>"
    "<EchoNum>Script_Echo.Evaluate{Parameter_Script_Number = 1}</EchoNum>"
    "<EchoStr>Script_Echo.Evaluate{Parameter_Script_String = \"1\"}</EchoStr>"
    "<ZeroEq>Script_Zero.Evaluate</ZeroEq>"
    "<CountA>Script_Counter.Evaluate{Parameter_Script_String = \"a\"}</CountA>"
    "<CountB>Script_Counter.Evaluate{Parameter_Script_String = \"b\"}</CountB>"
    "<CountC>Script_Counter.Evaluate{Parameter_Script_String = \"c\"}</CountC>"
    "<CountFail>Script_Counter.Evaluate{Parameter_Script_String = \"fail\"}</CountFail>"
    "<BoundEq>Script_Bound.Evaluate</BoundEq>"
    "<ReachEq>Script_Reach.Evaluate</ReachEq>"
    "<BrokenEq>Script_Broken.Evaluate</BrokenEq>"
    "<MissingEq>Script_Nope.Evaluate</MissingEq>"
    "<DeepEq>Script_Deep.Evaluate</DeepEq>"
    "</Root>";

struct EvaluatorRig {
    World world;
    std::unique_ptr<ai::Engine> engine;
    std::shared_ptr<ScriptEvaluators> evaluators;

    EvaluatorRig() {
        namespace m = eawr::sim::math;
        foc::AiType station;
        station.type_id = 2; station.name = "STATION"; station.unit_ai = true; station.star_base = true;
        world.host->setup.content.types.push_back(station);
        world.host->types.clear();
        world.host->types_by_name.clear();
        for (const foc::AiType& entry : world.host->setup.content.types) {
            world.host->types.emplace(entry.type_id, &entry);
            world.host->types_by_name.emplace(entry.name, &entry);
        }
        const auto fixed = [](int value) { return m::Fixed::from_integer(value).value(); };
        ai::ViewUnit home;
        home.id = 20; home.type = 2; home.owner = 2; home.visible_to = 3;
        home.position = {fixed(30), fixed(40), fixed(0)};
        world.view->units.push_back(home);
        ai::AiData data;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto equations = ai::EquationSet::parse({{"evaluators.xml", equations_xml}}, converters);
        expect(equations.has_value(), "WGA-453: the evaluator equations parse");
        if (equations) data.equations = std::move(equations).value();
        engine = std::make_unique<ai::Engine>(world.host, std::move(data), std::vector<ai::PlanDef>{});
        world.host->engine = engine.get();
        auto made = ScriptEvaluators::create(world.host, corpus());
        expect(made.has_value(), "WGA-453: the evaluator host starts");
        if (made) evaluators = std::move(made).value();
        engine->attach_script_evaluators(evaluators);
        ai::EngineContractAccess::target_fixture(*engine, 1, 10);
    }
    std::optional<Real> eval(const char* equation, const tactical::PlayerId player = 2, const bool with_target = true) {
        return engine->evaluate(equation, player, with_target ? engine->target(1) : nullptr);
    }
};

// One run of the whole scripted sequence; the results must not depend on anything but the inputs.
std::vector<std::optional<Real>> sequence() {
    EvaluatorRig rig;
    std::vector<std::optional<Real>> out;
    for (const char* name : {"SumEq", "WordsEq", "BoomEq", "ZeroEq", "CountA", "CountB", "CountFail", "CountC",
             "BoundEq", "ReachEq", "BrokenEq", "MissingEq", "DeepEq"}) {
        out.push_back(rig.eval(name));
    }
    out.push_back(rig.eval("ReachEq", 1));
    out.push_back(rig.eval("BoundEq", 1, false));
    return out;
}

void numeric_and_failures() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    const auto names = rig.evaluators->names();
    expect(names == std::vector<std::string>({"BOOM", "BOUND", "BROKEN", "COUNTER", "ECHO", "REACH", "SUM", "WORDS", "ZERO"}),
        "WGA-453: the catalogue is the Lua modules directly under the evaluator directory");
    expect(rig.evaluators->known("SUM") && rig.evaluators->known("BROKEN") && !rig.evaluators->known("DEEP")
        && !rig.evaluators->known("NOTES"), "WGA-453: nested files and non-Lua files are not evaluators");
    expect(rig.eval("SumEq") == ai::real(5), "WGA-453: a numeric result succeeds, with the script parameters as arguments");
    expect(!rig.eval("WordsEq").has_value(), "WGA-453: a non-numeric result is a failure, not a value");
    expect(!rig.eval("BoomEq").has_value(), "WGA-453: an error is a failure, not zero");
    expect(rig.eval("ZeroEq") == ai::real(0), "WGA-453: a numeric zero is a result");
    expect(!rig.eval("MissingEq").has_value(), "WGA-453: a module the corpus does not supply is a failure");
    expect(!rig.eval("DeepEq").has_value(), "WGA-453: a module outside the evaluator directory is a failure");
    expect(!rig.eval("BrokenEq").has_value() && !rig.eval("BrokenEq").has_value(),
        "WGA-453: a module that does not load fails every lookup");
    expect(rig.eval("SumEq") == ai::real(5), "WGA-453: one broken module leaves the others working");
}

void retained_state_and_cleanup() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    expect(rig.eval("CountA") == ai::real(100), "WGA-453: the first call runs on the loaded script state");
    expect(rig.eval("CountB") == ai::real(201), "WGA-453: script state and the cleanup of the previous call persist");
    const auto before = rig.evaluators->stats().calls;
    expect(rig.eval("CountA") == ai::real(100) && rig.evaluators->stats().memo_hits == 1
        && rig.evaluators->stats().calls == before + 1,
        "perf: an identical lookup on one completed view is answered once (memo), not re-run");
    expect(!rig.eval("CountFail").has_value(), "WGA-453: the failing call has no value");
    expect(rig.eval("CountC") == ai::real(303),
        "WGA-453: the cleanup also runs after a failed Evaluate (Evaluate ran for a, b, c; Evaluator_Clean_Up ran after a, b and the failed call: 3*100+3)");
}

void memo_key_is_typed() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    expect(rig.eval("EchoNum") == ai::real(1), "perf: a number parameter reaches Lua as a number");
    expect(rig.eval("EchoStr") == ai::real(2) && rig.evaluators->stats().memo_hits == 0,
        "perf: number 1 and string \"1\" are different memo keys; the second lookup runs Evaluate");
}

void bound_context_and_helpers() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    expect(rig.eval("BoundEq", 2) == ai::real(2001), "WGA-453: PlayerObject is the context player and Target the context target");
    expect(rig.eval("BoundEq", 1, false) == ai::real(1000), "WGA-453: without a context target Target is nil");
    expect(rig.eval("ReachEq", 2) == ai::real(50),
        "helpers: Get_Space_Station, Get_Position and Target.Get_Distance(position) measure the station distance");
    expect(!rig.eval("ReachEq", 1).has_value(),
        "WGA-453: a player without a station fails the distance lookup instead of answering zero");
}

void serial_only() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    rig.world.host->script_service_active.store(true);
    expect(!rig.eval("SumEq").has_value(), "perf: evaluators refuse inside the partitioned script service");
    rig.world.host->script_service_active.store(false);
    expect(rig.eval("SumEq") == ai::real(5), "perf: evaluators answer again in the serial AI turn");
}

// The goal pass: a numeric evaluator proposes; a failing one is a rejection that names its lookup.
void goal_pass() {
    EvaluatorRig rig;
    if (!rig.evaluators) return;
    const int before = failures;
    auto journal = std::make_shared<foc::AiJournal>();
    rig.world.host->setup.journal = journal;
    rig.world.host->setup.trace_proposals = true;
    ai::GoalType goal;
    goal.application = {"FRIENDLY_UNIT"};
    auto& player = ai::EngineContractAccess::add_player(*rig.engine, 2);
    player.player = 2;
    player.per_frame = 8;
    player.targets = {1};
    for (const char* name : {"SumEq", "BoomEq", "ZeroEq", "WordsEq"}) {
        ai::GoalFunctionEntry function;
        function.goal = &goal;
        function.goal_name = "GOAL_";
        for (const char* letter = name; *letter != 0; ++letter)
            function.goal_name += static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)));
        function.equation = rig.engine->data().equations.find(name);
        function.equation_name = name;
        player.functions.push_back(std::move(function));
    }
    for (int frame = 0; frame < 10 && !player.maintenance_due; ++frame) ai::EngineContractAccess::propose(*rig.engine, player);
    const auto find = [&](const std::string& goal_name, const std::string& event) -> const foc::AiProposalSummary* {
        for (const auto& [key, row] : journal->proposals) {
            static_cast<void>(key);
            if (row.goal == goal_name && row.event == event) return &row;
        }
        return nullptr;
    };
    expect(find("GOAL_SUMEQ", "proposed") != nullptr, "ai_goals: a numeric positive evaluator proposes the goal");
    const auto* boom = find("GOAL_BOOMEQ", "rejected");
    expect(boom != nullptr && boom->reason.starts_with("authored perception unavailable")
        && boom->reason.find("SCRIPT_BOOM.EVALUATE") != std::string::npos,
        "ai_goals: an erroring evaluator rejects the goal and the trace names the lookup");
    expect(find("GOAL_BOOMEQ", "proposed") == nullptr && find("GOAL_WORDSEQ", "proposed") == nullptr,
        "ai_goals: failure and a non-numeric result are never proposals (never zero desire)");
    const auto* words = find("GOAL_WORDSEQ", "rejected");
    expect(words != nullptr && words->reason.starts_with("authored perception unavailable"),
        "ai_goals: a non-numeric result is an unavailable perception");
    const auto* zero = find("GOAL_ZEROEQ", "rejected");
    expect(zero != nullptr && zero->reason == "nonpositive authored desire",
        "ai_goals: a numeric zero is a result, rejected as a nonpositive desire rather than as unavailable");
    if (failures != before) {
        std::cerr << "ai_goals journal rows: " << journal->proposals.size() << '\n';
        for (const auto& [key, row] : journal->proposals) std::cerr << "  " << row.goal << " | " << row.event << " | " << row.reason << '\n';
    }
}

} // namespace

void script_evaluator_contracts() {
    numeric_and_failures();
    retained_state_and_cleanup();
    memo_key_is_typed();
    bound_context_and_helpers();
    serial_only();
    goal_pass();
    // Equal inputs, equal results: two independent hosts agree call by call.
    const auto first = sequence();
    const auto second = sequence();
    expect(first == second, "WGA-453: independent evaluator hosts agree on every result");
    std::cout << "Script evaluator contracts: " << failures << " failures\n";
}

} // namespace foc_contract

// GameObject.Build on a build pad (#1702, EXP-E01) and the negative Give_Money the mods pay for it
// with (WGA-100). Every overload and refusal, and the money: Build itself never debits (the AI's
// pad execution has its debit flag false, WBP-09), so one started build costs exactly the
// mod's own adjustment (or the TaskForce reservation, WBP-47/48), never both.

#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_internal.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace foc_contract {

namespace {

namespace t = eawr::sim::tactical;
namespace m = eawr::sim::math;

m::Fixed q(const std::int64_t value) { return m::Fixed::from_integer(value).value(); }

constexpr t::TypeId pad_type = 20, unit_type = 21, uc_menu = 30, uc_other = 31, mine = 40, mine_other = 41;

// One AI player (1) owning pad 1 and a plain ship 2. Its faction menu offers UC 30 only.
struct BuildRig {
    std::optional<t::TacticalSession> session;
    std::shared_ptr<ai::Host> host;
    std::optional<auth::ScriptScheduler> scripts;
    eawr::platform::ThreadWorkerAdapter executor{1};
    std::uint64_t sequence{};

    BuildRig() {
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {3, 3, 300, 0}};
        start.units = {{1, pad_type, 1}, {2, unit_type, 1}};
        start.units[1].position.x = q(500);
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20, true}, {3, q(1000), 20}};
        rules.menus = {{pad_type, 100, {{uc_menu, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 3;
        rules.pads.capture = {{pad_type, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{uc_menu, mine, q(100), 1, 1}, {uc_other, mine_other, q(100), 1, 1}};
        rules.pads.influence = {{pad_type, false}, {uc_menu, false}, {mine, false}};
        t::DurabilityTable health;
        for (const t::TypeId type : {pad_type, unit_type, uc_menu, uc_other, mine, mine_other}) {
            t::DurabilityProfile profile; profile.type_id = type; profile.max_hull = q(300);
            health.profiles.push_back(profile);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(made.has_value(), "EXP-E01: the build-pad fixture starts");
        if (!made) return;
        session.emplace(std::move(made).value());
        host = ai::make_tactical_host();
        host->world = &*session;
        for (const auto& [type, name] : {std::pair{pad_type, "PAD"}, {unit_type, "SHIP"}, {uc_menu, "UC_E"}, {uc_other, "UC_X"},
                {mine, "MINE"}, {mine_other, "MINE_X"}}) {
            foc::AiType row; row.type_id = type; row.name = name; host->setup.content.types.push_back(row);
        }
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        host->setup.players = {{1, "EMPIRE", false, true, ""}, {3, "NEUTRAL", true, false, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        refresh();
    }
    void refresh() {
        host->snapshot = session->snapshot();
        host->view = foc::detail::build_view(*session, *host->snapshot, &executor);
        host->clear_barrier_state();
    }
    void step() {
        expect(session->step(executor).has_value(), "EXP-E01: the world steps");
        refresh();
    }
    // Runs `Check(pad, ship, player)` of a fresh script with the globals this class registers.
    std::optional<auth::ServiceReport> run(const std::string& body) {
        auth::ModuleManifest manifest;
        expect(manifest.add("BUILD.LUA", "function Check(pad, ship, player)\n" + body + "\nend\n").has_value(), "EXP-E01: the script loads");
        auth::SessionConfig config;
        config.tick_duration = auth::TickDuration{1, 30};
        auto made = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
        if (!made) { expect(false, "EXP-E01: the scheduler starts"); return std::nullopt; }
        scripts.emplace(std::move(made).value());
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(*scripts, host, errors);
        foc::tactical_ai_detail::register_methods(*scripts, host, errors);
        if (!errors.empty() || !scripts->create_instance(1, "BUILD.LUA").has_value()) {
            expect(false, "EXP-E01: the bindings register");
            return std::nullopt;
        }
        auth::ScriptEvent event;
        event.key = {scripts->completed_tick() + 1, auth::first_simulation_producer, 1, sequence++};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 1}}, auth::Value{auth::Handle{foc::handle_game_object, 2}},
            auth::Value{auth::Handle{foc::handle_player, 1}}};
        expect(scripts->submit_event(std::move(event)).has_value(), "EXP-E01: the call queues");
        auto report = scripts->service(executor);
        if (!report) { expect(false, "EXP-E01: the script service runs: " + report.error().message); return std::nullopt; }
        return std::move(report).value();
    }
    // "true", "false", "nil" or "other" for a global of the last script.
    std::string kind(const std::string& name) {
        const auto value = scripts->read_global(1, name);
        if (!value || !value.value()) return "nil";
        const auto& data = value.value()->data;
        if (std::holds_alternative<std::monostate>(data)) return "nil";
        if (const bool* flag = std::get_if<bool>(&data)) return *flag ? "true" : "false";
        return "other";
    }
    // Delivers the report's commands to the world through the real translator, then steps once.
    bool deliver(const auth::ServiceReport& report) {
        for (const auto& command : report.commands) {
            auto order = foc::tactical_ai_detail::translate_order(command);
            if (!order) { expect(false, "EXP-E01: the command translates: " + order.error().message); return false; }
            const t::PlayerCommand sent{{session->completed_tick(), order.value().issuer, command.sequence},
                order.value().units, order.value().payload};
            if (!session->submit(sent).has_value()) { expect(false, "EXP-E01: the order submits"); return false; }
        }
        step();
        return true;
    }
    m::Fixed credits() { return session->ledgers().front().credits; }
};

bool refused(const std::optional<auth::ServiceReport>& report) {
    if (!report) return false;
    if (!report->commands.empty()) return false;
    for (const auto& diagnostic : report->diagnostics)
        if (diagnostic.code == auth::codes::script_error) return true;
    return false;
}

void overloads_and_money() {
    // The type by name, in any case: the command is the AI's pad build, no debit follows.
    BuildRig by_name;
    if (!by_name.session) return;
    auto report = by_name.run("result = pad.Build('uc_e')");
    expect(report && report->diagnostics.empty() && by_name.kind("result") == "true" && report->commands.size() == 1
        && report->commands.front().verb == foc::verb_pad_build, "EXP-E01: Build(type name) answers true and starts the pad build");
    if (report && report->commands.size() == 1) {
        const auto order = foc::tactical_ai_detail::translate_order(report->commands.front());
        const auto* payload = order ? std::get_if<t::PadBuildPayload>(&order.value().payload) : nullptr;
        expect(payload && payload->type == uc_menu && order.value().issuer == 1 && order.value().units == std::vector<eawr::sim::EntityId>{1},
            "EXP-E01: the order is the receiver owner's pad build of the named type");
        const auto before = by_name.credits();
        expect(by_name.deliver(*report) && by_name.session->pads().at(1).under_construction != 0,
            "EXP-E01: the existing pad path starts the under-construction child");
        expect(by_name.credits() == before && before == q(1000),
            "EXP-E01/WBP-09: Build itself debits nothing (the AI's pad execution has its debit flag false)");
        // A second Build on the occupied pad is an error and nil.
        auto occupied = by_name.run("result = pad.Build('UC_E')");
        expect(refused(occupied) && by_name.kind("result") == "nil", "EXP-E01: an occupied pad reports an error and answers nil");
        // The mod's own adjustment is then the only charge: -40 (WGA-100), clamped to the balance.
        auto pay = by_name.run("player.Give_Money(-40)");
        expect(pay && pay->diagnostics.empty() && pay->commands.size() == 1 && pay->commands.front().verb == foc::verb_reservation_debit,
            "WGA-100: a negative Give_Money is a debit command, not a Lua error");
        expect(pay && by_name.deliver(*pay) && by_name.credits() == q(960), "WGA-100: the debit leaves exactly the adjustment charged once");
        auto drain = by_name.run("player.Give_Money(-5000)");
        expect(drain && drain->diagnostics.empty() && by_name.deliver(*drain) && by_name.credits() == q(0),
            "WGA-100: a debit has no affordability test and floors the balance at zero");
        auto empty = by_name.run("player.Give_Money(-5)");
        expect(empty && empty->diagnostics.empty() && empty->commands.empty(), "WGA-100: a debit of an empty wallet changes nothing");
        auto gift = by_name.run("player.Give_Money(25)");
        expect(gift && gift->diagnostics.empty() && gift->commands.size() == 1 && gift->commands.front().verb == foc::verb_credit_grant,
            "WGA-100: positive Give_Money keeps the credit grant");
        auto zero = by_name.run("player.Give_Money(0)");
        expect(zero && !zero->diagnostics.empty() && zero->commands.empty(), "WGA-100: a zero amount stays a script error");
    }

    // The type by handle, with the optional Boolean.
    BuildRig by_handle;
    if (!by_handle.session) return;
    auto handle_report = by_handle.run("result = pad.Build(Find_Object_Type('UC_E'))");
    expect(handle_report && by_handle.kind("result") == "true" && handle_report->commands.size() == 1,
        "EXP-E01: Build(type handle) answers true");
    for (const auto& [body, expected, commands, label] : {
             std::tuple<const char*, const char*, std::size_t, const char*>{"result = pad.Build('UC_E', true)", "true", 1U, "the checked form accepts a menu type on a free pad"},
             {"result = pad.Build('UC_X', true)", "false", 0U, "the checked form refuses a type outside the owner's menu with false, not an error"},
             {"result = pad.Build('UC_E', false)", "true", 1U, "the unchecked form starts a menu type"}}) {
        BuildRig rig;
        if (!rig.session) return;
        auto run = rig.run(body);
        expect(run && run->diagnostics.empty() && rig.kind("result") == expected && run->commands.size() == commands,
            std::string("EXP-E01: ") + label);
    }
}

// True means the start happens: a start the pad order would reject is an error and nil, never a silent no-start.
void answers_match_the_simulation() {
    for (const char* body : {"result = pad.Build('UC_X', false)", "result = pad.Build('UC_X')"}) {
        BuildRig rig;
        if (!rig.session) return;
        auto run = rig.run(body);
        expect(refused(run) && rig.kind("result") == "nil" && run->commands.empty(),
            "EXP-E01: a type the simulation would reject (not on the owner's menu) is an error and nil even unchecked");
        expect(rig.deliver(*run) && rig.session->pads().at(1).under_construction == 0, "EXP-E01: nothing started");
    }
    // Two Builds on one free pad in one turn: the first starts, the second sees the pad taken.
    BuildRig twice;
    if (!twice.session) return;
    auto both = twice.run("first = pad.Build('UC_E')\nsecond = pad.Build('UC_E')");
    expect(both && both->commands.size() == 1 && twice.kind("first") == "true" && twice.kind("second") == "nil"
        && !both->diagnostics.empty(), "EXP-E01: a second same-turn Build on the pad is an error and nil (one start, one charge)");
    expect(both && twice.deliver(*both) && twice.session->pads().at(1).under_construction != 0, "EXP-E01: the one start happens");
    // A new turn starts clean: the same pad is occupied by the real child, not by the stale reservation.
    auto later = twice.run("result = pad.Build('UC_E')");
    expect(refused(later) && twice.kind("result") == "nil", "EXP-E01: the next turn sees the started child");
}

void same_turn_debits_floor_at_zero() {
    BuildRig rig;
    if (!rig.session) return;
    auto run = rig.run("player.Give_Money(-800)\nplayer.Give_Money(-800)\nplayer.Give_Money(-800)");
    expect(run && run->diagnostics.empty() && run->commands.size() == 2,
        "WGA-100: the second debit clamps to what the first left, the third finds an empty wallet");
    expect(run && rig.deliver(*run) && rig.credits() == q(0),
        "WGA-100: two same-turn debits floor the balance at zero and the simulation accepts both");
    // A grant earlier in the turn counts: balance 0, +100 then -500 leaves 0, not 100.
    auto grant_then_debit = rig.run("player.Give_Money(100)\nplayer.Give_Money(-500)");
    expect(grant_then_debit && grant_then_debit->diagnostics.empty() && grant_then_debit->commands.size() == 2,
        "WGA-100: the debit clamps to the balance including the grant queued before it");
    expect(grant_then_debit && rig.deliver(*grant_then_debit) && rig.credits() == q(0),
        "WGA-100: grant then debit in one turn floors at zero");
}

void refusals() {
    for (const auto& [body, label] : {
             std::pair<const char*, const char*>{"result = pad.Build()", "no type"},
             {"result = pad.Build('UC_E', true, 1)", "three arguments"},
             {"result = pad.Build('NO_SUCH_TYPE')", "an unknown type name"},
             {"result = pad.Build(5)", "a number for the type"},
             {"result = pad.Build('UC_E', 'yes')", "a non-Boolean flag"},
             {"result = pad.Build('MINE')", "a type without under-construction behavior"},
             {"result = ship.Build('UC_E')", "a receiver without build behavior"}}) {
        BuildRig rig;
        if (!rig.session) return;
        auto run = rig.run(body);
        expect(refused(run) && rig.kind("result") == "nil", std::string("EXP-E01: ") + label + " reports an error and answers nil");
    }
}

} // namespace

void object_build_contracts() {
    overloads_and_money();
    refusals();
    answers_match_the_simulation();
    same_turn_debits_floor_at_zero();
    std::cout << "Object Build contracts: " << failures << " failures\n";
}

} // namespace foc_contract

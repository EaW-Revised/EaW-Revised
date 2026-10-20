#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_markup.hpp"
#include "galactic_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <exception>

namespace {
namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using galactic_test::expect;
using eawr::testing::result_diagnostic::must;

struct RejectPublication final : auth::ImmediateEffectHost {
    std::shared_ptr<auth::ImmediateEffectHost> host;
    bool reject{};
    explicit RejectPublication(std::shared_ptr<auth::ImmediateEffectHost> input) : host(std::move(input)) {}
    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override { return host->capture(); }
    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState> state) override {
        if (reject) {
            eawr::core::Diagnostic diagnostic; diagnostic.code="test-publication-refused"; diagnostic.message="publication refused";
            return eawr::core::Result<void>::failure(std::move(diagnostic));
        }
        return host->publish(std::move(state));
    }
};
void encoded_bytes_contract() {
    ai::GalacticMarkup markup({}, {1}, {1}, {10});
    ai::GalacticMarkupState state;
    state.next_handle = 2;
    state.hints[1] = {"S", {{"H", ai::Real::from_repr(std::uint64_t{1} << 63U)}}, {{"P", {}}}};
    const ai::GalacticMarkupKey key{1, false, 10};
    state.values[key] = ai::real(1);
    state.handles[1] = {ai::real(1), {key, key}};
    must(markup.validate(state));
    markup.replace(std::move(state));
    const std::vector<std::uint8_t> expected{
        0x14, 0x00, 0x00, 0x00, 0x65, 0x61, 0x77, 0x72, 0x2d, 0x67, 0x61, 0x6c, 0x61, 0x63, 0x74, 0x69,
        0x63, 0x2d, 0x6d, 0x61, 0x72, 0x6b, 0x75, 0x70, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x53, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xf0, 0x3f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    expect(markup.encode() == expected, "fixed markup state retains exact little-endian bytes, hints and repeated targets");
}
ai::GalacticPerceptionSetup setup() {
    ai::GalacticPerceptionSetup out;
    out.players = {{1, 1, "One", "Normal", false, true, false, 10},
        {2, 2, "Two", "Normal", false, true, false, 20}, {3, 3, "Human", "Normal", false, false, true, 30}};
    out.planets = {{10, "Alpha", 100, {}, {}, {}, {}, {}}, {20, "Beta", 200, {}, {}, {}, {}, {}}, {30, "Gamma", 300, {}, {}, {}, {}, {}}};
    out.markup_hints[1] = {"CUSTOM", {{"MOD_HINT", ai::real(2)}, {"SIGNED", ai::Real{}}}, {{"ALPHA", {{"MOD_HINT", ai::real(7)}}}}};
    out.cache_intervals["MARKUP"] = ai::real(-1);
    return out;
}
std::shared_ptr<const gc::GalacticSnapshot> snapshot(bool remove = false) {
    std::vector<gc::Planet> planets{{10, {}, 1}, {20, {}, 2}, {30, {}, 3}};
    if (remove) planets.erase(planets.begin() + 1);
    return std::make_shared<gc::GalacticSnapshot>(0, gc::GalacticClock{},
        std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}, {3, 3, 0}}, planets,
        std::vector<gc::Route>{}, std::vector<gc::Fleet>{{40, 1, 10, {}, 0, {}}}, std::vector<gc::GalacticEvent>{});
}
ai::AiData data() {
    auto equations = must(ai::EquationSet::parse({{"fixture.xml", R"(<Equations>
        <Markup>Variable_Target.Markup</Markup><PlayerMarkup>Variable_Self.Markup</PlayerMarkup>
        <EnemyMarkup>Variable_Enemy.Markup</EnemyMarkup>
        <Hint>Variable_Target.Hints.Mod_Hint</Hint><Unknown>Variable_Target.Hints.Unknown</Unknown>
        </Equations>)"}}, [](std::string_view, std::string_view) -> std::optional<ai::Real> { return {}; }));
    ai::AiData out; out.equations = std::move(equations); out.players["QUERY"] = ai::PlayerType{}; return out;
}
std::string lifetime_contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto world = std::make_shared<ai::GalacticWorld>(setup());
    must(world->bind(snapshot(), {}, executor, 0, 1000));
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest manifest;
    must(manifest.add("mod.lua", R"(
        h=nil; key=nil; cycle=nil; get=nil; co=nil; witness=nil; anchor_name=""
        function Hold() h=Make(1.25) end
        function Drop() h=nil end
        function WarningDrop() h=nil; Warn() end
        function Local() local at=Make(0.1) end
        function Many() Make(1e20); Make(1); Make(-1e20) end
        function Anchored() local at={Make(1)}; anchor_name=tostring(at) end
        function Graph()
            local at=Make(2)
            key={[at]=true}; cycle={at}; cycle.self=cycle
            get=function() return at end
        end
        function ClearGraph() key=nil; cycle=nil; get=nil end
        function Coroutine()
            co=coroutine.create(function() local at=Make(3); coroutine.yield(); witness=at; witness=nil end)
            assert(coroutine.resume(co))
        end
        function FinishCoroutine() assert(coroutine.resume(co)); co=nil end
        function Receive(at) h=at end
        function Queued() local at=Make(4); Post(at); Timer(at); Send(at) end
        function Fail() h=nil; Make(99); error("rollback") end
        function Fault() h=nil; while true do end end
    )"));
    auth::ValueList host_roots;
    const auto create = [&](std::shared_ptr<ai::GalacticWorld> owner, std::uint64_t interval = 1) {
        auto scripts = must(auth::ScriptScheduler::create(config, manifest));
        must(scripts.register_managed_handles({ai::handle_galactic_markup, "galactic.markup.inventory", "galactic.markup.release", interval}));
        must(scripts.set_immediate_effect_host(ai::markup_immediate_host({}, owner->markup()), 32));
        scripts.set_managed_handle_roots([&] { return host_roots; });
        must(scripts.register_binding("Make", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("galactic.markup", {foc::detail::handle(foc::handle_player, 1),
                auth::Value{auth::ValueList{foc::detail::handle(foc::handle_game_object, 10), foc::detail::handle(foc::handle_game_object, 10)}},
                args.at(0), auth::Value{}});
        }));
        must(scripts.register_binding("Post", [](auth::BindingContext& context, const auth::ValueList& args) {
            auth::ScriptEvent event; event.target=9; event.kind=auth::ScriptEvent::Kind::call; event.name="Receive"; event.arguments=args;
            auto result=context.post_event(std::move(event));
            return result ? eawr::core::Result<auth::ValueList>::success({}) : eawr::core::Result<auth::ValueList>::failure(result.error());
        }));
        must(scripts.register_binding("Timer", [](auth::BindingContext& context, const auth::ValueList& args) {
            auth::ScriptEvent event; event.target=9; event.kind=auth::ScriptEvent::Kind::call; event.name="Receive"; event.arguments=args;
            auto result=context.start_timer(ai::real(1), std::move(event));
            return result ? eawr::core::Result<auth::ValueList>::success({}) : eawr::core::Result<auth::ValueList>::failure(result.error());
        }));
        must(scripts.register_binding("Send", [](auth::BindingContext& context, const auth::ValueList& args) {
            context.issue_command("ModAlias", args);
            return eawr::core::Result<auth::ValueList>::success({});
        }));
        must(scripts.register_binding("Warn", [](auth::BindingContext& context, const auth::ValueList&) {
            context.report(auth::codes::unsupported_api, "benign mod warning");
            return eawr::core::Result<auth::ValueList>::success({});
        }));
        return scripts;
    };
    auto scripts=create(world); must(scripts.create_instance(7, "mod.lua")); must(scripts.create_instance(9, "mod.lua"));
    std::uint64_t sequence{};
    const auto call = [&](auth::ScriptScheduler& target, std::string name, auth::ValueList args = {}, std::uint64_t id = 7) {
        auth::ScriptEvent event; event.key={target.completed_tick()+1, auth::first_simulation_producer, id, sequence++};
        event.target=id; event.kind=auth::ScriptEvent::Kind::call; event.name=std::move(name); event.arguments=std::move(args);
        must(target.submit_event(std::move(event))); return must(target.service(executor));
    };
    const auto value = [&](const auto& owner) { return owner->markup()->value({1, false, 10}); };
    auto report=call(scripts, "Local");
    expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "dropped local subtracts rounded repeated-target contribution");
    call(scripts, "Hold"); report=call(scripts, "WarningDrop");
    expect(report.diagnostics.size() == 1 && report.managed_handles.released == 1 && value(world) == ai::Real{},
        "benign diagnostic does not suppress automatic release");
    report=call(scripts, "Anchored");
    expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "private identity anchor does not retain a dead container's handles");
    for (const std::string survivor : {"key", "cycle", "get"}) {
        call(scripts, "Graph");
        for (const std::string name : {"key", "cycle", "get"}) if (name != survivor) {
            auth::ScriptEvent clear; clear.key={scripts.completed_tick()+1, auth::first_simulation_producer, 7, sequence++};
            clear.target=7; clear.kind=auth::ScriptEvent::Kind::assign; clear.name=name;
            must(scripts.submit_event(std::move(clear)));
        }
        report=must(scripts.service(executor));
        expect(report.managed_handles.released == 0 && value(world) == ai::real(4), "each key/cycle/upvalue alias independently retains ownership");
        report=call(scripts, "ClearGraph");
        expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "each isolated graph alias releases after its last root drops");
    }
    const auto release_frontier=world->markup()->state().next_handle;
    report=call(scripts, "Many");
    expect(report.managed_releases == std::vector<auth::Handle>{{ai::handle_galactic_markup, release_frontier},
        {ai::handle_galactic_markup, release_frontier+1}, {ai::handle_galactic_markup, release_frontier+2}}, "release sequence uses ascending stable native IDs");
    expect(report.managed_handles.released == 3 && value(world) == ai::Real{}, "multiple releases preserve stable native ID subtraction order");
    report=call(scripts, "Graph");
    expect(report.managed_handles.released == 0 && value(world) == ai::real(4), "table keys, cycles and closed upvalues retain an alias");
    const auto before_save=world->markup()->encode(); const auto saved=must(scripts.save(executor));
    must(scripts.state_hash(executor));
    expect(world->markup()->encode() == before_save && must(scripts.save(executor)) == saved, "save/hash never collect native values");
    must(scripts.load(saved));
    expect(world->markup()->encode() == before_save, "replacing the old VM during load never releases its native owner");
    auto restored_world=std::make_shared<ai::GalacticWorld>(setup()); must(restored_world->bind(snapshot(), {}, executor, 0, 1000));
    must(restored_world->restore(must(world->capture())));
    auto restored=create(restored_world); must(restored.load(saved));
    expect(must(restored.save(executor)) == saved && restored_world->markup()->encode() == before_save, "native and script restore neither release nor reapply");
    auto resumed_report=call(restored, "ClearGraph"); report=call(scripts, "ClearGraph");
    expect(report.managed_handles.released == 1 && resumed_report.managed_handles.released == 1
        && value(world) == ai::Real{} && world->markup()->encode() == restored_world->markup()->encode()
        && must(scripts.save(executor)) == must(restored.save(executor)), "restored aliases release once at the same barrier");
    report=call(scripts, "Coroutine");
    expect(report.diagnostics.empty() && report.managed_handles.released == 0 && value(world) == ai::real(6), "yielded coroutine stack retains its local handle");
    report=call(scripts, "FinishCoroutine");
    expect(report.diagnostics.empty() && report.managed_handles.released == 1 && value(world) == ai::Real{}, "finished unreachable coroutine releases its handle");
    report=call(scripts, "Queued");
    expect(report.managed_handles.released == 0 && report.commands.size() == 1 && value(world) == ai::real(8), "outgoing posts, timers and commands retain otherwise-local aliases");
    const auto command_roots=report.commands.front().arguments;
    host_roots=command_roots;
    report=must(scripts.service(executor));
    expect(report.managed_handles.released == 0 && value(world) == ai::real(8), "removed source cannot release destination/event aliases");
    host_roots.clear();
    report=call(scripts, "Drop", {}, 9);
    expect(report.managed_handles.released == 0, "pending timer retains aliases after destination globals drop");
    host_roots=command_roots;
    must(scripts.remove_instance(7));
    must(scripts.remove_instance(9)); report=must(scripts.service(executor));
    expect(report.managed_handles.released == 0 && value(world) == ai::real(8), "retained host values survive all VM removals");
    host_roots.clear(); report=must(scripts.service(executor));
    expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "last host alias release subtracts once with no surviving instances");
    report=must(scripts.service(executor)); expect(report.managed_handles.released == 0, "repeat census cannot subtract twice");
    must(scripts.create_instance(7, "mod.lua")); call(scripts, "Hold");
    const auto retained=world->markup()->encode(); report=call(scripts, "Fail");
    expect(!report.diagnostics.empty() && report.managed_handles.censuses == 0 && world->markup()->encode() == retained,
        "rolled-back resume skips automatic release of earlier contributions");
    const auto failed_save=must(scripts.save(executor));
    expect(world->markup()->encode() == retained, "saving after rollback retains contribution until a clean invocation");
    must(restored_world->restore(must(world->capture()))); must(restored.load(failed_save));
    resumed_report=must(restored.service(executor));
    report=must(scripts.service(executor)); expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "next clean invocation collects rollback orphan");
    expect(resumed_report.managed_handles.released == 1 && world->markup()->encode() == restored_world->markup()->encode()
        && must(scripts.save(executor)) == must(restored.save(executor)), "save after rollback resumes at the same release boundary");
    call(scripts, "Hold"); const auto before_fault=world->markup()->encode(); report=call(scripts, "Fault");
    expect(!report.diagnostics.empty() && report.managed_handles.censuses == 0 && world->markup()->encode() == before_fault,
        "quota fault skips release during rollback/removal");
    report=must(scripts.service(executor)); expect(report.managed_handles.released == 1 && value(world) == ai::Real{}, "clean empty invocation releases faulted-instance ownership");
    static_cast<void>(failed_save);
    auto rejected=create(world); must(rejected.create_instance(7, "mod.lua")); call(rejected, "Hold");
    const auto before_rejected=world->markup()->encode();
    auto refusing=std::make_shared<RejectPublication>(ai::markup_immediate_host({}, world->markup()));
    refusing->reject=true; must(rejected.set_immediate_effect_host(refusing, 32));
    auth::ScriptEvent drop; drop.key={rejected.completed_tick()+1, auth::first_simulation_producer, 7, 0};
    drop.target=7; drop.kind=auth::ScriptEvent::Kind::call; drop.name="Drop"; must(rejected.submit_event(std::move(drop)));
    expect(!rejected.service(executor) && world->markup()->encode() == before_rejected, "failed publication never releases live native contributions");
    // A fixed cadence is in the completed-invocation domain. Save immediately
    // before its boundary, with both an orphaned local and a dropped global.
    constexpr std::uint64_t interval = 64;
    auto deferred_world=std::make_shared<ai::GalacticWorld>(setup());
    must(deferred_world->bind(snapshot(), {}, executor, 0, 1000));
    auto deferred=create(deferred_world, interval); must(deferred.create_instance(7, "mod.lua"));
    report=call(deferred, "Local");
    const auto deferred_native=deferred_world->markup()->encode();
    expect(report.managed_handles.censuses == 0 && value(deferred_world) != ai::Real{}, "off-cadence local remains until its fixed release boundary");
    while (deferred.completed_tick() < interval-1) {
        report=must(deferred.service(executor));
        expect(report.managed_handles.censuses == 0 && deferred_world->markup()->encode() == deferred_native,
            "off-cadence service neither walks nor releases");
    }
    auto deferred_resumed_world=std::make_shared<ai::GalacticWorld>(setup());
    must(deferred_resumed_world->bind(snapshot(), {}, executor, 0, 1000));
    must(deferred_resumed_world->restore(must(deferred_world->capture())));
    auto deferred_resumed=create(deferred_resumed_world, interval);
    const auto before_boundary=must(deferred.save(executor));
    must(deferred_resumed.load(before_boundary));
    expect(deferred_resumed.completed_tick() == interval-1 && must(deferred_resumed.save(executor)) == before_boundary
        && deferred_resumed_world->markup()->encode() == deferred_native, "save/load preserves the pending cadence phase without collecting");
    auto wrong_interval=create(deferred_resumed_world);
    expect(!wrong_interval.load(before_boundary), "a different invocation interval rejects the saved session identity");
    report=must(deferred.service(executor)); resumed_report=must(deferred_resumed.service(executor));
    expect(report.managed_handles.released == 1 && resumed_report.managed_handles.released == 1
        && value(deferred_world) == ai::Real{} && deferred_world->markup()->encode() == deferred_resumed_world->markup()->encode()
        && must(deferred.save(executor)) == must(deferred_resumed.save(executor)), "uninterrupted and restored locals release at the same fixed boundary");
    call(deferred, "Hold");
    while (deferred.completed_tick() < 2*interval-2) must(deferred.service(executor));
    report=call(deferred, "Drop");
    expect(report.managed_handles.released == 0 && value(deferred_world) == ai::parse_real("2.5").value(),
        "last global alias dropped before the boundary remains until the due invocation");
    must(deferred_resumed_world->restore(must(deferred_world->capture())));
    must(deferred_resumed.load(must(deferred.save(executor))));
    report=must(deferred.service(executor)); resumed_report=must(deferred_resumed.service(executor));
    expect(report.managed_handles.released == 1 && resumed_report.managed_handles.released == 1
        && value(deferred_world) == ai::Real{} && deferred_world->markup()->encode() == deferred_resumed_world->markup()->encode()
        && must(deferred.save(executor)) == must(deferred_resumed.save(executor)), "restored dropped globals retain the same fixed release phase");
    report=must(deferred.service(executor));
    expect(report.managed_handles.released == 0, "service after the due release never subtracts twice");
    const auto final_bytes=world->markup()->encode();
    return must(scripts.save(executor)) + std::string(final_bytes.begin(), final_bytes.end()) + must(deferred.save(executor));
}

std::string contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto world = std::make_shared<ai::GalacticWorld>(setup());
    ai::GalacticConnectivity connectivity;
    connectivity.goal_reach = {{1, 30, "FRIENDLY_ONLY", {20, 10}}};
    must(world->bind(snapshot(), connectivity, executor, 0, 1000));
    const auto unsigned_hints = world->markup()->state();
    auto destroyed = unsigned_hints;
    const auto doomed = must(world->markup()->apply(destroyed, 1,
        std::array<ai::GalacticMarkupKey, 3>{{{1, false, 10}, {1, false, 20}, {1, false, 10}}}, ai::parse_real("1.25").value(), {}));
    must(world->markup()->apply(destroyed, 1, std::array<ai::GalacticMarkupKey, 1>{{{1, false, 20}}}, ai::real(2), {}));
    must(world->markup()->apply(destroyed, 2, std::array<ai::GalacticMarkupKey, 1>{{{2, false, 10}}}, ai::real(3), {}));
    const auto player_mark = must(world->markup()->apply(destroyed, 1,
        std::array<ai::GalacticMarkupKey, 1>{{{1, true, 2}}}, ai::real(5), {}));
    expect(world->markup()->destroy(destroyed, doomed) && !destroyed.handles.contains(doomed)
        && destroyed.values.at({1, false, 10}) == ai::Real{} && destroyed.values.at({1, false, 20}) == ai::real(2)
        && destroyed.values.at({2, false, 10}) == ai::real(3), "native destroy removes every repeated contribution and preserves other handles/observers");
    const auto once = destroyed;
    expect(!world->markup()->destroy(destroyed, doomed) && destroyed == once, "repeated native destroy cannot subtract twice or rewind the identity frontier");
    expect(world->markup()->destroy(destroyed, player_mark) && destroyed.values.at({1, true, 2}) == ai::Real{},
        "native destroy also subtracts player-root contributions");
    must(world->markup()->validate(destroyed));
    auto signed_hints = unsigned_hints;
    signed_hints.hints.at(1).defaults.at("SIGNED") = ai::Real::from_repr(std::uint64_t{1} << 63U);
    world->markup()->replace(signed_hints);
    const auto signed_bytes = world->markup()->encode();
    world->markup()->replace(unsigned_hints);
    world->markup()->replace(must(world->markup()->decode(signed_bytes)));
    expect(world->markup()->encode() == signed_bytes, "retained hint save preserves signed-zero payload bits");
    world->markup()->replace(unsigned_hints);
    auto host = std::make_shared<foc::detail::Host>(); host->ai_world = world;
    for (const auto& player : setup().players) host->setup.players.push_back({player.player, player.faction, false, player.ai, "QUERY", player.human});
    for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
    auto engine = std::make_shared<ai::Engine>(host, data(), std::vector<ai::PlanDef>{}); host->engine = engine.get();
    auth::SessionConfig config; config.galactic_clock = true;
    auth::ModuleManifest manifest;
    must(manifest.add("proof.lua", R"(
        retained = nil
        player_handle = nil
        score = 0
        finished = false
        path_count = 0
        stage_id = nil
        function Mark(p, a, b)
            retained = Apply_Markup(p, {a, b, a}, 1.25)
            finished = retained.IsFinished()
            assert(finished == false and retained.Result() == true)
            score = EvaluatePerception("Markup", p, a)
        end
        function Replace(p, a, b)
            retained = Apply_Markup(p, b, 4, retained)
            score = EvaluatePerception("Markup", p, a)
        end
        function PlayerMark(p, other)
            player_handle = Apply_Markup(p, other, 3)
            score = EvaluatePerception("EnemyMarkup", p)
        end
        function Fail(p, a)
            retained = Apply_Markup(p, a, 99, retained)
            error("rollback markup")
        end
        function Path(p, a, b)
            local path = Find_Path(p, a, b)
            assert(path[1] == a and path[2] == b)
            path_count = table.getn(path)
            stage_id = _FindStageArea(p, b, nil)
        end
        function List(a, b, c)
            local planets = FindPlanet.Get_All_Planets()
            assert(planets[1] == a and planets[2] == b and planets[3] == c)
            path_count = table.getn(planets)
        end
        function Destroy()
            retained:Destroy()
        end
        function Release()
            retained:Release()
        end
        function InvalidTarget(p, target)
            retained = Apply_Markup(p, target, 99, retained)
        end
    )"));
    auto scripts = must(auth::ScriptScheduler::create(config, manifest));
    std::vector<eawr::core::Diagnostic> diagnostics;
    foc::tactical_ai_detail::register_globals(scripts, host, diagnostics);
    foc::tactical_ai_detail::register_methods(scripts, host, diagnostics);
    expect(diagnostics.empty(), "markup fixture installs shared Lua bindings");
    must(ai::register_galactic_markup_bindings(scripts, world, engine,
        [](auto, auto a, auto b) { return eawr::core::Result<std::vector<gc::PlanetId>>::success({a, b}); }));
    must(scripts.set_immediate_effect_host(ai::markup_immediate_host({}, world->markup(), world, engine), 32));
    std::uint64_t sequence{}; must(engine->before_service(*world, scripts, sequence, &executor));
    const auto query = [&](std::string_view equation, gc::PlanetId target, gc::PlayerId player = 1) {
        return world->evaluate(*engine, equation, player, engine->target_of_object(target));
    };
    expect(query("Markup", 10) == ai::Real{} && query("Markup", 20, 2) == ai::Real{}, "markup records start at zero independently of hint file presence");
    expect(query("Hint", 10) == ai::real(7) && query("Hint", 20) == ai::real(2)
        && !query("Hint", 20, 2) && !query("Unknown", 10), "authored override, set default, absent branch and unknown token are distinct");
    expect(world->markup_stage(1, 30, "FRIENDLY_ONLY") == 20 && world->markup_stage(1, 30, "SINGLE_HOP_DISCONNECTED") == 30,
        "stage selects first retained production site or disconnected target");
    must(scripts.create_instance(7, "proof.lua"));
    const auto call = [&](std::string name, auth::ValueList args) {
        auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 7, sequence++};
        event.target = 7; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name); event.arguments = std::move(args);
        must(scripts.submit_event(event)); return must(scripts.service(executor));
    };
    const auto player = foc::detail::handle(foc::handle_player, 1), a = foc::detail::handle(foc::handle_game_object, 10),
        b = foc::detail::handle(foc::handle_game_object, 20), c = foc::detail::handle(foc::handle_game_object, 30);
    auto report = call("Mark", {player, a, b});
    expect(report.diagnostics.empty() && query("Markup", 10) == ai::parse_real("2.5").value()
        && query("Markup", 20) == ai::parse_real("1.25").value() && query("Markup", 10, 2) == ai::Real{},
        "additive markup retains repeated list entries and observer isolation");
    const auto score = must(scripts.read_global(7, "score"));
    const auto finished = must(scripts.read_global(7, "finished"));
    expect(finished && std::get_if<bool>(&finished->data) && !std::get<bool>(finished->data), "markup handle remains unfinished as sourced");
    expect(score && std::get<ai::Real>(score->data) == ai::parse_real("2.5").value(), "same Lua transaction reads its private markup writes");
    report = call("Replace", {player, a, b});
    expect(report.diagnostics.empty() && query("Markup", 10) == ai::Real{} && query("Markup", 20) == ai::real(4), "handle reuse removes all old records before applying the new target/value");
    report = call("PlayerMark", {player, foc::detail::handle(foc::handle_player, 2)});
    expect(report.diagnostics.empty() && query("EnemyMarkup", 10) == ai::real(3)
        && query("PlayerMarkup", 10) == ai::Real{}, "Lua player targets have independent retained roots and perception reads");
    const auto retained = world->markup()->encode();
    report = call("Path", {player, a, c});
    const auto path_count = must(scripts.read_global(7, "path_count"));
    const auto stage = must(scripts.read_global(7, "stage_id"));
    expect(report.diagnostics.empty() && path_count && std::get<ai::Real>(path_count->data) == ai::real(2),
        "Lua Find_Path returns both callback planets");
    const auto* stage_handle = stage ? foc::detail::as_handle(*stage, foc::handle_game_object) : nullptr;
    expect(stage_handle && stage_handle->id == 20, "Lua stage selection returns the first production site's planet handle");
    report = call("List", {a, b, c});
    const auto planet_count = must(scripts.read_global(7, "path_count"));
    expect(report.diagnostics.empty() && planet_count && std::get<ai::Real>(planet_count->data) == ai::real(3),
        "Lua Get_All_Planets returns the three bound planets");
    for (const auto* method : {"Destroy", "Release"}) {
        report = call(method, {});
        expect(!report.diagnostics.empty() && world->markup()->encode() == retained,
            "markup handles reject unsupported lifetime methods without changing contributions");
    }
    expect(world->containing_planet(40) == 10, "non-planet negative fixture is a live fleet at its own parent planet");
    report = call("InvalidTarget", {player, foc::detail::handle(foc::handle_game_object, 40)});
    expect(!report.diagnostics.empty() && world->markup()->encode() == retained,
        "live non-planet target refuses replacement rather than silently skipping or marking its parent");
    report = call("InvalidTarget", {player, foc::detail::handle(foc::handle_game_object, 999)});
    expect(!report.diagnostics.empty() && world->markup()->encode() == retained,
        "unavailable target root refuses replacement without silently removing its previous contribution");
    report = call("Fail", {player, a});
    expect(!report.diagnostics.empty() && world->markup()->encode() == retained, "failed Lua resume rolls back replacement and handle allocation");
    const auto saved = must(world->capture()); const auto digest = must(world->state_hash());
    expect(saved.components.size() == 1 && saved.components.contains("GALACTIC_PERCEPTION"), "perception owns one composed native component including markup");
    ai::GalacticWorld resumed(setup()); must(resumed.bind(snapshot(), connectivity, executor, 0, 1000));
    must(resumed.restore(saved)); expect(must(resumed.state_hash()) == digest, "save retains numbers, hints and replacement handle records");
    const auto bytes = world->markup()->encode();
    for (std::size_t size = 0; size < bytes.size(); ++size) expect(!world->markup()->decode(std::span(bytes).first(size)), "all truncated markup saves refuse before publication");
    auto corrupt = saved; corrupt.components.at("GALACTIC_PERCEPTION").pop_back();
    expect(!world->restore(corrupt) && must(world->state_hash()) == digest, "invalid markup component leaves all perception state untouched");
    auto state = world->markup()->state();
    const std::array<ai::GalacticMarkupKey, 1> keys{{{2, false, 10}}};
    const auto other = must(world->markup()->apply(state, 2, keys, ai::parse_real("0.1").value(), {}));
    expect(state.handles.at(other).value == ai::to_single(ai::parse_real("0.1").value()), "writer rounds each contribution to single precision");
    const std::array<ai::GalacticMarkupKey, 1> player_keys{{{1, true, 2}}};
    must(world->markup()->apply(state, 1, player_keys, ai::real(3), {}));
    expect(!world->markup()->apply(state, 3, std::array<ai::GalacticMarkupKey, 1>{{{3, false, 10}}}, ai::real(3), {}), "human observers cannot write AI markup");
    world->markup()->replace(std::move(state));
    must(world->bind(snapshot(true), {}, executor, 0, 1000));
    const auto reduced = world->markup()->state();
    for (const auto& [id, handle] : reduced.handles) {
        static_cast<void>(id); for (const auto& target : handle.targets) expect(target.player_target || target.target != 20, "removed planet drops from retained handle references");
    }
    return must(world->state_hash()) + scripts.state_digest();
}
}
int case_galactic_ai_markup_tests() {
    try {
        encoded_bytes_contract();
        std::string baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto digest = contracts(workers) + lifetime_contracts(workers);
            if (workers == 1) baseline = digest; else expect(digest == baseline, "markup, script transactions and persistence equal on 1/2/4/8 workers");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

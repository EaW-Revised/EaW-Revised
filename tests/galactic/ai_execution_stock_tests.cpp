#include "galactic_support.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_execution_state.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_state.hpp"
#include "tactical_ai_internal.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>

// Insert before the anonymous namespace; this is fixture setup only.
namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static void magic_fixture(Engine& engine, tactical::PlayerId player, std::uint64_t instance,
        sim::EntityId planet, tactical::TypeId type) {
        PlayerAi owner; owner.player = player; engine.players_.push_back(std::move(owner));
        Plan plan; plan.id = plan.goal = 7; plan.instance = instance; plan.player = player;
        plan.definition = 0; plan.taskforces = {1}; plan.target_object = planet;
        engine.running_.emplace(plan.id, plan); engine.plan_of_instance_[instance] = plan.id;
        TaskForce force; force.id = 1; force.plan = plan.id; force.name = "MainForce"; force.types = {type};
        engine.taskforces_.emplace(force.id, std::move(force));
        Target target; target.id = 1; target.object = planet; engine.targets_.emplace(target.id, std::move(target));
    }
    static void complete_native(Engine& engine) { engine.service_execution(engine.players_.front()); }
};
}


namespace {
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace cp = eawr::campaign;
using galactic_test::expect;
using ga::Number;
using eawr::sim::math::Fixed;
Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}

void immediate_cash_contracts() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        gc::SyntheticGalaxyOptions options; options.players = 1; options.planets = 4;
        options.fleets_per_player = options.members_per_fleet = 1;
        auto setup = gc::synthetic_galaxy(options);
        setup.player_economy = gc::PlayerEconomySetup{{{1, whole(100), whole(1000), 2, 5}}};
        auto world = take(gc::GalacticSession::create(setup));
        auto funding = std::make_shared<ai::GalacticFunding>();
        ai::EvaluatedFundingTemplate policy; policy.equations = {{1, whole(1)}};
        expect(static_cast<bool>(funding->initialize(1, std::span(&policy, 1), whole(100))), "immediate cash funding initializes");
        ga::NativeActions actions(world, funding, {}, {{1, whole(2)}}, {}, &executor);
        actions.bind_handlers();
        auto effects = actions.immediate_host({}, 16);
        const auto before_world = world.state_sha256();
        const auto before_funding = funding->state();
        const auto before_id = world.next_entity_id();
        auth::ModuleManifest manifest;
        expect(static_cast<bool>(manifest.add("Contract.lua", R"(
Player = SelfPlayer()
function Good()
    Before = Player.Get_Credits()
    Player.Give_Money(5)
    After = Player.Get_Credits()
    assert(After == Before + 10)
end
function Failed()
    Player.Give_Money(500)
    error('discard cash and windfall')
end
function Negative()
    BeforeNegative = Player.Get_Credits()
    Player.Give_Money(-3)
    AfterNegative = Player.Get_Credits()
    assert(AfterNegative == BeforeNegative - 3)
end
function Caught()
    BeforeCaught = Player.Get_Credits()
    local ok = pcall(function() Player.Give_Money(9) end)
    AfterCaught = Player.Get_Credits()
    assert(not ok and AfterCaught == BeforeCaught)
end
)")), "immediate cash manifest mounts");
        expect(static_cast<bool>(manifest.add("FailedRoot.lua", "Player = SelfPlayer() Player.Give_Money(9) error('root rollback')")), "failed native root mounts");
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.galactic_clock = true;
        auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
        auto host = std::make_shared<foc::detail::Host>();
        host->ai_world = std::make_shared<ai::GalacticWorld>(ai::GalacticPerceptionSetup{});
        host->view = std::make_shared<foc::detail::WorldView>();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host, errors);
        foc::tactical_ai_detail::register_methods(scripts, host, errors);
        expect(errors.empty(), "real immediate player methods register");
        expect(static_cast<bool>(scripts.register_binding("SelfPlayer", [](auth::BindingContext&, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value{auth::Handle{foc::handle_player, 1}}});
        })), "immediate fixture supplies native player");
        expect(static_cast<bool>(scripts.set_immediate_effect_host(effects, 3)), "bounded GC native provider installs");
        expect(!scripts.create_instance(9, "FailedRoot.lua") && world.state_sha256() == before_world && funding->state() == before_funding
            && world.next_entity_id() == before_id, "root failure discards real account, funding and frontier");
        for (const auto id : {3ULL, 2ULL, 1ULL}) expect(static_cast<bool>(scripts.create_instance(id, "Contract.lua")), "cash instances load in reverse order");
        for (const auto& [id, name] : {std::pair{1ULL, "Good"}, std::pair{2ULL, "Failed"}, std::pair{3ULL, "Negative"}}) {
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, id, 0}; event.target = id;
            event.kind = auth::ScriptEvent::Kind::call; event.name = name;
            expect(static_cast<bool>(scripts.submit_event(event)), "cash observer event submits");
        }
        const auto report = take(scripts.service(executor));
        expect(report.diagnostics.size() == 1 && world.snapshot()->player_economy()->players.front().credits == whole(107),
            "accepted native cash commits in instance order, failed handler contributes no cash");
        expect(std::get<Number>(take(scripts.read_global(1, "After"))->data) == Number(110)
            && std::get<Number>(take(scripts.read_global(3, "BeforeNegative"))->data) == Number(110)
            && std::get<Number>(take(scripts.read_global(3, "AfterNegative"))->data) == Number(107), "immediate reads share successful canonical writes");
        expect(funding->ledger(1)->categories.at(1).cash == whole(107) && funding->ledger(1)->categories.at(1).available_cash == whole(100)
            && world.next_entity_id() == before_id,
            "one accepted windfall per actual delta and no identity allocation for cash");
        const auto trace = world.state_sha256() + take(actions.state_hash()) + scripts.state_digest();
        if (baseline.empty()) baseline = trace; else expect(trace == baseline, "real immediate cash world/funding/script equality on 1/2/4/8");
        expect(actions.immediate_work().copied_scene_objects == 0 && actions.immediate_work().requests == 2,
            "cash copies only the two accepted account writes, never the scene");
        auto stale = take(effects->capture());
        (void)take(stale->apply("galactic.give_money", {auth::Value{auth::Handle{foc::handle_player, 1}}, auth::Value::number(Number(1))}));
        (void)take(world.add_credits(1, whole(1), whole(1)));
        const auto live = world.state_sha256(); const auto ledger = funding->state();
        expect(!effects->publish(std::move(stale)) && world.state_sha256() == live && funding->state() == ledger,
            "stale native publication refuses before changing live world or funding");
        auto overflow = funding->state();
        overflow.at(1).categories.at(1).cash = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        take(funding->restore(std::move(overflow)));
        const auto refusal = funding->state();
        auth::ScriptEvent caught; caught.key = {2, auth::first_simulation_producer, 1, 0}; caught.target = 1;
        caught.kind = auth::ScriptEvent::Kind::call; caught.name = "Caught";
        expect(static_cast<bool>(scripts.submit_event(caught)), "caught native funding refusal submits");
        const auto caught_report = take(scripts.service(executor));
        expect(caught_report.diagnostics.empty() && world.state_sha256() == live && funding->state() == refusal
            && std::get<Number>(take(scripts.read_global(1, "AfterCaught"))->data) == Number(108),
            "pcall catches a funding refusal after private cash calculation without leaking the native action");
    }
}

void paid_contracts() {
    std::map<int, std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto mode : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        gc::SyntheticGalaxyOptions options; options.players = mode == 1 ? 2U : 1U; options.planets = 4;
        options.fleets_per_player = options.members_per_fleet = 1;
        auto setup = gc::synthetic_galaxy(options);
        for (auto& seat : setup.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
        setup.seat_control.reset();
        setup.rules.fiscal_cycle_frames = 10000;
        setup.rules.maximum_fleet_movement_distance = Fixed{};
        setup.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
        setup.player_economy = gc::PlayerEconomySetup{{{1, whole(mode == 5 ? 0 : 250), whole(10000), 2, 5}}};
        setup.fiscal = gc::FiscalSetup{whole(2500), {{1, true, whole(1)}}, {}};
        setup.production = gc::ProductionSetup{5, {{1, false, true, true, whole(50), {}, {}, {}, {}}}, {}};
        for (const auto& planet : setup.planets) {
            setup.fiscal->planets.push_back({planet.id, {}});
            gc::PlanetProduction producer;
            producer.planet = planet.id; producer.target_owner = producer.starbase_owner = producer.ground_occupier = 1;
            producer.space_level = 1; producer.maximum_space_level = 5;
            producer.ground_slots = producer.space_slots = 3; producer.maximum_ground_forces = 10;
            producer.enabled = producer.surface_accessible = true;
            gc::ProductionOffer type; type.type = 1000; type.price = whole(mode >= 6 ? 900 : 100); type.seconds = whole(1); type.tech = 2;
            type.population = whole(1); type.affiliated = type.direct_build_allowed = true;
            producer.offers.push_back(type); setup.production->planets.push_back(producer);
        }
        if (mode == 1) {
            setup.player_economy->players.push_back({2, whole(250), whole(10000), 2, 5});
            setup.fiscal->players.push_back({2, true, whole(1)});
            auto other = setup.production->players.front(); other.player = 2; setup.production->players.push_back(other);
            for (auto& producer : setup.production->planets) {
                producer.maximum_ground_forces = 0; producer.orbit_occupier = 2;
                producer.offers.front().seconds = Fixed{};
            }
        }
        auto world = take(gc::GalacticSession::create(setup));
        auto funding = std::make_shared<ai::GalacticFunding>();
        ai::EvaluatedFundingTemplate policy; policy.equations = {{1, whole(1)}};
        expect(static_cast<bool>(funding->initialize(1, std::span(&policy, 1), whole(250))), "native paid fixture initializes C ledger");
        expect(static_cast<bool>(funding->reserve(1, {7, 1, whole(mode == 6 ? 100 : mode >= 7 ? 25 : 200), {}, {}, {}})), "native paid fixture reserves C goal");
        if (mode >= 6) {
            auto state = funding->state(); state.at(1).goals.at(7).capital = whole(25);
            expect(static_cast<bool>(funding->restore(std::move(state))), "shortage fixture retains distinct completion cost and held contribution");
        }
        ga::NativeActions actions(world, funding, {}, {{1, whole(1)}}, {}, &executor, [mode](auto) { return eawr::core::Result<Fixed>::success(whole(mode == 8 ? (std::int64_t{1} << 24) : mode == 4 ? 80 : 100)); });
        const auto record = [&] {
            const auto bytes = take(ga::encode_funding_state(funding->state()));
            expect(take(ga::decode_funding_state(bytes)) == funding->state(), "funding v2 preserves aggregate dead budget exactly");
            const auto trace = world.state_sha256() + take(actions.state_hash()) + std::string(bytes.begin(), bytes.end());
            if (!baseline.contains(mode)) baseline.emplace(mode, trace);
            else expect(baseline.at(mode) == trace, "paid, blocked, refund and orphan world/ledger equality on 1/2/4/8");
        };
        ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = setup.planets.front().id; request.type = 1000;
        const auto initial = world.state_sha256();
        expect(!actions.admit_paid(request, 7) && world.state_sha256() == initial, "absent reconstructed producer handler refuses without mutation");
        actions.bind_handlers();
        expect(!actions.admit(request), "plan identity cannot silently stand in for goal identity");
        const auto first = take(actions.admit_paid(request, 7));
        expect(first.admitted && take(actions.build_status(1, first.request)).state == ai::GalacticBuildStatus::State::waiting,
            "accepted build identity waits for the authoritative producer outcome");
        expect(!actions.build_status(2, first.request) && !actions.build_status(1, first.request + 10000),
            "observer refuses foreign and unknown retained build identities");
        if (mode >= 6) {
            (void)take(world.step(executor));
            const auto categories = funding->ledger(1)->categories;
            (void)take(world.step(executor));
            const auto elapsed = take(eawr::sim::math::subtract(whole(1), take(Fixed::from_ratio(29, 30))));
            const auto due = take(eawr::sim::math::multiply(whole(900), elapsed));
            const auto shortage = mode >= 7 ? take(eawr::sim::math::subtract(due, whole(25))) : Fixed{};
            expect(actions.purchases().at(first.request).billed == Fixed{} && world.snapshot()->player_economy()->players.front().credits == whole(250)
                && funding->ledger(1)->categories == categories, "rejected bill stalls without category or account debit");
            expect(funding->ledger(1)->goals.at(7).marked == take(eawr::sim::math::add(whole(mode == 8 ? (std::int64_t{1} << 24) : 100), shortage)),
                "rejected bill marks computed cost shortage, not held-funds shortage");
            const auto& entry = world.snapshot()->production()->planets.front().queues[0].front();
            expect(entry.last_remaining == whole(1) && entry.start_frame == 1, "rejected bill restores previous serviced timer");
            expect(static_cast<bool>(actions.restore(take(actions.capture()))), "extra uncapped shortage mark survives native restore");
            record(); continue;
        }
        if (mode == 1) {
            (void)take(world.step(executor));
            expect(actions.purchases().at(first.request).billed == Fixed{}, "held zero-duration completion never bills");
            const auto blocked = take(actions.capture());
            expect(static_cast<bool>(actions.restore(blocked)), "held zero-duration state restores with its service cursor");
            (void)take(world.step(executor));
            expect(!world.snapshot()->production()->planets.front().queues[0].empty() && actions.purchases().at(first.request).billed == Fixed{}, "repeated held completion preserves queue and zero bill");
            record(); continue;
        }
        if (mode == 2 || mode == 3) {
            for (unsigned frame = 0; frame < 15; ++frame) (void)take(world.step(executor));
            const auto before_cancel = world.snapshot()->player_economy()->players.front().credits;
            const auto mark = funding->ledger(1)->goals.at(7).marked;
            expect(static_cast<bool>(actions.finish_task(first.request, mode == 3)), "finished current task applies sourced salvage/refund branch");
            expect(funding->ledger(1)->goals.at(7).marked == mark, "finished task never clears residual mark");
            if (mode == 2) {
                expect(world.snapshot()->production()->planets.front().queues[0].empty() && world.snapshot()->player_economy()->players.front().credits > before_cancel, "non-salvage task cancellation refunds stored paid fraction");
                expect(take(actions.build_status(1, first.request)).state == ai::GalacticBuildStatus::State::failed,
                    "manual cancellation commits a failed outcome after removing the queue entry");
                expect(static_cast<bool>(actions.restore(take(actions.capture()))), "cancelled obligation remains restorable");
            } else {
                expect(!world.snapshot()->production()->planets.front().queues[0].empty() && world.snapshot()->player_economy()->players.front().credits == before_cancel, "salvage retains queue without refund");
                expect(static_cast<bool>(funding->remove_goal(1, 7)), "salvage retains dead-goal obligation");
                const auto dead_before = funding->ledger(1);
                for (unsigned frame = 15; frame < 35; ++frame) (void)take(world.step(executor));
                expect(actions.purchases().at(first.request).billed > Fixed{} && funding->ledger(1)->dead_budget < dead_before->dead_budget
                    && funding->ledger(1)->categories == dead_before->categories && funding->ledger(1)->dead_goals == dead_before->dead_goals, "orphan bills aggregate only, leaving keyed rows and categories unchanged");
                expect(static_cast<bool>(actions.restore(take(actions.capture()))), "orphan obligation remains restorable");
            }
            record(); continue;
        }
        const auto second = take(actions.admit_paid(request, 7));
        expect(first.admitted && second.admitted && first.request != second.request, "real native queues retain distinct accepted build identities");
        expect(world.snapshot()->player_economy()->players.front().credits == whole(mode == 5 ? 0 : 250)
            && funding->ledger(1)->goals.at(7).marked == whole(mode == 4 ? 160 : 200), "native admission marks but never bills up front");
        for (unsigned frame = 0; frame < 15; ++frame) (void)take(world.step(executor));
        const auto saved = take(actions.capture());
        const auto before = take(actions.state_hash());
        const auto& bytes = saved.components.at("galactic-production");
        for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
            auto truncated = saved; truncated.components["galactic-production"].resize(prefix);
            expect(!actions.restore(truncated) && take(actions.state_hash()) == before, "truncated paid mapping restore is atomic");
        }
        expect(static_cast<bool>(actions.restore(saved)), "paid mapping rebind accepts same reconstructed queue/ledger boundary");
        actions.bind_handlers();
        for (unsigned frame = 15; frame < 65; ++frame) (void)take(world.step(executor));
        const auto billed = take(eawr::sim::math::add(actions.purchases().at(first.request).billed, actions.purchases().at(second.request).billed));
        expect(billed > Fixed{} && billed < whole(200), "completed fronts receive no terminal bill (WGEP-65)");
        expect(world.snapshot()->player_economy()->players.front().credits == std::max(Fixed{}, take(eawr::sim::math::subtract(whole(mode == 5 ? 0 : 250), billed))), "goal-funded installments ignore account affordability and clamp balance independently");
        expect(funding->ledger(1)->goals.at(7).marked == std::max(Fixed{}, take(eawr::sim::math::subtract(whole(mode == 4 ? 160 : 200), billed))), "completion preserves residual marks");
        expect(world.snapshot()->production()->planets.front().objects.size() == 2, "two ordinary real paid completions place actors");
        for (auto build : {first.request, second.request}) {
            const auto status = take(actions.build_status(1, build));
            expect(status.state == ai::GalacticBuildStatus::State::completed && status.object != 0 && status.object != build,
                "completed paid identity resolves its distinct actual actor");
            expect(std::any_of(world.snapshot()->production()->planets.front().objects.begin(), world.snapshot()->production()->planets.front().objects.end(),
                [&](const auto& object) { return object.id == status.object; }), "retained completion actor exists in authoritative production results");
        }
        record();
        expect(static_cast<bool>(actions.restore(take(actions.capture()))), "completed mapping preserves actual bills and aggregated residual marks");
    }
    std::string loss_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4; options.fleets_per_player = 0;
        auto setup = gc::synthetic_galaxy(options); setup.seat_control.reset(); setup.rules.fiscal_cycle_frames = 10000;
        for (auto& seat : setup.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
        setup.rules.maximum_fleet_movement_distance = Fixed{};
        setup.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
        setup.player_economy = gc::PlayerEconomySetup{{{1, whole(200), whole(1000), 2, 5}, {2, whole(200), whole(1000), 2, 5}}};
        setup.fiscal = gc::FiscalSetup{whole(2500), {{1, true, whole(1)}, {2, true, whole(1)}}, {}};
        for (const auto& planet : setup.planets) setup.fiscal->planets.push_back({planet.id, {}});
        setup.production = gc::ProductionSetup{5, {{1, false, true, true, whole(50), {}, {}, {}, {}}, {2, false, true, true, whole(50), {}, {}, {}, {}}}, {}};
        setup.control.emplace();
        setup.routes.clear(); setup.control->change_seconds = whole(30); setup.control->max_ground_forces = 10;
        setup.control->players = {{1, false, true, 1}, {2, true, false, 2}};
        setup.control->relations = {{1, 1, true}, {1, 2, false}, {2, 1, false}, {2, 2, true}};
        setup.control->types = {{1000, 1, false, false}};
        for (const auto& site : setup.planets) {
            gc::ControlPlanet presence; presence.planet = site.id; presence.type = site.id;
            presence.surface_accessible = true; presence.ground_base = {1, 1}; presence.starbase = {1, 1};
            setup.control->planets.push_back(presence);
        }
        const auto planet = setup.planets.front().id;
        gc::PlanetProduction producer; producer.planet = planet; producer.target_owner = producer.ground_occupier = 1;
        producer.starbase_owner = 1; producer.space_level = 1; producer.maximum_space_level = 5;
        producer.ground_slots = producer.space_slots = 3; producer.maximum_ground_forces = 10; producer.enabled = producer.surface_accessible = true;
        gc::ProductionOffer offer; offer.type = 1000; offer.price = whole(100); offer.seconds = whole(1); offer.tech = 2;
        offer.domain = gc::ProductionDomain::space;
        offer.affiliated = offer.direct_build_allowed = true; producer.offers = {offer}; setup.production->planets = {producer};
        for (const auto& site : setup.planets) if (site.id != planet) {
            auto other = producer; other.planet = site.id; other.offers.clear();
            setup.production->planets.push_back(std::move(other));
        }
        auto world = take(gc::GalacticSession::create(setup));
        auto funding = std::make_shared<ai::GalacticFunding>(); ai::EvaluatedFundingTemplate policy; policy.equations = {{1, whole(1)}};
        expect(static_cast<bool>(funding->initialize(1, std::span(&policy, 1), whole(200))), "producer-loss ledger initializes");
        expect(static_cast<bool>(funding->reserve(1, {7, 1, whole(100), {}, {}, {}})), "producer-loss goal reserves");
        ga::NativeActions actions(world, funding, {}, {{1, whole(1)}, {2, whole(1)}}, {}, &executor,
            [](auto) { return eawr::core::Result<Fixed>::success(whole(100)); }); actions.bind_handlers();
        ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = planet; request.type = 1000;
        const auto admitted = take(actions.admit_paid(request, 7));
        for (unsigned frame = 0; frame < 15; ++frame) (void)take(world.step(executor));
        const auto credits = world.snapshot()->player_economy()->players.front().credits;
        expect(credits < whole(200), "producer-loss fixture has already paid installments");
        gc::ControlBattleReturn returned; returned.planet = planet; returned.invader = 2; returned.defender = 1; returned.winner = 2;
        expect(static_cast<bool>(world.apply_control_battle(returned)), "producer-loss space return removes the real starbase owner");
        (void)take(world.step(executor));
        expect(world.snapshot()->production()->planets.front().queues[1].empty()
            && take(actions.build_status(1, admitted.request)).state == ai::GalacticBuildStatus::State::failed,
            "automatic producer-loss cancellation publishes failed retained build outcome");
        expect(world.snapshot()->player_economy()->players.front().credits == credits, "WGEP-37: automatic starbase loss keeps the paid amount without direct refund");
        const auto hash = world.state_sha256() + take(actions.state_hash());
        if (loss_baseline.empty()) loss_baseline = hash; else expect(hash == loss_baseline, "automatic producer-loss cancellation equality at 1/2/4/8");
    }
}

void live_execution_restore() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        ga::ExecutionRetainedPlayer row; row.initialized = row.finder_present = true;
        row.next_refresh = 234; row.node_order = {20, 10}; row.maximum_distance = Number(50); row.reservations = {{100, 7}};
        ga::ExecutionStateContributor codec;
        expect(static_cast<bool>(codec.replace({{2, row}})), "live execution saved state stages");
        auto pool = std::make_shared<ga::FreestoreInput>(); pool->pool = std::make_shared<ga::FreestorePool>();
        ga::ExecutionRetainedState retained;
        std::map<gc::PlayerId, ga::ExecutionRoutes> routes;
        bool reject{};
        ga::LiveExecutionStateContributor live(retained, routes, pool, [&](auto player, const auto*) {
            if (reject) { eawr::core::Diagnostic error; error.code = "test.route"; return eawr::core::Result<ga::ExecutionRouteInput>::failure(error); }
            ga::ExecutionRouteInput input; input.player = player; input.maximum_distance = Number(0);
            input.planets = {{10, {}, {}, {}, {}, true}, {20, {whole(10), {}, {}}, {}, {}, {}, true}};
            return eawr::core::Result<ga::ExecutionRouteInput>::success(std::move(input));
        }, [](const auto&) { return eawr::core::Result<void>::success(); }, executor);
        expect(static_cast<bool>(live.restore(take(codec.capture()))), "live execution restores native identities then rebuilds derived matrices");
        expect(retained.at(2) == row && routes.at(2).next_refresh == 234 && routes.at(2).available
            && pool->pool->reservations == row.reservations, "saved node config/deadline/reservations reach real consumers before first service");
        expect(routes.at(2).rows.size() == 4 && !routes.at(2).rows[1].empty, "derived rebuild uses saved max distance rather than new default");
        expect(routes.at(2).paths && routes.at(2).paths->nodes == row.node_order
            && take(ga::cached_execution_path(routes.at(2), 2, 10, 20)).planets == std::vector<gc::PlanetId>({10, 20}),
            "WGA-139/389: derived restore publishes full predecessor matrix in saved node order before continuation");
        const auto old_paths = routes.at(2).paths;
        const auto old_pool = pool->pool;
        const auto before = take(live.state_hash());
        reject = true;
        expect(!live.restore(take(codec.capture())) && pool->pool == old_pool && routes.at(2).paths == old_paths && take(live.state_hash()) == before,
            "failed derived route rebuild publishes no partial reservation/config state");
        if (baseline.empty()) baseline = before; else expect(before == baseline, "live execution native restore equality on 1/2/4/8");
    }
}

void native_execution_state() {
    ga::ExecutionStateContributor source;
    ga::ExecutionRetainedPlayer row;
    row.initialized = row.finder_present = true;
    row.next_refresh = 234;
    row.node_order = {20, 10};
    row.maximum_distance = Number::from_repr(0x4025000000000000ULL);
    row.reservations = {{100, 7}, {101, 8}};
    expect(static_cast<bool>(source.replace({{2, row}})), "execution native state accepts retained config");
    const auto saved = source.capture().value();
    const auto original_hash = source.state_hash().value();
    expect(saved.components.size() == 1 && saved.components.contains("galactic-execution"), "execution contributes one versioned native component");
    ga::ExecutionStateContributor unavailable;
    expect(!unavailable.restore(saved), "execution restore requires reconstructed native identities");
    const auto reference_validator = [](const ga::ExecutionRetainedState& state) {
        for (const auto& [player, retained] : state) {
            bool valid = player == 2;
            for (const auto node : retained.node_order) valid = valid && (node == 10 || node == 20);
            for (const auto& [object, plan] : retained.reservations)
                valid = valid && ((object == 100 && plan == 7) || (object == 101 && plan == 8));
            if (!valid) {
                eawr::core::Diagnostic error; error.code = "test.reference"; error.message = "unresolved restored identity";
                return eawr::core::Result<void>::failure(std::move(error));
            }
        }
        return eawr::core::Result<void>::success();
    };
    ga::ExecutionStateContributor restored(reference_validator);
    auto envelope = saved;
    envelope.components["other-lane"] = {1, 2, 3};
    expect(static_cast<bool>(restored.restore(envelope)) && restored.state() == source.state(), "execution round trip preserves node query order, raw distance, deadline and reservation ownership");
    expect(restored.state_hash().value() == original_hash, "component hash excludes unrelated native components");
    auto bytes = saved.components.at("galactic-execution");
    for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
        auto truncated = saved;
        truncated.components["galactic-execution"].resize(prefix);
        expect(!restored.restore(truncated) && restored.state_hash().value() == original_hash,
            "truncated execution component restore is transactional");
    }
    auto invalid = saved; invalid.components["galactic-execution"].push_back(0);
    expect(!restored.restore(invalid), "execution component rejects trailing bytes");
    invalid = saved; invalid.components["galactic-execution"][4 + ga::ExecutionStateContributor::component.size()] = 2;
    expect(!restored.restore(invalid), "execution component refuses an unsupported version");
    invalid = saved; invalid.version = 2;
    expect(!restored.restore(invalid), "execution component refuses an unsupported native envelope");
    invalid = saved; invalid.components.erase("galactic-execution");
    expect(!restored.restore(invalid), "execution component must be present");
    auto bad_row = row; bad_row.node_order = {10, 10};
    expect(!restored.replace({{2, bad_row}}), "execution node identities must be unique");
    bad_row = row; bad_row.maximum_distance = Number::from_repr(0x7ff0000000000000ULL);
    expect(!restored.replace({{2, bad_row}}), "execution retained distance must be finite");
    bad_row = row; bad_row.reservations.emplace(102, 0);
    expect(!restored.replace({{2, bad_row}}), "execution reservation requires potential-plan identity");
    expect(restored.state_hash().value() == original_hash, "failed validation preserves retained native state");
    bad_row = row; bad_row.reservations[101] = 9;
    expect(!restored.replace({{2, bad_row}}) && restored.state_hash().value() == original_hash,
        "unknown restored potential-plan identity rejects the complete publish");
    ga::ExecutionStateContributor validated([](const auto&) {
        eawr::core::Diagnostic error; error.code = "test.reference"; error.message = "missing reconstructed reference";
        return eawr::core::Result<void>::failure(std::move(error));
    });
    expect(!validated.restore(saved) && validated.state().empty(), "campaign reference fixup runs before execution state commit");
    row.next_refresh = 235;
    expect(static_cast<bool>(restored.replace({{2, row}})) && restored.state_hash().value() != original_hash,
        "route refresh deadline participates in whole-game state hash");
}

void routes() {
    ga::ExecutionRoutes state;
    std::uint64_t calls = 0;
    const ga::RouteProducer producer = [&](const auto&) {
        ++calls;
        return eawr::core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::success(
            std::vector<ai::GalacticExecutionRoute>{{2, 20, 10, false, Number(2), Number(7), Number(0)},
                {2, 10, 20, false, Number(2), Number(7), Number(0)}});
    };
    const eawr::sim::InlineExecutor executor;
    expect(!ga::refresh_execution_routes(state, 0, false, executor, producer).value() && !calls,
        "WGA-77: no refresh outside due execution");
    expect(ga::refresh_execution_routes(state, 0, true, executor, producer).value() && calls == 1 && state.next_refresh == 90,
        "WGA-77: zero deadline refresh rearms at mode frame plus ninety");
    expect(state.rows.front().source == 10 && ga::execution_connectivity(state).execution_available,
        "B route DTO committed in canonical player/source/candidate order");
    expect(!ga::refresh_execution_routes(state, 89, true, executor, producer).value(), "path deadline is independent of ordinary execution frequency");
    expect(!ga::refresh_execution_routes(state, 90, false, executor, producer).value(), "inclusive deadline still requires due execution");
    expect(ga::refresh_execution_routes(state, 90, true, executor, producer).value() && calls == 2,
        "WGA-77: inclusive deadline refreshes");
    const auto saved_revision = state.revision;
    const auto bad = [&](const auto&) {
        return eawr::core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::success(
            std::vector<ai::GalacticExecutionRoute>{{2, 10, 20}, {2, 10, 20}});
    };
    expect(!ga::refresh_execution_routes(state, 180, true, executor, bad) && state.revision == saved_revision
        && state.next_refresh == 180, "malformed route generation leaves prior publication and deadline intact");
    expect(ga::refresh_execution_routes(state, 180, true, executor, {}).value() && !state.available && state.rows.empty(),
        "unqualified route producer publishes unavailable rather than fabricated empty or safe routes");
}
void packed_routes() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto position = [](std::int64_t x) { return eawr::sim::math::Vec3{eawr::sim::math::Fixed::from_integer(x).value(), {}, {}}; };
        ga::ExecutionRouteInput input; input.player = 2; input.maximum_distance = Number(11);
        input.planets = {{10, position(0), {}, Number(900), {}, true}, {20, position(10), {}, Number(149), {}, true},
            {30, position(20), {}, Number(150), {}, true}, {40, position(200), {}, Number(0), {}, true}};
        auto built = ga::build_execution_routes(input, executor).value();
        const auto route = [](const auto& table, gc::PlanetId source, gc::PlanetId dest) -> const ai::GalacticExecutionRoute& {
            return *std::find_if(table.rows.begin(), table.rows.end(), [&](const auto& row) { return row.source == source && row.candidate == dest; });
        };
        const auto& path = route(built, 10, 30);
        expect(!path.empty && path.distance == Number(20) && path.threat == Number(200) && path.prefix_threat == Number(100),
            "WGA-126..140: source threat excluded, destination entered, prefix omits final entered planet");
        expect(route(built, 10, 10).distance == Number(0) && route(built, 10, 10).threat == Number(0), "execution same-planet decoded cost is zero");
        expect(route(built, 10, 40).empty && route(built, 10, 40).threat == Number(6553500), "disconnected path retains all-ones decoded fallback");
        expect(built.edges_tested == 16 && built.relaxations_tested == 64, "deterministic execution matrix work budget");
        std::ostringstream trace;
        for (const auto& row : built.rows) trace << row.source << ':' << row.candidate << ':' << row.empty << ':' << row.distance.repr << ':' << row.threat.repr << ':' << row.prefix_threat.repr << ';';
        if (baseline.empty()) baseline = trace.str(); else expect(trace.str() == baseline, "packed execution route equality on one/two/four/eight workers");
        gc::Route trade; trade.id = 100; trade.from = 10; trade.to = 40; trade.hs_speed_factor = eawr::sim::math::Fixed::from_integer(2).value();
        input.trade_routes = {trade}; built = ga::build_execution_routes(input, executor).value();
        expect(!route(built, 10, 40).empty && route(built, 10, 40).distance == Number(100), "trade-route distance scales by HS factor without proximity gate");
        input.planets[3].usable = false; built = ga::build_execution_routes(input, executor).value();
        expect(route(built, 10, 40).empty, "trade routes require both AI-usable endpoints");
        input.planets[0].layer_z_adjust = Number(10); built = ga::build_execution_routes(input, executor).value();
        expect(route(built, 10, 20).empty && !route(built, 20, 10).empty, "source layer adjustment produces directed proximity eligibility");
        ga::ExecutionRoutes retained;
        const ga::RouteProducer producer = [&](const auto& e) {
            auto result = ga::build_execution_routes(input, e);
            if (!result) return eawr::core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::failure(result.error());
            return eawr::core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::success(std::move(result.value().rows));
        };
        expect(static_cast<bool>(ga::initialize_execution_routes(retained, 7, 1, 0, executor, producer)) && retained.next_refresh == 97,
            "execution initialization computes immediately and lone nonhuman schedules plus ninety");
        expect(!ga::refresh_execution_routes(retained, 7, true, executor, producer).value(), "initialization avoids duplicate zero-deadline refresh");
        expect(static_cast<bool>(ga::initialize_execution_routes(retained, 7, 3, 5, executor, producer)) && retained.next_refresh == 157,
            "execution initialization uses actual player index, not dense nonhuman ordinal");
        retained.next_refresh = 234;
        const auto revision = retained.revision;
        expect(static_cast<bool>(ga::rebuild_execution_routes_after_load(retained, true, executor, producer)) && retained.revision == revision,
            "WGA-139: present finder does not recompute on load");
        expect(static_cast<bool>(ga::rebuild_execution_routes_after_load(retained, false, executor, producer)) && retained.next_refresh == 234
            && retained.revision == revision + 1, "WGA-139: absent finder rebuild preserves restored deadline");
    }
}
void admission() {
    ga::FreestoreObject row; row.id = 7; row.type = 11; row.owner = 2; row.planet = 10;
    expect(ga::freestore_eligible(row, 2), "whole owned unit enters freestore");
    expect(!ga::freestore_eligible(row, 1), "WGA-28: foreign-owned unit excluded");
    for (const auto kind : {ga::FreestoreKind::planet, ga::FreestoreKind::member,
        ga::FreestoreKind::ground_structure, ga::FreestoreKind::space_structure}) {
        row.kind = kind; expect(!ga::freestore_eligible(row, 2), "WGA-28/61: excluded semantic object kind");
    }
    row.kind = ga::FreestoreKind::fleet; row.directly_parented_to_planet = true;
    expect(!ga::freestore_eligible(row, 2), "WGA-28: planet-parented ordinary fleet excluded");
    row.kind = ga::FreestoreKind::unit; row.directly_parented_to_planet = false;
    ga::FreestorePool pool; pool.objects.push_back(row); pool.reservations[7] = 99;
    expect(ga::freestore_assignment(pool, 7, 2, 98) == ga::FreestoreAssignment::failed, "WGA-29: other potential-plan reservation fails");
    expect(ga::freestore_assignment(pool, 7, 2, 99) == ga::FreestoreAssignment::assigned, "WGA-29: reserving plan assigns stationary unit");
    pool.objects[0].transit = true;
    expect(ga::freestore_assignment(pool, 7, 2, 99) == ga::FreestoreAssignment::waiting, "WGA-29: transit waits even with a retained source planet");
    pool.objects[0].transit = false; pool.objects[0].planet = 0;
    expect(ga::freestore_assignment(pool, 7, 2, 99) == ga::FreestoreAssignment::waiting, "WGA-29: unlocated reserved unit waits");
    expect(ga::freestore_assignment(pool, 8, 2, 99) == ga::FreestoreAssignment::failed, "lost identity fails build assignment");
}
void safety() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        gc::PoliticalControlSetup control; control.max_ground_forces = 1;
        gc::ControlPlanet surface; surface.planet = 10; surface.surface_accessible = true;
        surface.landed = {{8, 11, 2}}; control.planets.push_back(surface);
        gc::Fleet fleet; fleet.id = 5; fleet.owner = 2; fleet.orbit = 10; fleet.members = {{7, 11}};
        const std::vector<ga::FreestoreType> traits{{11, ga::FreestoreKind::unit, true}};
        const auto copy = [&](gc::PoliticalControlSetup input, gc::Fleet presence) {
            gc::GalacticSnapshot snapshot(0, {}, {{2, 2, 0}}, {{10, {}, 2}}, {}, {presence}, {}, {}, {}, {}, {}, {}, {}, {}, input);
            return ga::copy_freestore(snapshot, traits, executor).value();
        };
        auto pool = copy(control, fleet);
        const auto transport = [&](const auto& p) -> const ga::FreestoreObject& {
            return *std::find_if(p.objects.begin(), p.objects.end(), [](const auto& r) { return r.id == 7; });
        };
        expect(transport(pool).safe == false, "WGA-30: full authored surface limit rejects orbiting transport safety");
        control.planets[0].landed.clear(); pool = copy(control, fleet);
        expect(transport(pool).safe == true, "WGA-30: allied accessible surface below limit is safe");
        control.planets[0].surface_accessible = false; pool = copy(control, fleet);
        expect(transport(pool).safe == false, "WGA-30: inaccessible surface is unsafe");
        fleet.motion.transit = gc::FleetTransit{}; pool = copy(control, fleet);
        expect(transport(pool).safe == true && !transport(pool).planet && transport(pool).transit,
            "WGA-30: transit safe overrides stationary surface gates and clears containing planet");
        const auto trace = std::to_string(pool.work.objects) + ':' + std::to_string(pool.work.safety_rows)
            + ':' + std::to_string(transport(pool).safe.value());
        if (baseline.empty()) baseline = trace; else expect(trace == baseline, "freestore safety/work equality on one/two/four/eight workers");
    }
}
void lua_callbacks() {
    auth::ModuleManifest manifest;
    expect(static_cast<bool>(manifest.add("Contract.lua", R"(
function Base_Definitions()
    FreeStore = nil
    PlayerObject = nil
    ServiceRate = 8
    UnitServiceRate = 2
    Trace = ""
end
function main()
    while true do Trace = Trace .. "main;" coroutine.yield(true) end
end
function On_Unit_Service(object) Trace = Trace .. "unit;" end
function ReadCounts() Count = FreeStore.Get_Object_Count() end
)")), "numeric-rate callback contract manifest mounts");
    auth::SessionConfig config; config.galactic_clock = true;
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
    auto input = std::make_shared<ga::FreestoreInput>();
    auto pool = std::make_shared<ga::FreestorePool>();
    ga::FreestoreObject row; row.id = 7; row.owner = 2; row.type = 11; row.planet = 10;
    pool->objects.push_back(row); input->pool = pool;
    expect(static_cast<bool>(ga::register_freestore_bindings(scripts, input, true)), "callback bindings install");
    expect(static_cast<bool>(scripts.create_instance(1002, "Contract.lua")), "callback fixture loads");
    const ga::FreestoreAttachment attachment{2, 1002};
    expect(static_cast<bool>(ga::attach_freestore(scripts, attachment, true)), "callback fixture attaches");
    auth::ServiceOptions pacing; pacing.host_paced = [](auto) { return true; };
    const eawr::sim::InlineExecutor executor;
    expect(scripts.service(executor, pacing).value().diagnostics.empty(), "definitions/thread creation service succeeds");
    expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *pool, 60, nullptr, true)), "numeric unit equality boundary queues");
    expect(scripts.service(executor, pacing).value().diagnostics.empty(), "equality boundary succeeds");
    const auto trace = [&] { return std::get<std::string>(scripts.read_global(1002, "Trace").value()->data); };
    expect(trace().empty(), "numeric unit equality boundary does not invoke callback");
    expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *pool, 61, nullptr, true)), "numeric unit callback queues after equality");
    expect(scripts.service(executor, pacing).value().diagnostics.empty() && trace() == "unit;", "unit rate is independent of main rate for an unreserved pool member");
    expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *pool, 241, nullptr, true)), "both due callbacks queue");
    expect(scripts.service(executor, pacing).value().diagnostics.empty() && trace() == "unit;main;unit;", "main pump precedes same-service numeric unit callback");
    pool->reservations[7] = 99;
    expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *pool, 302, nullptr, true)), "reserved unit callback period queues");
    expect(scripts.service(executor, pacing).value().diagnostics.empty() && trace() == "unit;main;unit;",
        "periodic unit service skips reserved members while retaining pool membership");
    auth::ScriptEvent read; read.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1002, 0};
    read.target = 1002; read.kind = auth::ScriptEvent::Kind::call; read.name = "ReadCounts";
    expect(static_cast<bool>(scripts.submit_event(read)), "count observer queues");
    expect(scripts.service(executor, pacing).value().diagnostics.empty()
        && std::get<Number>(scripts.read_global(1002, "Count").value()->data) == Number(1), "potential reservation remains separate from pool membership/count");
    auto updated = std::make_shared<ga::FreestorePool>(*pool); updated->objects[0].owner = 1; input->pool = updated;
    read.key.tick = scripts.completed_tick() + 1;
    expect(static_cast<bool>(scripts.submit_event(read)), "updated pool count observer queues");
    expect(scripts.service(executor, pacing).value().diagnostics.empty()
        && std::get<Number>(scripts.read_global(1002, "Count").value()->data) == Number(0), "ordered snapshot replacement reaches existing Lua binding without re-registration");
    const auto before = trace();
    expect(static_cast<bool>(ga::attach_freestore(scripts, attachment, false)), "load route reattaches mappings");
    expect(scripts.service(executor, pacing).value().diagnostics.empty() && trace() == before, "load attachment retains restored thread and avoids fresh definitions");
}
std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* text{}; std::size_t size{};
    if (_dupenv_s(&text, &size, "EAWR_EAW_GAME_ROOT") || !text) return {};
    std::string result(text); std::free(text); return result.empty() ? std::nullopt : std::optional{result};
#else
    const auto* text = std::getenv("EAWR_EAW_GAME_ROOT"); return text && *text ? std::optional{std::string(text)} : std::nullopt;
#endif
}
std::string read(const eawr::vfs::Vfs& filesystem, std::string_view path) {
    const auto bytes = filesystem.open(path).value();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string installed_immediate(const cp::CampaignSetupServices& live, const gc::GalacticSetup& setup,
    gc::PlayerId player, gc::PlanetId planet, gc::TypeId type, const eawr::sim::PartitionExecutor& executor) {
    auto world = take(gc::GalacticSession::create(setup));
    auto funding = std::make_shared<ai::GalacticFunding>();
    const auto credits = [&] {
        for (const auto& row : world.snapshot()->player_economy()->players) if (row.player == player) return row.credits;
        throw std::runtime_error("installed immediate account missing");
    };
    ai::EvaluatedFundingTemplate policy; policy.equations = {{1, whole(1)}};
    const auto initial_credits = credits();
    expect(static_cast<bool>(funding->initialize(player, std::span(&policy, 1), initial_credits)), "installed immediate native ledger initializes");
    std::map<gc::PlayerId, Fixed> multipliers;
    for (const auto& row : setup.fiscal->players) multipliers.emplace(row.player, row.positive_credit_multiplier);
    ga::NativeActions actions(world, funding, {}, multipliers, {}, &executor);
    actions.bind_handlers();
    // Retail-qualified Normal assistance is exercised from zero cash; the
    // selected installed account, cap, multiplier and original types are real.
    (void)take(world.add_credits(player, whole(-1000000), whole(1)));
    auth::ModuleManifest manifest;
    expect(static_cast<bool>(manifest.add("Immediate.lua", R"(
Player = SelfPlayer()
Force = SelfForce()
Stage = SelfStage()
function Failed()
    Player.Give_Money(6000)
    Force.Produce_Force(Stage)
    assert(Force.Get_Force_Count() == 1)
    error('discard native cash, placement, taskforce and identities')
end
function Success()
    Before = Player.Get_Credits()
    Player.Give_Money(6000)
    After = Player.Get_Credits()
    Block = Force.Produce_Force(Stage)
    Count = Force.Get_Force_Count()
    Unit = Force.Get_Unit_Table()[1]
    assert(Count == 1 and Force.Is_Valid() and Unit.Is_Valid())
    Type = Unit.Get_Type()
    Owner = Unit.Get_Owner()
    Position = Unit.Get_Position()
    Finished = Block.IsFinished()
    assert(not Finished)
    Repeat = Force.Build_All(Stage)
    assert(Force.Get_Force_Count() == 1)
end
function ObserveCompletion()
    Completed = Block.IsFinished()
    Result = Block.Result()
end
)")), "installed immediate Lua stock-amount observer mounts");
    auth::SessionConfig config; config.tick_duration = {1, 30}; config.galactic_clock = true;
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
    auto host = std::make_shared<foc::detail::Host>();
    auto completed_world = std::make_shared<ai::GalacticWorld>(ai::GalacticPerceptionSetup{});
    take(completed_world->bind(world.snapshot(), {}, executor, world.completed_tick(), world.next_entity_id()));
    host->ai_world = std::move(completed_world); // explicit Produce_Force staging reads the actual completed world
    host->view = std::make_shared<foc::detail::WorldView>();
    ai::PlanDef definition; definition.name = "ImmediateMagicFixture"; definition.magic = true;
    ai::Engine engine(host, {}, {definition}); host->engine = &engine;
    const auto instance = ai::first_plan_instance;
    ai::EngineContractAccess::magic_fixture(engine, player, instance, planet, type);
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts, host, errors);
    foc::tactical_ai_detail::register_methods(scripts, host, errors);
    expect(errors.empty(), "installed actual player, taskforce and object methods register");
    const auto bind_handle = [&](std::string name, auth::Handle handle) {
        expect(static_cast<bool>(scripts.register_binding(name, [handle](auth::BindingContext&, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value{handle}});
        })), "installed fixture identity binding registers");
    };
    bind_handle("SelfPlayer", {foc::handle_player, player});
    bind_handle("SelfForce", {ai::handle_taskforce, 1});
    bind_handle("SelfStage", {ai::handle_ai_target, 1});
    expect(static_cast<bool>(scripts.set_immediate_effect_host(actions.immediate_host(
        [source = live.stock_starting_forces](const auto& request, const auto& view, auto frontier) {
            return cp::stock_native_force_shadow_builder(source, {request.player, request.target, request.type, {}})(view, frontier);
        }, 8), 1)), "installed bounded native shadow provider registers");
    expect(static_cast<bool>(scripts.create_instance(instance, "Immediate.lua")), "installed immediate observer loads");
    const auto deliver = [&](std::string name) {
        auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, instance, 0};
        event.target = instance; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name);
        expect(static_cast<bool>(scripts.submit_event(event)), "installed native observer submits");
        return take(scripts.service(executor));
    };
    const auto original = world.state_sha256(); const auto ledgers = funding->state(); const auto frontier = world.next_entity_id();
    auto rejected = deliver("Failed");
    expect(rejected.diagnostics.size() == 1 && rejected.commands.empty() && world.state_sha256() == original
        && funding->state() == ledgers && world.next_entity_id() == frontier && engine.taskforce(1)->types == std::vector<gc::TypeId>{type},
        "installed failed handler discards real cash, windfall, placement, taskforce bookkeeping and frontier");
    auto accepted = deliver("Success");
    expect(accepted.diagnostics.empty(), "installed immediate success has no unsupported/missing diagnostics");
    const auto lua = [&](std::string_view name) { return *take(scripts.read_global(instance, name)); };
    const auto actual = credits();
    expect(std::get<Number>(lua("Before").data) == Number(0) && std::get<Number>(lua("After").data) == Number(6000)
        && actual == whole(6000),
        "installed Lua Give_Money returns before the very next Get_Credits observes real stock credits");
    const auto actor = std::get<auth::Handle>(lua("Unit").data).id;
    expect(actor >= frontier && std::get<auth::Handle>(lua("Type").data) == auth::Handle{foc::handle_type, type}
        && std::get<auth::Handle>(lua("Owner").data) == auth::Handle{foc::handle_player, player},
        "installed same-call magic handle resolves real original type and requested owner");
    expect(world.next_entity_id() == frontier + 2 && !std::get<bool>(lua("Finished").data),
        "same-call repeated magic production does not allocate twice or finish the block early");
    auto reference = take(gc::GalacticSession::create(setup));
    auto reference_funding = std::make_shared<ai::GalacticFunding>();
    expect(static_cast<bool>(reference_funding->initialize(player, std::span(&policy, 1), initial_credits)), "direct native reference ledger initializes");
    ga::NativeActions direct(reference, reference_funding,
        [source = live.stock_starting_forces](const auto& request, const auto& snapshot, auto next) {
            return cp::stock_native_force_builder(source, {request.player, request.target, request.type, {}})(snapshot, next);
        }, multipliers, {}, &executor);
    direct.bind_handlers();
    (void)take(reference.add_credits(player, whole(-1000000), whole(1)));
    ga::ActionRequest request; request.player = player; request.kind = ga::ActionKind::give_money; request.amount = Number(6000);
    expect(take(direct.admit(request)).admitted, "direct reference applies the same stock cash");
    request.kind = ga::ActionKind::magic; request.target = planet; request.type = type;
    expect(take(direct.admit(request)).admitted && reference.state_sha256() == world.state_sha256()
        && reference_funding->state() == funding->state() && take(direct.state_hash()) == take(actions.state_hash()),
        "sparse local placement and grouped native publication equal direct E world/funding semantics");
    expect(static_cast<bool>(engine.after_service(accepted)) && accepted.commands.empty()
        && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{actor}, "committed native actors enter the existing task owner without a second spawn");
    ai::EngineContractAccess::complete_native(engine);
    auto completed = deliver("ObserveCompletion");
    expect(completed.diagnostics.empty() && std::get<bool>(lua("Completed").data) && std::get<bool>(lua("Result").data),
        "native task completion later finishes the ordinary production block");
    expect(actions.immediate_work().requests == 2 && actions.immediate_work().copied_scene_objects == 0,
        "installed shadow uses one cash write and one local placement, with no scene clone");
    const auto& work = actions.immediate_work();
    std::ostringstream trace;
    trace << world.state_sha256() << take(actions.state_hash()) << scripts.state_digest()
        << work.requests << ':' << work.copied_accounts << ':' << work.copied_planets << ':'
        << work.copied_orbital_objects << ':' << work.prepared_objects << ':' << work.copied_scene_objects;
    return trace.str();
}

std::string installed_actions(const cp::CampaignSetupPlan& plan, const cp::CampaignSetupServices& live,
    const gc::GalacticSetup& setup, gc::PlayerId player, const eawr::sim::PartitionExecutor& executor) {
    // This is real stock native admission/placement, with a harness-owned C
    // reservation. D's natural activation and immediate Lua routing remain
    // composition seams; the retail-qualified Normal location is not a forced
    // positive equation binding (WGA-142).
    auto world = take(gc::GalacticSession::create(setup));
    auto funding = std::make_shared<ai::GalacticFunding>();
    const auto credits = [&] {
        for (const auto& row : world.snapshot()->player_economy()->players) if (row.player == player) return row.credits;
        throw std::runtime_error("installed action account missing");
    };
    ai::EvaluatedFundingTemplate policy; policy.equations = {{1, whole(1)}};
    expect(static_cast<bool>(funding->initialize(player, std::span(&policy, 1), credits())), "installed paid admission initializes real C ledger from stock account");
    expect(static_cast<bool>(funding->reserve(player, {7, 1, credits(), {}, {}, {}})), "installed harness allocates C reservation, not a tactical wallet");
    std::map<gc::PlayerId, Fixed> multipliers;
    for (const auto& row : setup.fiscal->players) multipliers.emplace(row.player, row.positive_credit_multiplier);
    ga::NativeActions actions(world, funding, [source = live.stock_starting_forces](const auto& request, const auto& snapshot, auto frontier) {
        return cp::stock_native_force_builder(source, {request.player, request.target, request.type, {}})(snapshot, frontier);
    }, multipliers, {}, &executor, [source = live.stock_starting_forces](auto id) {
        for (const auto& binding : source->bindings) if (binding.id == id) {
            const auto* type = source->types.find(binding.original_type);
            if (type && type->build_cost_credits) return eawr::core::Result<Fixed>::success(whole(type->build_cost_credits->trunc_to_integer()));
        }
        eawr::core::Diagnostic error; error.code = "test.original-cost"; return eawr::core::Result<Fixed>::failure(error);
    });
    actions.bind_handlers();
    ga::ActionReceipt accepted; ga::ActionRequest request;
    request.kind = ga::ActionKind::produce; request.player = player; request.plan = 42;
    const auto initial_credits = credits();
    for (const auto& producer : setup.production->planets) {
        if (producer.target_owner != player) continue;
        for (const auto& offer : producer.offers) {
            if (offer.kind != gc::ProductionKind::unit || !offer.direct_build_allowed) continue;
            auto adjusted = gc::prepared_production_offer(offer, world.snapshot()->income() ? &world.snapshot()->income()->inputs : nullptr,
                world.snapshot()->planets(), producer.planet, player);
            if (!adjusted || adjusted.value().price > initial_credits || adjusted.value().seconds > whole(120)) continue;
            request.target = producer.planet; request.type = offer.type;
            auto receipt = actions.admit_paid(request, 7);
            if (receipt && receipt.value().admitted) { accepted = receipt.value(); break; }
        }
        if (accepted.admitted) break;
    }
    expect(accepted.admitted, "installed actual AI paid producer admits a stock unit");
    if (!accepted.admitted) throw std::runtime_error("no stock native paid candidate");
    const auto price = actions.purchases().at(accepted.request).price;
    const auto paid_type = request.type;
    expect(credits() == initial_credits && funding->ledger(player)->goals.at(7).marked == actions.purchases().at(accepted.request).marked,
        "installed producer marks once and preserves balance at admission");
    auto same_type_magic = request; same_type_magic.kind = ga::ActionKind::magic;
    const auto mark_before_magic = funding->ledger(player)->goals.at(7).marked;
    expect(take(actions.admit(same_type_magic)).admitted, "direct stock magic placement accepts a type also being paid-produced");
    bool queued{};
    for (const auto& producer : world.snapshot()->production()->planets) for (const auto& queue : producer.queues)
        for (const auto& entry : queue) queued |= entry.build_id == accepted.request;
    expect(queued && credits() == initial_credits && funding->ledger(player)->goals.at(7).marked == mark_before_magic,
        "magic scheduled removal does not cancel paid entries, refund cash or change marks");
    std::uint64_t frames{};
    bool complete{};
    while (!complete && frames < 4000) {
        auto tick = take(world.step(executor)); ++frames;
        for (const auto& event : tick.snapshot->events()) if (event.kind == gc::GalacticEventKind::production_completed && event.sequence == accepted.request) complete = true;
        if (frames == 2) {
            const auto saved = take(actions.capture());
            expect(static_cast<bool>(actions.restore(saved)), "installed paid mapping restores against current real queue/ledger");
            actions.bind_handlers();
        }
    }
    const auto billed = actions.purchases().at(accepted.request).billed;
    expect(complete && billed < price, "installed real paid production completes without a final catch-up bill");
    expect(funding->ledger(player)->goals.at(7).marked == std::max(Fixed{}, take(eawr::sim::math::subtract(actions.purchases().at(accepted.request).marked, billed))), "installed paid completion retains residual C mark");
    request.kind = ga::ActionKind::give_money; request.amount = Number(-1000000);
    expect(take(actions.admit(request)).admitted && credits() == Fixed{}, "native negative Give_Money accepts and clamps to zero");
    const auto before_magic = world.next_entity_id();
    request.kind = ga::ActionKind::magic;
    request.target = 0; request.type = 0;
    for (const auto& location : plan.locations()) if (location.original_type == "Kamino" && location.planet) request.target = location.planet->id;
    for (const auto& binding : plan.type_bindings()) if (const auto* type = plan.original_type(binding.id); type && type->name == "Victory_Destroyer") request.type = binding.id;
    if (!request.target || !request.type) throw std::runtime_error("retail-qualified Normal native magic bindings absent");
    auto magic = take(actions.admit(request));
    expect(magic.admitted && actions.created().size() == 1 && magic.request >= before_magic && credits() == Fixed{},
        "retail-qualified Normal location directly creates one real stock ship at zero cash without paid queues");
    bool owner{};
    for (const auto& fleet : world.snapshot()->fleets()) for (const auto& member : fleet.members) if (member.id == magic.request)
        owner = fleet.owner == player && fleet.orbit == request.target && member.type == request.type;
    expect(owner, "magic placement retains requesting plan player and actual staging planet");
    const auto immediate = installed_immediate(live, setup, player, request.target, request.type, executor);
    const auto no_mutation = world.state_sha256();
    request.type = std::numeric_limits<gc::TypeId>::max();
    expect(!actions.admit(request) && world.state_sha256() == no_mutation, "invalid native original type creates no partial placement");
    request.kind = ga::ActionKind::give_money; request.amount = Number(6000);
    expect(take(actions.admit(request)).admitted && credits() == whole(6000), "stock Normal native cash assistance applies selected multiplier");
    request.amount = Number(-100);
    expect(take(actions.admit(request)).admitted && credits() == whole(5900), "negative native cash bypasses positive multiplier");
    request.amount = Number(1000000000);
    // WGEP-17: max(1, owned planets) * authored Credit_Cap_Per_Planet.
    // WGA-144's 80000 is its two-planet retail fixture, not this full stock campaign.
    const auto owned = std::count_if(world.snapshot()->planets().begin(), world.snapshot()->planets().end(),
        [player](const auto& planet) { return planet.owner == player; });
    const auto cap = take(eawr::sim::math::multiply(whole(std::max<std::int64_t>(1, owned)), setup.fiscal->credit_cap_per_planet));
    expect(take(actions.admit(request)).admitted && credits() == cap, "stock Empire native cash clamps at ownership-derived galactic cap");
    const gc::GalacticReplay replay{world.setup(), world.snapshot()->completed_tick(), {},
        std::vector<gc::NativeEffect>(world.native_effects().begin(), world.native_effects().end())};
    const auto encoded = take(gc::write_replay(replay));
    auto reconstructed = take(gc::GalacticSession::from_replay(take(gc::parse_replay(encoded))));
    ga::NativeActions::bind_economic_replay_owners(reconstructed,
        [source = live.stock_starting_forces](const auto& effect, const auto& snapshot, auto frontier) {
            return cp::stock_native_force_builder(source, {effect.player, effect.target, effect.type, {}})(snapshot, frontier);
        });
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) (void)take(reconstructed.step(executor));
    take(reconstructed.replay_native_barrier(executor)); take(reconstructed.finish_native_replay());
    expect(reconstructed.state_sha256() == world.state_sha256(),
        "installed paid, authored magic and cash journal rebuilds exact parent without native AI initialization");
    std::cout << "stock native actions frames=" << frames << " paid_type=" << paid_type << " price=" << price.trunc_to_integer() << " magic_actor=" << magic.request << " owned_planets=" << owned << " cash_cap=" << cap.trunc_to_integer() << '\n';
    return world.state_sha256() + take(actions.state_hash()) + immediate;
}
void installed() {
    const auto root = game_root(); if (!root) throw std::runtime_error("installed freestore requires EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Rebel";
    const auto* source = data.campaign(campaign_name); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.local_observer = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> names; for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end()); for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto plan = cp::plan_live_campaign_setup(filesystem, data, types, campaign_name, options, "Normal_Default").value();
    const auto live = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, plan, "Normal_Default").value();
    auto campaign = cp::CampaignRoot::create(plan, live).value();
    auto host = std::make_shared<foc::detail::Host>();
    std::vector<ga::FreestoreType> traits;
    auto input = std::make_shared<ga::FreestoreInput>();
    for (const auto& binding : plan.type_bindings()) {
        const auto* type = plan.original_type(binding.id); if (!type) continue;
        const auto behaves = [&](std::string_view behavior) { return std::find(type->behavior.begin(), type->behavior.end(), behavior) != type->behavior.end(); };
        ga::FreestoreType row; row.type = binding.id; row.transport = behaves("TRANSPORT");
        if (behaves("FLEET")) row.kind = ga::FreestoreKind::fleet;
        else if (behaves("DUMMY_GROUND_STRUCTURE") || behaves("DUMMY_GROUND_BASE")) row.kind = ga::FreestoreKind::ground_structure;
        else if (behaves("DUMMY_STAR_BASE")) row.kind = ga::FreestoreKind::space_structure;
        traits.push_back(row); input->types_by_name.emplace(foc::detail::upper(type->name), binding.id);
    }
    gc::PlayerId player{};
    for (const auto& binding : plan.player_bindings()) {
        host->setup.players.push_back({binding.player.player_id, foc::detail::upper(binding.faction), false, true, {}, false});
        if (binding.faction == "Empire") player = binding.player.player_id;
    }
    if (!player || options.local_observer == player) throw std::runtime_error("stock Empire AI player missing");
    for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        input->pool = std::make_shared<ga::FreestorePool>(ga::copy_freestore(*campaign.game()->galaxy().snapshot(), traits, executor).value());
        std::int64_t ships = 0, ground = 0;
        for (const auto& row : input->pool->objects) if (ga::freestore_eligible(row, player)) { if (row.transport) ++ground; else ++ships; }
        expect(ships > 0 && ground > 0, "installed campaign provides real whole space and ground freestore units");
        auth::ModuleManifest manifest;
        for (const auto path : {"data/scripts/freestore/galacticfreestore.lua", "data/scripts/freestore/galacticherofreestore.lua",
            "data/scripts/library/pgcommands.lua", "data/scripts/library/pgbasedefinitions.lua", "data/scripts/library/pgbase.lua",
            "data/scripts/library/pgdebug.lua", "data/scripts/library/pgaicommands.lua"})
            expect(static_cast<bool>(manifest.add(path, read(filesystem, path))), "unmodified installed stock freestore closure mounts");
        auth::SessionConfig config; config.seed = 42; config.galactic_clock = true;
        config.script_directories = {"data/scripts/library/", "data/scripts/freestore/"};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host, errors);
        foc::tactical_ai_detail::register_methods(scripts, host, errors);
        expect(errors.empty(), "production FoC library bindings install");
        expect(static_cast<bool>(ga::register_freestore_bindings(scripts, input, true)), "real freestore pool bindings install");
        expect(static_cast<bool>(scripts.create_instance(1002, "data/scripts/freestore/galacticfreestore.lua")), "actual stock freestore instance loads");
        const ga::FreestoreAttachment attachment{player, 1002};
        expect(static_cast<bool>(ga::attach_freestore(scripts, attachment, true)), "production fresh freestore attachment");
        auth::ServiceOptions pacing; pacing.host_paced = [](auto) { return true; };
        const auto run = [&] {
            const auto result = scripts.service(executor, pacing).value();
            for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
            expect(result.diagnostics.empty() && result.commands.empty(), "stock freestore service has no missing APIs/stand-ins/queued actions");
        };
        run();
        const auto number = [&](std::string_view name) -> std::optional<Number> {
            const auto value = scripts.read_global(1002, name).value();
            if (value) if (const auto* n = std::get_if<Number>(&value->data)) return *n;
            return {};
        };
        expect(number("ServiceRate") == Number(8) && !number("UnitServiceRate"), "stock authored rate typo remains a nonnumeric Lua callback skip");
        expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *input->pool, 240, nullptr, true)), "freestore equality boundary queues"); run();
        expect(!number("object_count"), "strict elapsed equality does not pump stock main");
        expect(static_cast<bool>(ga::service_freestore(scripts, attachment, *input->pool, 241, nullptr, true)), "first stock main deadline queues"); run();
        expect(number("object_count") == Number(ships + ground) && number("scnt") == Number(ships) && number("gcnt") == Number(ground),
            "actual stock FreeStoreService consumes production whole-unit domain counts");
        expect(number("LastUnitService") == Number(0), "misspelled authored unit rate leaves independent unit stamp unchanged");
        std::ostringstream trace; trace << ships << ':' << ground << ':' << scripts.state_digest()
            << installed_actions(plan, live, campaign.game()->galaxy().setup(), player, executor);
        if (baseline.empty()) baseline = trace.str(); else expect(trace.str() == baseline, "installed stock Lua/pool equality on one/two/four/eight workers");
        std::cout << "stock freestore workers=" << workers << " space=" << ships << " ground=" << ground << '\n';
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else { immediate_cash_contracts(); native_execution_state(); live_execution_restore(); paid_contracts(); routes(); packed_routes(); admission(); safety(); lua_callbacks(); }
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

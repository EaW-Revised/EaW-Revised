#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {


void station_barrier_expiry() {
    for (const auto workers : {1U, 2U, 4U, 6U, 8U}) {
        World world;
        auto& station = world.host->setup.content.types.front();
        station.star_base = true;
        station.base_level = 3;
        auto replacement = station;
        replacement.type_id = 2;
        replacement.name = "REPLACEMENT_STATION";
        replacement.base_level = 5;
        world.host->setup.content.types.push_back(replacement);
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        world.host->setup.schedule.mode = foc::AiSchedule::Mode::faithful;
        world.host->setup.players = {foc::AiPlayer{1, "REBEL", false, true, "STATION"}};
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
        setup.units = {{10, 1, 1, {}}, {20, 2, 1, {}}};
        tactical::EconomyRules economy;
        economy.players = {{1, {}, 20}};
        auto session = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(session.has_value(), "station expiry: tactical session creates");
        if (!session) return;
        world.host->world = &session.value();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"station.xml", "<Root><Level>Variable_Self.BaseLevel</Level></Root>"}}, converters);
        expect(equations.has_value(), "station expiry: equation parses");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        data.players["STATION"] = ai::PlayerType{};
        ai::Engine engine(world.host, std::move(data), {});
        auto scripts = auth::ScriptScheduler::create({}, {});
        expect(scripts.has_value(), "station expiry: scheduler creates");
        if (!scripts) return;
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::uint64_t sequence = 0;
        for (const auto expected : {3, 5}) {
            world.host->snapshot = session.value().snapshot();
            world.host->view = foc::detail::build_view(session.value(), *world.host->snapshot, &executor);
            unsigned calls = 0;
            const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId player, ai::Engine::ExecutionEffects&) {
                ++calls;
                expect(player == 1 && ai::EngineContractAccess::has_prepared_station_levels(engine),
                    "station expiry: real service retains prepared levels inside the barrier");
                expect(engine.input_counts().producer_prepasses == 1 && engine.input_counts().pad_prepasses == 1
                    && engine.input_counts().late_prepasses == 0 && ai::EngineContractAccess::has_prepared_pads(engine),
                    "shared inputs: producer and pad preparation precede the actual player hook");
                expect(engine.evaluate("Level", player, nullptr) == ai::real(expected),
                    "station expiry: barrier query sees the current station level");
                return eawr::core::Result<void>::success();
            };
            expect(engine.before_service(*world.host->ai_world, scripts.value(), sequence, &executor, hook).has_value(),
                "station expiry: real before_service completes");
            expect(calls == 1, "station expiry: execution hook observed the prepared barrier");
            expect(!ai::EngineContractAccess::has_prepared_pads(engine), "shared inputs: barrier pad rows expire before Lua reads");
            expect(!ai::EngineContractAccess::has_prepared_station_levels(engine),
                "station expiry: real before_service expires prepared levels on return");
            expect(session.value().stage_remove(expected == 3 ? 10 : 20).has_value(),
                "station expiry: station removal between services succeeds");
            expect(engine.evaluate("Level", 1, nullptr) == ai::real(expected == 3 ? 5 : 0),
                "station expiry: later query sees removal instead of stale prepared levels");
            expect(scripts.value().service(executor).has_value(), "station expiry: later script service completes");
            // The faithful execution service is due again after three tactical ticks.
            for (unsigned tick = 0; tick < 3; ++tick)
                expect(session.value().step(executor).has_value(), "station expiry: next service frame advances");
        }
    }
}


void shared_station_perception() {
    for (const auto workers : {1U, 2U, 4U, 6U, 8U}) {
        World world;
        auto& station = world.host->setup.content.types.front();
        station.star_base = true;
        station.base_level = 3;
        station.previous_base_levels = {2, 1};
        station.tech_level = 7;
        auto enemy = station;
        enemy.type_id = 2;
        enemy.name = "ENEMY_STATION";
        enemy.base_level = 5;
        world.host->setup.content.types.push_back(enemy);
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        world.view->players = {{1, 0, 0}, {2, 1, 1}, {3, 0, 2}, {4, 1, 3}};
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 0, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 1, 2, eawr::sim::tactical::player_flag_commandable},
            {3, 0, 1, eawr::sim::tactical::player_flag_commandable},
            {4, 1, 2, eawr::sim::tactical::player_flag_commandable}};
        setup.units = {{10, 1, 1, {}}, {20, 2, 2, {}}, {25, 2, 3, {}}};
        eawr::sim::tactical::EconomyRules economy;
        economy.players = {{1, {}, 20}, {2, {}, 20}, {3, {}, 20}, {4, {}, 20}};
        for (auto& player : economy.players) { player.start_tech = 1; player.max_tech = 5; }
        namespace tactical = eawr::sim::tactical;
        const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
        tactical::DurabilityTable health;
        tactical::DurabilityProfile profile;
        profile.type_id = 1;
        profile.max_hull = q(1000);
        profile.hardpoints.resize(10);
        for (std::size_t index = 0; index < 9; ++index) {
            profile.hardpoints[index].destroyable = true;
            profile.hardpoints[index].max_health = q(100);
        }
        health.profiles.push_back(profile);
        auto session = tactical::TacticalSession::create(setup, {}, health, {}, std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(session), "SAE-02: four ledgers share three live stations");
        if (!session) return;
        world.host->world = &session.value();
        world.host->snapshot = session.value().snapshot();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"team.xml", "<Root><Level>Variable_Self.BaseLevel</Level></Root>"}}, converters);
        expect(static_cast<bool>(equations), "SAE-02: shared station perception equation parses");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto level = [&](tactical::PlayerId player) {
            const auto direct = engine.evaluate("Level", player, nullptr);
            const auto prepared = ai::EngineContractAccess::prepared_station_level(engine, player, executor);
            expect(prepared == direct,
                "partitioned station perception matches live queries after damage, content edits and removals");
            expect(engine.evaluate("Level", player, nullptr) == direct,
                "clearing the test preparation restores uncached live station queries");
            return prepared;
        };
        for (const auto player : {1U, 3U})
            expect(level(player) == ai::real(3),
                "SAE-02: both teammates read the first station, excluding higher allied and enemy levels");
        for (const auto player : {2U, 4U})
            expect(level(player) == ai::real(5),
                "SAE-02: the opposing team's second player also sees its shared station");
        auto& first = world.host->setup.content.types.front();
        first.capture_point = true;
        expect(level(1) == ai::real(5),
            "SAE-02: a capture-point station is skipped for the next qualifying ally");
        first.capture_point = false;
        first.unit_ai = false;
        expect(level(3) == ai::real(5),
            "SAE-02: a non-UnitAI station does not enter allied station perception");
        first.unit_ai = true;
        const auto configuration = foc::tactical_ai_detail::native_configuration(world.host->setup);
        auto changed = world.host->setup;
        changed.content.types.front().previous_base_levels = {1};
        const auto changed_configuration = foc::tactical_ai_detail::native_configuration(changed);
        expect(configuration && changed_configuration && configuration.value() != changed_configuration.value(),
            "SAE-02: predecessor levels participate in the native configuration identity");
        first.base_level = 12;
        expect(level(1) == ai::real(12),
            "SAE-02: an undersized hardpoint layout retains its authored level");
        first.base_level = 3;
        for (std::uint32_t index = 0; index < 9; ++index) {
            const auto frame = session.value().completed_tick();
            expect(session.value().submit({{frame, 1, index}, {10}, tactical::DamagePayload{q(100), index}}).has_value(),
                "SAE-02: targeted station hardpoint damage is admitted");
            expect(session.value().step(executor).has_value(), "SAE-02: station damage commits on every worker count");
            world.host->snapshot = session.value().snapshot();
            world.host->view = foc::detail::build_view(session.value(), *world.host->snapshot, &executor);
            const auto expected = index == 8 ? 0 : 3 - (3 * (index + 1)) / 9;
            for (const auto player : {1U, 3U})
                expect(level(player) == ai::real(expected),
                    "SAE-02: only destroyed destroyable hardpoints reduce the first allied station's level");
            if (index == 2) {
                first.previous_base_levels.clear();
                expect(level(1) == Real{},
                    "SAE-02: damage with a missing predecessor returns zero");
                first.previous_base_levels = {1};
                expect(level(3) == Real{},
                    "SAE-02: a predecessor chain that skips the surviving level returns zero");
                first.previous_base_levels = {2, 1};
            }
            expect(session.value().ledgers().front().tech_level == 1,
                "SAE-02: perceived station damage leaves the player's tech ledger independent");
        }
        expect(static_cast<bool>(session.value().stage_remove(10)), "SAE-02: remove shared station");
        expect(level(1) == ai::real(5),
            "SAE-02: station removal advances to the next qualifying ally");
        expect(static_cast<bool>(session.value().stage_remove(25)), "SAE-02: remove remaining allied station");
        for (const auto player : {1U, 3U})
            expect(level(player) == Real{},
                "SAE-02: station loss clears both teammates' level without borrowing the enemy's");
        expect(level(4) == ai::real(5), "SAE-02: other team's station survives independently");
    }
}


void campaign_defender_fallback() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.perception.campaign_game = true;
        world.host->setup.perception.campaign_defender = 2;
        world.host->setup.perception.base_level = 1;
        auth::ModuleManifest manifest;
        expect(manifest.add("DEFENDER.LUA", "function Check(player) Allowed = EvaluatePerception(\"Allowed_As_Defender_Land\", player) end\n").has_value(),
            "campaign defender fallback fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, world.host, errors);
        expect(errors.empty() && scripts.create_instance(1, "DEFENDER.LUA").has_value(), "perception fallback binds without a goal engine");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const tactical::PlayerId player : {1U, 2U}) {
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, player}}};
            expect(scripts.submit_event(std::move(event)).has_value() && scripts.service(executor).has_value(), "fallback player call services");
            const auto allowed = scripts.read_global(1, "Allowed");
            expect(allowed && allowed.value() && std::get<Real>(allowed.value()->data) == ai::real(player == 1 ? 1 : 0),
                "campaign fallback qualifies each player's defender role without an engine");
        }
    }
}


void campaign_retreat_inputs() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& setup = world.host->setup;
        setup.perception.campaign_game = true;
        setup.perception.campaign_defender = 2;
        eawr::sim::tactical::TacticalSetup initial;
        initial.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto child = eawr::sim::tactical::TacticalSession::create(initial).value();
        world.host->snapshot = child.snapshot();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return {}; };
        auto equations = ai::EquationSet::parse({{"campaign.xml", "<Root><Retreat>Variable_Self.CanRetreat</Retreat>"
            "<Defender>Variable_Self.IsDefender</Defender></Root>"}}, converters).value();
        ai::AiData data; data.equations = std::move(equations);
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        ai::EngineContractAccess::collect_fixture(engine);
        expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "WGT-33: absent retreat qualification remains inert");
        setup.campaign_can_retreat = [](const auto& snapshot, auto player) { return player == 2 && snapshot.completed_tick() == 0; };
        expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "WGT-33: eligibility alone cannot enable an unroutable retreat");
        setup.campaign_retreat_order = [](eawr::sim::tactical::PlayerId player) {
            return eawr::core::Result<auth::TacticalOrder>::success({player, {}, eawr::sim::tactical::QuitPayload{}});
        };
        expect(engine.evaluate("Retreat", 2, nullptr) == ai::real(1) && engine.evaluate("Retreat", 1, nullptr) == Real{},
            "WGT-33: retreat reads the owning campaign's per-player snapshot qualification");
        expect(engine.evaluate("Defender", 2, nullptr) == ai::real(1) && engine.evaluate("Defender", 1, nullptr) == Real{},
            "WGT-33: defender is per player");
        auth::ModuleManifest manifest;
        expect(manifest.add("WITHDRAW.LUA", "function Check(tf) tf.Withdraw_Units() end\n").has_value(), "campaign withdrawal fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts, world.host, errors);
        expect(errors.empty() && scripts.create_instance(1, "WITHDRAW.LUA").has_value(), "stock withdrawal binding registers");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const bool campaign : {true, false}) {
            setup.perception.campaign_game = campaign;
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}};
            expect(scripts.submit_event(std::move(event)).has_value(), "withdrawal call queues");
            const auto report = scripts.service(executor).value();
            expect(report.diagnostics.empty(), "withdrawal binding has no diagnostic");
            expect(report.commands.size() == (campaign ? 1U : 0U), "only qualified campaign withdrawal stages a command");
            if (campaign) expect(report.commands[0].verb == foc::verb_campaign_retreat
                && report.commands[0].arguments.size() == 1, "withdrawal uses the ordinary deferred replay command route");
            else expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "standalone still perceives retreat disabled");
        }
    }
}


void economy_inputs() {
    World world;
    foc::AiType other; other.type_id = 2; other.name = "OTHER";
    world.host->types.emplace(2, &other); world.host->types_by_name.emplace(other.name, &other);
    world.host->setup.perception.campaign_game = false;
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto equations = ai::EquationSet::parse({{"economy.xml", "<Root>"
        "<Campaign>Game.IsCampaignGame</Campaign>"
        "<Credits>Variable_Self.CreditsUnnormalized</Credits>"
        "<Room>Variable_Self.UnitSpaceAvailable</Room>"
        "<Pool>Variable_Self.ReinforcementsUnnormalized</Pool>"
        "<Level>Variable_Self.BaseLevel</Level>"
        "<Structures>Variable_Self.TacticalBuiltStructureCount{Parameter_Type = \"SHIP\"}</Structures>"
        "<FilteredPool>Variable_Self.ReinforcementsUnnormalized{Parameter_Category = 1}</FilteredPool>"
        "<OpenPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"SHIP\"}</OpenPads>"
        "<OtherPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"OTHER\"}</OtherPads>"
        "<Contestable>Variable_Target.IsContestable</Contestable>"
        "<BuildPad>Variable_Target.IsBuildPad</BuildPad>"
        "</Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-02: economy equations parse");
    if (!equations) return;
    ai::AiData data;
    data.equations = std::move(equations).value();
    ai::Engine engine(world.host, std::move(data), {});
    expect(engine.evaluate("Campaign", 1, nullptr) == Real{}, "SAE-01: skirmish campaign input is zero");
    expect(engine.evaluate("Credits", 1, nullptr) == Real{}, "SAE-02: absent economy has no spendable credits");
    const auto snapshot = [&](std::uint32_t population, std::uint32_t cap, std::int64_t credits) {
        eawr::sim::tactical::EconomyView account;
        account.player = 1;
        account.credits = eawr::sim::math::Fixed::from_integer(credits).value();
        account.population = population;
        account.population_cap = cap;
        account.pool = {1, 1};
        world.host->snapshot = std::make_shared<eawr::sim::tactical::TacticalSnapshot>(0,
            std::vector<eawr::sim::tactical::SnapshotPlayer>{}, std::vector<eawr::sim::tactical::TacticalInstance>{},
            std::vector<eawr::sim::tactical::Event>{}, std::vector<eawr::sim::tactical::CombatEvent>{},
            std::vector<eawr::sim::tactical::Projectile>{}, std::nullopt,
            std::vector<eawr::sim::tactical::SpinningCraft>{}, std::vector<eawr::sim::tactical::SquadronTarget>{},
            std::vector<eawr::sim::tactical::Squadron>{}, std::vector<eawr::sim::tactical::EconomyView>{account});
    };
    snapshot(7, 20, 1499);
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1499), "SAE-02: credits read the completed account");
    expect(engine.evaluate("Room", 1, nullptr) == ai::real(13), "SAE-02: population room is cap minus population");
    expect(engine.evaluate("Pool", 1, nullptr) == ai::real(2000), "SAE-02: reinforcement power includes both pooled units");
    expect(engine.evaluate("FilteredPool", 1, nullptr) == Real{}, "SAE-02: reinforcement power honors the requested category");
    snapshot(23, 20, 1500);
    expect(engine.evaluate("Room", 1, nullptr) == Real{}, "SAE-02: an over-cap account never underflows room");
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1500), "SAE-02: the saving threshold uses current credits");
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                     {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
    setup.units = {{10, 1, 1}, {11, 1, 1}, {12, 1, 2}};
    eawr::sim::tactical::EconomyRules rules;
    rules.players = {{1, eawr::sim::math::Fixed::from_integer(1000).value(), 0},
        {2, eawr::sim::math::Fixed::from_integer(1000).value(), 0}};
    rules.pads.neutral = 2;
    rules.pads.capture = {{1, eawr::sim::math::Fixed::from_integer(100).value(),
        eawr::sim::math::Fixed::from_integer(1).value(), {1, 2}, false, true, true}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(session), "SAE-02: the synthetic ownership world starts");
    if (session) {
        world.host->world = &session.value();
        world.host->setup.content.types[0].star_base = true;
        world.host->setup.content.types[0].base_level = 3;
        world.host->setup.content.types[0].tech_level = 7;
        expect(engine.evaluate("Level", 1, nullptr) == ai::real(3), "SAE-02: station level uses Base_Level, independently of Tech_Level");
        expect(engine.evaluate("Structures", 1, nullptr) == Real{}, "WAS-10: unbuilt pads are not completed structures");
        expect(session.value().production_counts(1, 1).owned_player == 2, "SAE-03: ownership cache starts from live units");
        expect(session.value().production_counts(99, 1).current_allies == 0,
            "SAE-02: an undeclared AI service slot has no production ownership");
        expect(!session.value().build_allowed(99, 10, 1), "SAE-03: an undeclared buyer cannot use a live producer");
        world.host->setup.content.types[0].capture_point = true;
        world.view->units.clear();
        for (const auto& placed : setup.units) {
            ai::ViewUnit unit;
            unit.id = placed.entity_id;
            unit.type = placed.type_id;
            unit.owner = placed.owner;
            world.view->units.push_back(unit);
        }
        expect(engine.evaluate("OpenPads", 1, nullptr) == ai::real(2), "SAE-02: open pad count excludes the other player's pads");
        expect(engine.evaluate("OtherPads", 1, nullptr) == Real{}, "SAE-02: open pad count honors the requested type");
        ai::Target target;
        target.object = 10;
        expect(engine.evaluate("Contestable", 1, &target) == ai::real(1), "SAE-02: capture targets expose real contestability");
        expect(engine.evaluate("BuildPad", 1, &target) == ai::real(1), "SAE-02: capture targets expose their build capability");
    }
    ai::GoalType goal;
    goal.time_limit = ai::real(30);
    goal.build_time_delay_tolerance = ai::real(2);
    expect(ai::production_time_limit(goal, std::nullopt, ai::real(5)) == ai::real(35), "SAE-05: no potential uses the authored time limit");
    expect(ai::production_time_limit(goal, ai::real(10), ai::real(5)) == ai::real(30), "SAE-05: potential limit multiplies duration plus adjustment");
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(20)), "SAE-05: equality is admitted");
    expect(!ai::production_time_allowed(goal, ai::real(10), ai::real(21)), "SAE-05: exceeding a positive limit is rejected");
    goal.build_time_delay_tolerance = Real{};
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(200)), "SAE-05: a zero limit does not reject");
}


void tactical_activation_estimate() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    for (const int scenario : {0, 1, 2}) {
        t::TacticalSetup start;
        start.players = {{1, 1, 100, t::player_flag_commandable}};
        start.units = {{1, 10, 1}};
        t::EconomyRules rules;
        rules.players = {{1, q(scenario == 2 ? 0 : 1000), 20}};
        rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 900, 900, 1, true}}}};
        auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "SAE-09: activation fixture starts");
        if (!made) continue;
        if (scenario == 1) {
            expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(),
                "SAE-09: long-running production enters the queue");
            eawr::sim::InlineExecutor executor;
            expect(made.value().step(executor).has_value(), "SAE-09: queued purchase advances");
            expect(!made.value().ledgers().front().queues[0].empty(), "SAE-09: existing delay is present");
        }
        auto host = ai::make_tactical_host();
        host->setup.perception.campaign_game = false;
        foc::AiType factory;
        factory.type_id = 10;
        factory.star_base = true;
        host->types.emplace(10, &factory);
        host->world = &made.value();
        host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
        ai::PlanDef definition;
        definition.allow_free_store = false;
        definition.ignore_target = true;
        ai::TaskForceDef force;
        ai::TeamDef team;
        team.min_count = team.max_count = 1;
        team.types = {50};
        force.teams = {team};
        definition.taskforces = {force};
        ai::Engine engine(host, {}, {definition});
        expect(engine.producer(1, 50) == 1 && host->economy(1) != nullptr,
            "SAE-09: tactical selection has a producer and a current credit account");
        ai::GoalType type;
        type.time_limit = ai::real(1);
        type.build_time_delay_tolerance = ai::real(1) / ai::real(100);
        ai::PlayerAi player;
        player.player = 1;
        ai::GoalFunctionEntry function;
        function.goal = &type;
        player.functions.push_back(function);
        ai::Goal goal;
        goal.id = 1;
        goal.potential.emplace();
        goal.potential->valid = true;
        const bool allowed = ai::EngineContractAccess::test_valid(engine, player, goal);
        expect(allowed == (scenario != 2),
            "SAE-09/03: production duration and queue delay do not block selection; insufficient credits do");
        if (allowed) expect(goal.potential->units == std::vector<t::TypeId>{50}
            && goal.potential->sources == std::vector<std::uint8_t>{2},
            "SAE-09: selected new production survives a positive delay limit below its duration");
    }
}


void production_lifecycle() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // Stations use the owner's faction even when an allied buyer has another menu.
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}};
    start.units = {{1, 10, 2}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}, {2, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(900), 30, 30, 1, true}}},
        {10, 200, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true},
                  {51, t::BuildKind::unit, t::BuildQueue::units, q(200), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: allied producer fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    auto view = std::make_shared<ai::WorldView>();
    ai::ViewUnit station;
    station.id = 1; station.type = 10; station.owner = 2;
    view->units.push_back(station);
    for (eawr::sim::EntityId id = 2; id <= 1001; ++id) {
        ai::ViewUnit unit; unit.id = id; unit.type = 99;
        view->units.push_back(unit);
    }
    foc::AiType station_type;
    station_type.type_id = 10; station_type.star_base = true;
    host->types.emplace(10, &station_type);
    host->view = view; host->world = &made.value();
    ai::Engine engine(host, {}, {});
    expect(made.value().build_allowed(1, 1, 51) && engine.producer(1, 51) == 1,
        "SAE-03: AI discovers a type exclusive to its allied station owner's faction");
    const auto* option = engine.build_option(1, 50, 1);
    expect(option != nullptr && option->price == q(100), "SAE-03: AI uses owner-faction station price");
    for (std::uint32_t i = 0; i < 100; ++i) {
        expect(engine.producer(1, 500 + i) == 0, "SAE-03: missing type has no producer");
        expect(engine.producer(1, 50) == 1, "SAE-03: indexed producer remains deterministic");
    }
    expect(engine.producer_work().entities == 1001 && engine.producer_work().candidates == 101,
        "SAE-03: repeated candidates and misses scan world entities only once per service");
    eawr::sim::InlineExecutor executor;
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(), "SAE-03: allied purchase submits");
    expect(made.value().step(executor).has_value() && made.value().ledgers().front().credits == q(900),
        "SAE-03: ordinary purchase debits the same owner-faction price");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: lifecycle scheduler starts");
    if (!scripts) return;
    std::uint64_t sequence = 0;
    const auto snapshot = made.value().snapshot();
    expect(engine.before_service(*host->ai_world, scripts.value(), sequence).has_value(),
        "SAE-03: next AI service resets its derived producer index");
    expect(engine.producer(1, 50) == 1 && engine.producer_work().entities == 1001 && engine.producer_work().candidates == 1,
        "SAE-03: producer work counters reset and the next service rebuilds once");

    // Rejection, constructor removal, pad destruction and failed final replacement all
    // finish the block and release its reservation; the ordinary successful build still waits.
    // The allied pad owner remains a combatant; neutral capture ownership is a distinct player.
    for (const int scenario : {0, 1, 2, 3, 4}) {
        start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}, {3, 3, 300, 0}};
        start.units = {{1, 20, 2}};
        rules.players = {{1, q(1000), 20, true}, {2, q(1000), 20}, {3, q(1000), 20}};
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{31, t::BuildKind::structure, t::BuildQueue::units, q(900), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 3;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const t::TypeId type : {20U, 30U, 31U, 40U, 41U}) {
            if (scenario == 3 && type == 40) continue;
            t::DurabilityProfile profile; profile.type_id = type; profile.max_hull = q(300);
            health.profiles.push_back(profile);
        }
        auto pad_world = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(pad_world), "SAE-03: pad lifecycle fixture starts");
        if (!pad_world) continue;
        host = ai::make_tactical_host();
        host->world = &pad_world.value(); host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::Engine pad_engine(host, {}, {ai::PlanDef{}});
        expect(pad_engine.build_option(1, 30, 1) != nullptr && pad_engine.build_option(1, 31, 1) == nullptr,
            "SAE-03: allied pad preserves buyer-faction menu exception");
        expect(pad_engine.producer(1, 30) == 1, "SAE-03: indexed allied pad is initially available");
        auto& player = ai::EngineContractAccess::add_player(pad_engine, 1);
        ai::EngineContractAccess::seed(pad_engine, player, scripts.value());
        expect(pad_engine.producer(1, 30) == 0, "SAE-03: a reservation made after indexing blocks the next ordered lookup");
        ai::EngineContractAccess::service(pad_engine, player, 1);
        expect(ai::EngineContractAccess::orders(pad_engine) == 1 && !pad_engine.block(1)->finished,
            "SAE-03: pad production emits an ordinary command and waits");
        if (scenario == 0) {
            expect(pad_world.value().submit({{0, 2, 0}, {1}, t::PadBuildPayload{31}}).has_value(), "SAE-03: competing allied build submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: competing construction occupies pad");
            expect(pad_world.value().submit({{1, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: stale AI request submits for Core rejection");
        } else {
            expect(pad_world.value().submit({{0, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: pad build submits");
        }
        expect(pad_world.value().step(executor).has_value(), "SAE-03: pad command delivery completes");
        if (scenario == 1 || scenario == 2) {
            const auto victim = scenario == 1 ? pad_world.value().pads().at(1).under_construction : 1;
            expect(victim != 0 && pad_world.value().submit({{1, 1, 1}, {victim}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
                "SAE-03: constructor or pad destruction submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: destruction commits");
        } else if (scenario >= 3) {
            host->snapshot = pad_world.value().snapshot();
            host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
            ai::EngineContractAccess::service(pad_engine, player, 4);
            expect(!pad_engine.block(1)->finished, "SAE-03: accepted construction remains pending while its child lives");
            for (int frame = 0; frame < 31; ++frame) expect(pad_world.value().step(executor).has_value(), "SAE-03: construction advances");
        }
        host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::EngineContractAccess::service(pad_engine, player, 100);
        expect(player.tasks.empty() && pad_engine.block(1)->finished && pad_engine.block(1)->result == (scenario == 4),
            "SAE-03: pad outcome completes the production block with the correct result");
        expect(pad_engine.plan(1)->reserved_pads.empty() && player.reserved_credits.at(1) == Real{},
            "SAE-03: completed or failed pad task releases pad and credit reservations");
        if (scenario == 4) expect(ai::EngineContractAccess::completed_pad_contrast(pad_engine, player),
            "GS-22: real completed pad build remains valid after execution releases its reservation");
    }
}


void spending_pad_states() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 2, 200, 1}, {4, 1, 300, 1}};
        start.units = {{1, 20, 1}, {2, 20, 2}, {3, 20, 3}, {4, 20, 4}, {5, 10, 1}, {6, 20, 2}};
        for (std::size_t i = 0; i < start.units.size(); ++i) start.units[i].position.x = q(static_cast<std::int64_t>(i) * 1000);
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}, {3, q(1000), 20, true}, {4, q(1000), 20}};
        rules.pads.neutral = 4;
        rules.pads.capture = {{20, q(100), q(1), {100, 200, 300}, true, true, true, {}, false, 6}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}, {32, 42, q(100), 1, 1}};
        rules.pads.influence = {{10, false}, {20, false}, {30, false}, {31, false}, {32, false}, {40, false}, {41, false}, {42, false}};
        for (const auto& [faction, type] : {std::pair{100U, 30U}, std::pair{200U, 31U}, std::pair{300U, 32U}})
            rules.menus.push_back({20, faction, {{type, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false});
        t::DurabilityTable health;
        for (const auto type : {20U, 30U, 31U, 32U, 40U, 41U, 42U}) {
            t::DurabilityProfile row; row.type_id = type; row.max_hull = q(300); health.profiles.push_back(row);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WAS-10/11: typed allied pad fixture starts");
        if (!made) continue;
        auto& world = made.value();
        auto host = ai::make_tactical_host(); host->world = &world;
        host->setup.journal = std::make_shared<foc::AiJournal>();
        for (const auto& [type, name] : {std::pair{20U, "PAD"}, {30U, "UC_E"}, {31U, "UC_R"}, {32U, "UC_U"},
                {40U, "MINE_E"}, {41U, "MINE_R"}, {42U, "MINE_U"}}) {
            foc::AiType row; row.type_id = type; row.name = name; host->setup.content.types.push_back(row);
        }
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        host->setup.players = {{1, "EMPIRE", false, false, "", true}, {2, "EMPIRE", false, true, ""},
            {3, "REBEL", false, true, ""}, {4, "NEUTRAL", true, false, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"spending.xml", "<Root>"
            "<Resources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"MINE_E\", Parameter_Type=\"MINE_R\", Parameter_Type=\"MINE_U\"}</Resources>"
            "<ResourceDesire>Function_Resources.Evaluate > 0</ResourceDesire>"
            "<Constructors>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC_E\"}</Constructors>"
            "<AllStructures>Variable_Self.TacticalBuiltStructureCount</AllStructures>"
            "<ForeignStructures>Variable_Enemy.TacticalBuiltStructureCount</ForeignStructures>"
            "<Open>Variable_Self.OpenBuildPadCount</Open>"
            "<Menu>Variable_Self.OpenBuildPadCount{Parameter_Type=\"UC_E\"}</Menu>"
            "<Multiple>Variable_Self.OpenBuildPadCount{Parameter_Type=\"PAD\", Parameter_Type=\"UC_E\", Parameter_Type=\"PAD\"}</Multiple>"
            "<WrongMenu>Variable_Self.OpenBuildPadCount{Parameter_Type=\"UC_R\"}</WrongMenu>"
            "<Unknown>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UNKNOWN\"}</Unknown>"
            "<MixedResources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UNKNOWN\", Parameter_Type=\"MINE_E\", Parameter_Type=\"MINE_R\", Parameter_Type=\"MINE_U\"}</MixedResources>"
            "<Savings>(Function_Menu.Evaluate > 0) * (Variable_Self.CreditsUnnormalized < 1500)</Savings>"
            "</Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-10/12: resource and savings equations parse");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {});
        ai::EngineContractAccess::perception_executor(engine, &executor);
        const auto value = [&](const char* equation) {
            const auto journals = host->setup.journal->plans.size();
            const auto direct = engine.evaluate(equation, 2, nullptr);
            host->setup.journal->plans.resize(journals);
            ai::EngineContractAccess::prepare_pads(engine, executor);
            const auto prepared = engine.evaluate(equation, 2, nullptr);
            ai::EngineContractAccess::reset_pads(engine);
            expect(prepared == direct, "WAS-10/11: prepared pads preserve every live-state and typed-filter result");
            return prepared;
        };
        const auto foreign = [&] {
            ai::AiRandom random;
            return host->ai_world->perception(engine, *engine.data().equations.find("FOREIGNSTRUCTURES"),
                ai::Context{2, 1, {}, nullptr}, random);
        };
        auto& pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads());
        pads.at(2).cooldown_until = 6;
        pads.at(6).cooldown_until = 1000;
        expect(value("Open") == ai::real(1) && value("Menu") == ai::real(1)
            && value("Multiple") == ai::real(1) && value("WrongMenu") == Real{},
            "WAS-11: eligible human ally matches faction menu once, hostile/neutral/cooldown pads do not");
        expect(value("Savings") == ai::real(1), "WAS-12: allied menu-capable open pad enables refinery savings");
        expect(value("AllStructures") == Real{}, "WAS-10: an unfiltered empty pad census is zero");
        expect(!foreign(), "WAS-10: foreign built-count context rejects an allied player too");
        host->setup.journal->plans.clear();
        expect(value("Unknown") == Real{}, "WAS-10: unknown resource types contribute zero without failing evaluation");
        expect(host->setup.journal->plans.size() == 1
            && host->setup.journal->plans.back().player == 2
            && host->setup.journal->plans.back().event == "error"
            && host->setup.journal->plans.back().detail == "TacticalBuiltStructureCount: unknown type UNKNOWN",
            "WAS-10: unknown type is reported through the existing AI journal");
        expect(value("MixedResources") == Real{}, "WAS-10: missing resource filters retain the known empty counts");
        ai::PlayerAi player; player.player = 2;
        for (const auto type : {30U, 31U, 32U}) {
            ai::Goal goal; goal.id = type; goal.potential = ai::PotentialPlan{};
            goal.potential->valid = true; goal.potential->sources = {2}; goal.potential->units = {type};
            goal.potential->producers = {1}; goal.potential->cost = ai::real(100);
            ai::EngineContractAccess::reserve(engine, player, goal);
            expect(value("AllStructures") == ai::real(1), "WAS-10: unfiltered pending reservation counts once");
            expect(value("Resources") == ai::real(1) && value("ResourceDesire") == ai::real(1),
                "WAS-10: every faction's pending constructor alias enables resource-gated desire before completion");
            expect(value("MixedResources") == ai::real(1), "WAS-10: an unknown filter does not discard a known pending resource");
            expect(value("Open") == Real{}, "WAS-11: pending reservation excludes the last eligible pad");
            ai::EngineContractAccess::release(engine, player, goal);
            expect(value("Resources") == Real{} && value("Menu") == ai::real(1),
                "WAS-10/11: abandonment releases typed and open-pad reservation inputs");
        }
        for (int frame = 0; frame < 5; ++frame) expect(world.step(executor).has_value(), "WAS-11: cooldown advances");
        refresh();
        expect(value("Open") == ai::real(1), "WAS-11: cooldown still excludes the pad before its deadline");
        expect(world.step(executor).has_value(), "WAS-11: cooldown reaches deadline");
        refresh();
        expect(value("Open") == ai::real(2), "WAS-11: cooldown deadline is inclusive");
        expect(world.submit({{6, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value()
            && world.step(executor).has_value(), "WAS-10: human allied construction starts");
        refresh();
        expect(value("Menu") == ai::real(1), "WAS-11: actual construction excludes its pad");
        // WAS-10: the fallback tests the actual child's type when no reservation is stored.
        const auto child = world.pads().at(1).under_construction;
        expect(child != 0, "WAS-10: under-construction child is live");
        expect(value("AllStructures") == ai::real(1), "WAS-10: unfiltered census includes live construction");
        expect(value("Constructors") == ai::real(1) && value("Resources") == Real{},
            "WAS-10: an unreserved allied child matches its actual type, without a completed-type alias");
        for (int frame = 0; frame < 31; ++frame) expect(world.step(executor).has_value(), "WAS-10: mine completes");
        refresh();
        expect(value("Resources") == ai::real(1) && value("Menu") == ai::real(1),
            "WAS-10/11: completed human mine remains visible to allied AI and excludes open-pad count");
        expect(value("MixedResources") == ai::real(1), "WAS-10: an unknown filter does not discard a known completed resource");
        expect(value("AllStructures") == ai::real(1) && !foreign(),
            "WAS-10: unfiltered census includes completion and foreign context still rejects it");
        expect(world.submit({{world.completed_tick(), 2, 0}, {2}, t::PadBuildPayload{30}}).has_value()
            && world.step(executor).has_value(), "WAS-10: a second allied pad starts construction");
        refresh();
        ai::Goal pending; pending.id = 100; pending.potential = ai::PotentialPlan{};
        pending.potential->valid = true; pending.potential->sources = {2}; pending.potential->units = {32};
        pending.potential->producers = {6}; pending.potential->cost = ai::real(100);
        ai::EngineContractAccess::reserve(engine, player, pending);
        expect(world.pads().at(1).constructed != 0 && world.pads().at(2).under_construction != 0
            && value("Resources") == ai::real(2) && value("Constructors") == ai::real(1)
            && value("AllStructures") == ai::real(3)
            && value("AllStructures") == *value("Resources") + *value("Constructors"),
            "WAS-10: unfiltered completed, constructing and reserved pads equal the nonoverlapping typed sum");
        ai::EngineContractAccess::release(engine, player, pending);
        expect(value("AllStructures") == ai::real(2), "WAS-10: releasing one reservation retains both live children");
        expect(world.stage_remove(world.pads().at(2).under_construction).has_value(),
            "WAS-10: second construction destruction stages");
        refresh();
        expect(value("AllStructures") == ai::real(1), "WAS-10: destroying construction retains the completed child");
        expect(world.stage_remove(world.pads().at(1).constructed).has_value(), "WAS-10: completed mine destruction stages");
        refresh();
        expect(value("Resources") == Real{}, "WAS-10: destroyed mine cannot retain the resource gate");
        expect(value("AllStructures") == Real{}, "WAS-10: unfiltered census drops destroyed children");
        const auto hash = world.step(executor);
        expect(static_cast<bool>(hash), "WAS-10/11: post-destruction tick commits");
        if (hash) {
            const auto digest = hash.value().state_sha256;
            if (workers == 1) baseline.push_back(digest);
            else expect(digest == baseline.front(), "WAS-10/11: pad state hashes equal on 1/2/4/8 workers");
        }
    }
}


void spending_pad_refunds() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    std::vector<std::string> callback_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
      {
        auto host = ai::make_tactical_host();
        ai::Engine engine(host, ai::AiData{}, {ai::PlanDef{}});
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, auth::ModuleManifest{});
        expect(scripts.has_value(), "WAS-26: station refusal interleaving scheduler starts");
        if (scripts) {
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            auto& player = ai::EngineContractAccess::rejected_station_fixture(engine);
            ai::EngineContractAccess::direct_pad_service(engine, player);
            expect(player.tasks.size() == 1 && !engine.block(1)->finished,
                "WAS-26/WBP-47: pad-only service retains observed station refusal for ordinary execution");
            ai::EngineContractAccess::service(engine, player, 2);
            expect(player.tasks.empty() && engine.block(1)->finished && !engine.block(1)->result,
                "WAS-26: ordinary execution settles the retained station refusal block");
        }
      }
      for (const auto multiplier : {1, 2}) {
       for (const auto scenario : {0, 1, 2, 3, 4}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start; start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 3, 300, 0}};
        start.units = {{1, 20, 1}};
        t::EconomyRules rules; rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}};
        rules.players[1].credit_multiplier = q(multiplier);
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(300), 30, 30, 0, true}}, 0, false}};
        rules.pads.capture = {{20, q(100), q(1), {100}, false, true, true}};
        rules.pads.neutral = 3;
        rules.pads.construction = {{30, 40, q(300), 100, 100}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const auto type : {20U, 30U, 40U}) {
            if ((scenario == 1 || scenario == 3) && type == 30) continue; // Core refuses creation after AI precheck.
            t::DurabilityProfile row; row.type_id = type; row.max_hull = q(300); health.profiles.push_back(row);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WBP-47/48: pad refund fixture starts"); if (!made) continue;
        auto& world = made.value(); auto host = ai::make_tactical_host(); host->world = &world;
        foc::AiType constructor; constructor.type_id = 30; constructor.name = "UC"; constructor.tactical_cost = q(100);
        host->setup.content.types = {constructor}; host->types.emplace(30, &host->setup.content.types.front());
        host->types_by_name.emplace("UC", &host->setup.content.types.front());
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        expect(world.step(executor).has_value(), "WBP-47: initial barrier completes"); refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"pad.xml", "<Root><Resource>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC\"}</Resource></Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-10: refund fixture resource equation parses"); if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {ai::PlanDef{}}); auto& player = ai::EngineContractAccess::add_player(engine, 2);
        auth::SessionConfig callback_config; callback_config.tick_duration = {1, 30};
        auto callbacks = auth::ScriptScheduler::create(callback_config, auth::ModuleManifest{});
        expect(static_cast<bool>(callbacks), "WBP-47/48: execution callback scheduler starts"); if (!callbacks) continue;
        ai::Goal goal; goal.id = 1; goal.potential = ai::PotentialPlan{};
        goal.potential->valid = true; goal.potential->cost = ai::real(300);
        goal.potential->sources = {2}; goal.potential->units = {30}; goal.potential->producers = {1};
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, callbacks.value(), sequence);
        const auto deliver = [&] {
            for (const auto& command : ai::EngineContractAccess::take_orders(engine)) {
                const auto order = foc::tactical_ai_detail::translate_order(command);
                expect(static_cast<bool>(order), "WBP-47/48: pad accounting command translates");
                if (order) expect(world.submit({{world.completed_tick(), order.value().issuer, sequence++},
                    order.value().units, order.value().payload}).has_value(), "WBP-47/48: pad accounting command submits");
            }
            expect(world.step(executor).has_value(), "WBP-47/48: pad accounting barrier commits"); refresh();
        };
        ai::EngineContractAccess::reserve(engine, player, goal); deliver();
        expect(engine.evaluate("Resource", 2, nullptr) == ai::real(1), "WAS-10: pad reservation enables the resource gate");
        if (scenario == 3 || scenario == 4) {
            host->engine = &engine;
            auto& installed = ai::EngineContractAccess::direct_pad_fixture(engine, 30, 1);
            auth::ModuleManifest manifest;
            expect(manifest.add("PAD.LUA", "refused = 0\nfunction Default_No_Units_Remaining(tf) refused = refused + 1 end\n"
                "function Request(tf,pad) tf.Build(\"UC\",pad) rest = tf.Build_All() end\n")
                .has_value(), "WBP-47: direct pad Lua loads");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
            expect(static_cast<bool>(scripts), "WBP-47: direct pad scheduler starts"); if (!scripts) continue;
            std::vector<eawr::core::Diagnostic> errors;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            foc::detail::register_plan_bindings(scripts.value(), host, errors);
            expect(errors.empty() && scripts.value().create_instance(1, "PAD.LUA").has_value(), "WBP-47: direct pad binding registers");
            if (scenario == 4) {
                const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads()).at(1).cooldown_until = 100;
                ai::EngineContractAccess::pause_planning(engine);
                // The caller's event counter expires before after_service executes.
                const auto prepare = [&] {
                    std::uint64_t temporary_sequence = 7;
                    return engine.before_service(*host->ai_world, scripts.value(), temporary_sequence, &executor);
                };
                expect(prepare().has_value(), "WBP-47: direct refusal preparation completes before Lua service");
            }
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Request";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}, auth::Value{auth::Handle{foc::handle_game_object, 1}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "WBP-47: direct pad call submits");
            auto report = scripts.value().service(executor);
            expect(report && engine.after_service(report.value()).has_value(), "WBP-47: direct pad call applies in its current barrier");
            if (!report) continue;
            if (scenario == 4) {
                expect(report.value().commands.empty(), "WBP-47: cooldown refuses the direct purchase");
                expect(ai::EngineContractAccess::attachment_events(engine).size() == 1,
                    "WBP-47: direct refusal queues its callback after preparation returned");
                const auto next_service = [&] {
                    std::uint64_t temporary_sequence = 7;
                    expect(engine.before_service(*host->ai_world, scripts.value(), temporary_sequence, &executor).has_value(),
                        "WBP-47: deferred callback receives a future tick and unique sequence");
                    expect(scripts.value().service(executor).has_value(), "WBP-47: deferred callback services");
                };
                next_service();
                next_service();
                const auto refused = scripts.value().read_global(1, "refused");
                expect(refused && refused.value() && std::get<Real>(refused.value()->data) == ai::real(1),
                    "WBP-47: direct refusal callback is delivered exactly once across service boundaries");
                const auto hash = scripts.value().state_hash();
                expect(hash.has_value(), "WBP-47: deferred callback state hashes");
                if (hash) {
                    if (workers == 1) callback_baseline.push_back(hash.value());
                    else expect(hash.value() == callback_baseline[static_cast<std::size_t>(multiplier - 1)],
                        "WBP-47: deferred callback hashes equal on 1/2/4/8 workers");
                }
                continue;
            }
            expect(report.value().commands.size() == 1 && report.value().commands.front().verb == foc::verb_pad_build
                && installed.tasks.size() == 1 && installed.tasks.front().issued == world.completed_tick(),
                "WBP-47: direct Lua Build runs precheck now and Build_All cannot duplicate the allocation");
            const auto rest = scripts.value().read_global(1, "rest");
            expect(rest && rest.value() && engine.block(std::get<auth::Handle>(rest.value()->data).id)->finished,
                "WBP-47: Build_All with no remaining allocation returns a completed block");
            ai::EngineContractAccess::return_orders(engine, report.value());
            ai::EngineContractAccess::bind_scripts(engine, callbacks.value(), sequence);
        } else {
            ai::EngineContractAccess::purchase(engine, player, goal, 30, 1);
            if (scenario == 0)
                const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads()).at(1).cooldown_until = 100;
            ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        }
        expect(player.reserved_credits.at(1) == ai::real(scenario == 0 ? 300 : 200),
            "WBP-47/48: only a passed precheck consumes the generic tactical cost");
        if (scenario == 0) expect(engine.plan(1)->reserved_pads == std::vector<eawr::sim::EntityId>{1},
            "WBP-48: early refusal retains the pad allocation for a later retry or plan release");
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(engine.evaluate("Resource", 2, nullptr) == ai::real(scenario == 1 || scenario == 3 ? 0 : 1),
            "WAS-10: Core no-child refusal clears its typed gate; pending and live child states keep it");
        if (scenario == 2) {
            const auto child = world.pads().at(1).under_construction;
            expect(child != 0 && world.submit({{world.completed_tick(), 2, sequence++}, {child},
                t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "WBP-48: accepted child destruction submits");
            expect(world.step(executor).has_value(), "WBP-48: accepted child destruction commits"); refresh();
            expect(engine.evaluate("Resource", 2, nullptr) == Real{},
                "WAS-10: destroyed construction child clears the gate before the next execution service");
            ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        }
        const auto expected_immediate = 700 + (scenario == 1 || scenario == 3 ? 100 * multiplier : 0);
        expect(host->credits(2) == q(expected_immediate),
            "WBP-48: no-child Core refusal refunds once; early refusal and later child death do not");
        // A second observation of the same rejection must not repeat its direct refund.
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(host->credits(2) == q(expected_immediate), "WBP-48: repeated observation cannot duplicate a refund");
        ai::EngineContractAccess::release(engine, player, goal); deliver();
        expect(world.ledgers()[1].credits == q(700 + (scenario == 2 ? 200 : 300) * multiplier),
            "WBP-47/48: abandonment refunds only the unconsumed pad allocation through the positive-credit path");
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "WBP-47/48: final pad refund tick commits");
        if (result) {
            const auto index = static_cast<std::size_t>((multiplier - 1) * 4 + scenario);
            if (workers == 1) baseline.push_back(result.value().state_sha256);
            else expect(result.value().state_sha256 == baseline[index], "WBP-47/48: refund hashes equal on 1/2/4/8 workers");
        }
       }
      }
    }
}


void spending_wallet_reservations() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
      for (const auto multiplier : {1, 2}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start; start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}};
        start.units = {{1, 10, 1}};
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}};
        rules.players[1].credit_multiplier = q(multiplier);
        rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(300), 100, 100, 0, true}}}};
        rules.menus.front().options.front().requirements.current_allies = 1;
        rules.menus.front().options.push_back({51, t::BuildKind::unit, t::BuildQueue::units, q(300), 100, 100, 0, true});
        auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WAS-25/26: reservation accounting fixture starts");
        if (!made) continue;
        auto& world = made.value();
        auto host = ai::make_tactical_host(); host->world = &world;
        foc::AiType station; station.type_id = 10; station.name = "STATION"; station.star_base = true;
        foc::AiType unit; unit.type_id = 50; unit.name = "SHIP"; unit.tactical_cost = q(200);
        host->setup.content.types = {station, unit};
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        expect(world.step(executor).has_value(), "WAS-25: first barrier completes"); refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"wallet.xml", "<Root><Wallet>Variable_Self.CreditsUnnormalized</Wallet></Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-05: wallet equation parses");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value(); ai::Engine engine(host, std::move(data), {ai::PlanDef{}});
        auth::ModuleManifest manifest;
        expect(manifest.add("WALLET.LUA", "function Read(player) wallet = player.Get_Credits() end\n").has_value(), "WAS-05: wallet Lua loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        expect(static_cast<bool>(scripts), "WAS-05: wallet scheduler starts"); if (!scripts) continue;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "WALLET.LUA").has_value(), "WAS-05: player credit binding registers");
        std::uint64_t event_sequence = 0, command_sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, scripts.value(), event_sequence);
        const auto wallet = [&](const std::int64_t expected) {
            expect(engine.evaluate("Wallet", 2, nullptr) == ai::real(expected), "WAS-05/25: perception sees reservation-time wallet");
            auth::ScriptEvent event; event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, event_sequence++};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Read";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}};
            expect(scripts.value().submit_event(std::move(event)).has_value() && scripts.value().service(executor).has_value(), "WAS-05: Lua wallet read runs");
            const auto value = scripts.value().read_global(1, "wallet");
            expect(value && value.value() && std::get<Real>(value.value()->data) == ai::real(expected), "WAS-05: Get_Credits sees the same immediate wallet");
        };
        t::TacticalReplay replay; replay.setup = start;
        const auto deliver = [&] {
            for (const auto& command : ai::EngineContractAccess::take_orders(engine)) {
                const auto order = foc::tactical_ai_detail::translate_order(command);
                expect(static_cast<bool>(order), "WAS-25/26: reservation/refund/prepaid command translates");
                if (!order) continue;
                t::PlayerCommand routed{{world.completed_tick(), order.value().issuer, command_sequence++}, order.value().units, order.value().payload};
                expect(world.submit(routed).has_value(), "WAS-25/26: deterministic accounting command submits"); replay.commands.push_back(routed);
            }
            expect(world.step(executor).has_value(), "WAS-25/26: accounting command commits"); refresh();
        };
        auto& player = ai::EngineContractAccess::add_player(engine, 2);
        ai::Goal goal; goal.id = 1; goal.potential = ai::PotentialPlan{}; goal.potential->valid = true; goal.potential->cost = ai::real(300);
        ai::Goal competing = goal; competing.id = 2; competing.potential->cost = ai::real(100);
        wallet(1000);
        ai::EngineContractAccess::reserve(engine, player, goal); wallet(700);
        ai::EngineContractAccess::reserve(engine, player, goal); wallet(700);
        ai::EngineContractAccess::reserve(engine, player, competing); wallet(600);
        expect(goal.potential->cost == Real{} && player.reserved_credits.at(1) == ai::real(300), "WAS-25: pending cost clears and remainder persists");
        ai::EngineContractAccess::release(engine, player, competing, false); competing.potential->reserved = false;
        ai::EngineContractAccess::reserve(engine, player, competing); wallet(600);
        deliver(); wallet(600);
        ai::EngineContractAccess::release(engine, player, competing); wallet(600 + 100 * multiplier);
        ai::EngineContractAccess::release(engine, player, competing); wallet(600 + 100 * multiplier);
        deliver();
        ai::EngineContractAccess::purchase(engine, player, goal, 50, 1);
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(player.reserved_credits.at(1) == ai::real(100), "WAS-26: accepted effective-price purchase consumes generic cost only");
        wallet(600 + 100 * multiplier);
        expect(world.ledgers()[1].queues[0].size() == 1 && world.ledgers()[1].queues[0].front().paid == q(300),
            "WAS-26: prepaid queue entry keeps its ordinary cancellation price");
        const t::PlayerCommand cancel{{world.completed_tick(), 2, command_sequence++}, {}, t::CancelPayload{0, 0}};
        replay.commands.push_back(cancel);
        expect(world.submit(cancel).has_value()
            && world.step(executor).has_value(), "WPR-31: started queue cancellation commits"); refresh();
        wallet(600 + 400 * multiplier);
        ai::EngineContractAccess::release(engine, player, goal); wallet(600 + 500 * multiplier); deliver();
        wallet(600 + 500 * multiplier);
        const auto prior = 600 + 500 * multiplier;
        ai::Goal partial; partial.id = 3; partial.potential = ai::PotentialPlan{};
        partial.potential->valid = true; partial.potential->cost = ai::real(600);
        ai::EngineContractAccess::reserve(engine, player, partial); deliver();
        ai::EngineContractAccess::purchase(engine, player, partial, 50, 1);
        ai::EngineContractAccess::purchase(engine, player, partial, 50, 1);
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(world.ledgers()[1].queues[0].size() == 1 && player.reserved_credits.at(3) == ai::real(400),
            "WAS-26: partial start consumes one generic allocation; refused shared-limit work stays refundable");
        ai::EngineContractAccess::abandon_goal(engine, player, 3, scripts.value());
        wallet(prior - 600 + 400 * multiplier); deliver();
        expect(!player.reserved_credits.contains(3), "WAS-26: plan abandonment returns the unused remainder once");
        const auto human_tick = world.completed_tick();
        for (const auto& payload : {t::CommandPayload{t::AiReservationDebitPayload{q(100)}},
                t::CommandPayload{t::BuyPayload{51, true}}}) {
            const t::PlayerCommand control{{human_tick, 1, command_sequence++},
                std::holds_alternative<t::BuyPayload>(payload) ? std::vector<eawr::sim::EntityId>{1} : std::vector<eawr::sim::EntityId>{}, payload};
            expect(world.submit(control).has_value(), "WAS-25/26: human account control submits"); replay.commands.push_back(control);
        }
        const t::PlayerCommand too_large{{human_tick, 2, command_sequence++}, {}, t::AiReservationDebitPayload{q(5000)}};
        expect(world.submit(too_large).has_value(), "WAS-25: unaffordable reservation debit submits"); replay.commands.push_back(too_large);
        expect(world.step(executor).has_value() && world.ledgers()[0].credits == q(1000)
            && world.ledgers()[0].queues[0].empty() && world.ledgers()[1].credits == q(prior - 600 + 400 * multiplier),
            "WAS-25/26: human accounts cannot debit AI reservations or use prepaid buys"); refresh();
        replay.final_tick_count = world.completed_tick();
        const auto bytes = t::write_replay(replay);
        expect(static_cast<bool>(bytes), "WAS-25/26: new opcodes encode");
        if (bytes) {
            const auto decoded = t::parse_replay(bytes.value());
            expect(decoded && decoded.value() == replay, "WAS-25/26: reservation/prepaid opcodes round-trip exactly");
        }
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "WAS-25/26: final accounting tick commits");
        if (result) {
            if (workers == 1) baseline.push_back(result.value().state_sha256);
            else expect(result.value().state_sha256 == baseline[static_cast<std::size_t>(multiplier - 1)], "WAS-25/26: wallet/queue hashes equal on 1/2/4/8 workers");
        }
      }
    }
}


void allied_resource_upgrade_selection() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 1, 100, 1}, {4, 2, 200, 1}};
    start.units = {{1, 10, 1}, {2, 20, 1}, {3, 20, 4}, {6, 21, 1}, {7, 21, 4}};
    t::EconomyRules rules;
    for (const auto player : {1U, 2U, 3U, 4U}) rules.players.push_back({player, q(1000), 20, false, {}, 1, 5});
    t::BuildOption option{50, t::BuildKind::upgrade, t::BuildQueue::upgrades, q(100), 10, 10, 0, true};
    option.requirements.current_allies = 1;
    option.requirements.prerequisites = {20};
    rules.menus = {{10, 100, {option, {20, t::BuildKind::unit, t::BuildQueue::units, q(100), 50, 50, 0, true}}}};
    rules.upgrades = {{50, false, true, 0, {}}};
    rules.pads.capture = {{21, q(100), q(1), {100, 200}, false, true, true}};
    rules.pads.neutral = 4;
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-12: human owns the shared station and resource structure");
    if (!made) return;
    auto& placed_pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(made.value().pads());
    placed_pads.at(6).constructed = 2; placed_pads.at(7).constructed = 3;
    auto host = ai::make_tactical_host();
    host->setup.perception.campaign_game = false;
    foc::AiType station; station.type_id = 10; station.name = "STATION"; station.star_base = true; station.base_level = 1;
    foc::AiType mine; mine.type_id = 20; mine.name = "MINE";
    foc::AiType upgrade; upgrade.type_id = 50; upgrade.name = "RESEARCH"; upgrade.category_bits = 1;
    host->setup.content.types = {station, mine, upgrade};
    for (const auto& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    host->world = &made.value();
    const auto refresh = [&] {
        host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
    };
    refresh();
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto equations = ai::EquationSet::parse({{"resources.xml", "<Root>"
        "<Resources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type = \"MINE\"}</Resources>"
        "<Desire>Function_Resources.Evaluate > 0</Desire></Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-12: resource-gated upgrade equation parses");
    if (!equations) return;
    ai::AiData data; data.equations = std::move(equations).value();
    ai::PlanDef plan; plan.ignore_target = true; plan.allow_free_store = false; plan.required_categories = {1};
    ai::TaskForceDef force;
    ai::TeamDef team; team.types = {50}; team.min_count = team.max_count = 1;
    force.teams = {team}; plan.taskforces = {force};
    ai::Engine engine(host, std::move(data), {plan});
    for (const auto player_id : {2U, 3U}) {
        expect(made.value().production_counts(player_id, 20).owned_player == 0,
            "SAE-12: AI has no own resource structure");
        expect(engine.evaluate("Resources", player_id, nullptr) == ai::real(1)
            && engine.evaluate("Desire", player_id, nullptr) == ai::real(1),
            "SAE-12: human's resource enables both allied AI upgrade desires");
        ai::PlayerAi player; player.player = player_id;
        ai::PotentialPlan potential;
        expect(ai::EngineContractAccess::selection(engine, player, potential, 7)
            && potential.units == std::vector<t::TypeId>{50} && potential.sources == std::vector<std::uint8_t>{2},
            "SAE-12: each AI can select research from the human's shared station");
    }
    eawr::sim::InlineExecutor executor;
    expect(made.value().submit({{0, 2, 0}, {1}, t::BuyPayload{50}}).has_value()
        && made.value().step(executor).has_value(), "SAE-12: AI research starts normally");
    refresh();
    expect(!made.value().build_allowed(3, 1, 50), "WPR-33: teammate reserves the research slot");
    expect(made.value().submit({{1, 2, 1}, {}, t::CancelPayload{1, 0}}).has_value()
        && made.value().step(executor).has_value(), "WPR-31: AI cancels research");
    refresh();
    expect(made.value().build_allowed(3, 1, 50), "WPR-33: cancellation releases the team slot");
    expect(made.value().submit({{2, 3, 0}, {1}, t::BuyPayload{50}}).has_value(), "SAE-12: other AI purchases research");
    for (int tick = 0; tick < 12; ++tick) expect(made.value().step(executor).has_value(), "SAE-12: research completes");
    for (const auto& account : made.value().ledgers())
        expect(account.tech_level == (account.player == 4 ? 1U : 2U), "WPR-22: AI research advances the human and both allies only");
    expect(made.value().stage_remove(2).has_value(), "SAE-12: human's last mine is lost");
    expect(engine.evaluate("Resources", 2, nullptr) == Real{} && engine.evaluate("Desire", 2, nullptr) == Real{},
        "SAE-12: enemy mine cannot keep the allied upgrade desire enabled");
    const auto counted = made.value().production_counts(2, 50);
    expect(counted.owned_allies == 1 && counted.owned_player == 0,
        "SAE-12: held research ownership is queryable independently of the buyer");
    expect(made.value().submit({{14, 2, 2}, {1}, t::BuyPayload{20}}).has_value()
        && made.value().step(executor).has_value(), "SAE-12: queue a type used by the resource filter");
    expect(made.value().production_counts(2, 20).queued_allies == 1
        && engine.evaluate("Resources", 2, nullptr) == Real{}, "SAE-12: queue entries are not completed resource structures");
    for (int tick = 0; tick < 52; ++tick) expect(made.value().step(executor).has_value(), "SAE-12: purchase reaches pool");
    expect(made.value().production_counts(2, 20).current_allies == 1
        && made.value().production_counts(2, 20).owned_allies == 0
        && engine.evaluate("Resources", 2, nullptr) == Real{}, "SAE-12: undeployed pool entries are not completed resource structures");

    auto neutral_start = start;
    neutral_start.players.push_back({5, 1, 300, 0});
    neutral_start.units.push_back({4, 20, 5});
    neutral_start.units.push_back({8, 21, 5});
    std::sort(neutral_start.units.begin(), neutral_start.units.end(), [](const auto& a, const auto& b) {
        return a.entity_id < b.entity_id;
    });
    t::CombatTable neutral_rules;
    neutral_rules.pad_neutral_factions = {300};
    auto neutral_world = t::TacticalSession::create(neutral_start, {}, {}, {}, {}, neutral_rules, {}, {}, rules);
    expect(static_cast<bool>(neutral_world), "SAE-12: neutral owner may share an ally's raw team number");
    if (neutral_world) {
        auto& neutral_pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(neutral_world.value().pads());
        neutral_pads.at(6).constructed = 2; neutral_pads.at(7).constructed = 3; neutral_pads.at(8).constructed = 4;
        const foc::AiPlayer neutral_player{5, "NEUTRAL", true, false, ""};
        host->players.emplace(5, &neutral_player);
        host->world = &neutral_world.value();
        host->snapshot = neutral_world.value().snapshot();
        host->view = foc::detail::build_view(neutral_world.value(), *host->snapshot);
        expect(host->neutral(5), "SAE-12: neutral relationship is also bound in the AI host");
        expect(engine.evaluate("Resources", 2, nullptr) == ai::real(1),
            "SAE-12: bound neutral owner is excluded despite its matching team number");
        expect(neutral_world.value().production_counts(5, 20).owned_allies == 0,
            "SAE-12: neutral buyers have no allied resource view");
        expect(neutral_world.value().stage_remove(2).has_value(), "SAE-12: lose the only non-neutral allied mine");
        expect(engine.evaluate("Resources", 2, nullptr) == Real{} && engine.evaluate("Desire", 2, nullptr) == Real{},
            "SAE-12: a neutral same-team mine cannot enable upgrade desires");
    }
}


void completed_purchases_are_available() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}};
    start.units = {{1, 10, 1}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 3, 3, 1, true},
                            {51, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: shared production pool fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->setup.perception.campaign_game = false;
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    foc::AiType factory;
    factory.type_id = 10; factory.star_base = true;
    host->types.emplace(10, &factory);
    ai::PlanDef definition;
    definition.allow_free_store = true;
    definition.ignore_target = true;
    ai::TaskForceDef force;
    ai::TeamDef team;
    team.min_count = team.max_count = 1;
    team.types = {50};
    force.teams = {team};
    definition.taskforces = {force};
    ai::Engine engine(host, {}, {definition});
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: shared production scheduler starts");
    if (!scripts) return;
    auto& player = ai::EngineContractAccess::add_player(engine, 1);
    ai::EngineContractAccess::seed_purchases(engine, player, scripts.value());
    ai::EngineContractAccess::service(engine, player, 1);
    expect(ai::EngineContractAccess::orders(engine) == 2, "SAE-03: both build tasks issue purchases");
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value()
        && made.value().submit({{0, 1, 1}, {1}, t::BuyPayload{51}}).has_value(),
        "SAE-03: purchases enter the ordinary queue");
    eawr::sim::InlineExecutor executor;
    for (int frame = 0; frame < 5; ++frame)
        expect(made.value().step(executor).has_value(), "SAE-03: first purchase completes before the second");
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::EngineContractAccess::service(engine, player, 6);
    expect(player.tasks.size() == 1 && player.tasks.front().type == 51 && !engine.block(1)->finished,
        "SAE-03: the producing block still waits for its later purchase");
    expect(engine.taskforce(1)->pooled.empty() && player.reserved_pool_tokens.empty()
        && player.reserved_pool.empty(), "SAE-03: new production does not reserve or attach the completed pool entry");
    ai::PotentialPlan potential;
    expect(ai::EngineContractAccess::selection(engine, player, potential, 67, 2)
        && potential.units == std::vector<t::TypeId>{50} && potential.sources == std::vector<std::uint8_t>{1}
        && potential.pool_tokens.size() == 1 && potential.pool_tokens.front() != 0,
        "SAE-03: another goal selects the completed purchase while production remains blocked");
    if (!potential.pool_tokens.empty()) {
        player.reserved_pool_tokens[potential.pool_tokens.front()] = 3;
        ++player.reserved_pool[50];
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 67, 2),
            "SAE-03/11: a subsequently reserved pool entry remains unavailable to another goal");
    }
}


void reinforcement_work_budget() {
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const m::Vec3 requested{q(-1000), q(50), q(8)};
    expect(ai::reinforcement_candidate(requested, 0) == requested, "SAE-10: radius zero tests the requested point");
    const auto first = ai::reinforcement_candidate(requested, 1);
    const auto second = ai::reinforcement_candidate(requested, 2);
    const auto next = ai::reinforcement_candidate(requested, 11);
    expect(first && first->x == q(-1500) && first->y == q(50), "SAE-10: first angle is reverse arrival facing at radius 500");
    expect(first && second && second->x > first->x && second->y < first->y, "SAE-10: second angle rotates positive 36 degrees");
    expect(next && next->x == q(-2000) && next->y == q(50), "SAE-10: next ring adds 500 and restarts its angle");
    expect(ai::reinforcement_candidate(requested, 211).has_value(), "SAE-10: search continues beyond the former 10000-unit cutoff");
    for (std::uint32_t attempt = 0; attempt < 221; ++attempt) {
        const auto candidate = ai::reinforcement_candidate(requested, attempt);
        expect(candidate && candidate->z == requested.z, "SAE-10: candidate geometry preserves the requested plane");
    }
    const std::array bounds{q(-2000), q(-2000), q(2000), q(2000)};
    const auto clamped = ai::reinforcement_candidate(requested, 81, {}, bounds);
    expect(clamped && clamped->x == q(-2000), "SAE-10: candidate clamps to playable bounds");
    const auto rotated = ai::reinforcement_candidate({}, 11, q(90));
    expect(rotated && rotated->x == q(0) && rotated->y == q(-1000), "SAE-10: ring origin follows authored arrival yaw");
}


void duplicate_reinforcement_calls() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: duplicate-call fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
    auth::ServiceReport report;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        auth::ScriptCommand call;
        call.issuer = 1;
        call.sequence = sequence;
        call.verb = std::string(ai::verb_ai);
        call.arguments = {auth::Value{std::string("reinforce")},
            auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
        report.commands.push_back(std::move(call));
    }
    expect(static_cast<bool>(engine.after_service(report)), "SAE-11: repeated Reinforce calls are taken");
    const auto* first = engine.block(ai::block_id(1, 1));
    const auto* second = engine.block(ai::block_id(1, 2));
    expect(first != nullptr && first == second && !first->finished,
        "SAE-11: both Lua handles wait on the same unfinished operation");
    const auto& orders = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(orders.size() == 1, "SAE-11: repeated calls emit exactly one request for the reserved purchase");
    if (orders.size() == 1) {
        const auto* token = std::get_if<auth::Handle>(&orders[0].arguments[4].data);
        expect(token != nullptr && token->id == 1, "SAE-11: request carries the first reservation's identity");
    }
    expect(player.reserved_pool_tokens.size() == 2 && engine.taskforce(1)->pooled.size() == 2,
        "SAE-11: submission keeps both reservations until authoritative admission");
    auto view = std::make_shared<foc::detail::WorldView>(*host->view);
    foc::detail::ViewUnit converted;
    converted.id = 50;
    converted.owner = 1;
    converted.type = 99;
    converted.purchase_type = 1;
    converted.purchase_token = 1;
    view->units.push_back(converted);
    host->view = view;
    const auto& next = ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{50}
        && engine.taskforce(1)->pooled.size() == 1 && !player.reserved_pool_tokens.contains(1),
        "SAE-11/WHE-49: a converted hull joins by logical purchase type and releases its exact reservation");
    expect(next.size() == 1 && engine.block(ai::block_id(1, 2)) == first && !first->finished,
        "SAE-11: both callers continue waiting while the second purchase reinforces");
    converted.id = 51;
    converted.purchase_token = 2;
    view->units.push_back(converted);
    const auto& done = ai::EngineContractAccess::reinforce_service(engine, player, 3);
    expect(done.empty() && first->finished && first->result && player.reserved_pool_tokens.empty()
        && engine.taskforce(1)->pooled.empty() && engine.taskforce(1)->units.size() == 2,
        "SAE-11: both handles finish together after each reserved purchase joins once");
}


void taskforce_attachment_waits_for_arrival() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    std::optional<std::uint64_t> baseline;
    for (const bool abandoned : {false, true}) {
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, t::player_flag_commandable}};
            setup.units = {{1, 10, 1}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "EX-13: arrival attachment fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "EX-13: buy reserved pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "EX-13: purchase reaches pool");
            auto host = ai::make_tactical_host();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            foc::AiType type;
            type.type_id = 1;
            type.locomotor = true;
            host->types.emplace(1, &type);
            ai::Engine engine(host, {}, {ai::PlanDef{}});
            auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
            auth::ServiceReport report;
            auth::ScriptCommand call;
            call.issuer = call.sequence = 1;
            call.verb = std::string(ai::verb_ai);
            call.arguments = {auth::Value{std::string("reinforce")},
                auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
            report.commands.push_back(std::move(call));
            expect(engine.after_service(report).has_value(), "EX-13: stage reinforcement block");
            const auto& requests = ai::EngineContractAccess::reinforce_service(engine, player, 2);
            expect(requests.size() == 1, "EX-13: submit one reserved purchase");
            const auto block_id = ai::block_id(1, 1);
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}, 1}},
                {block_id, 0}).has_value(), "EX-13: authoritative reinforcement admission retains the reserved purchase token");
            expect(world.step(executor).has_value() && !world.arrivals().empty(), "EX-13: ship exists during hyperspace");
            const auto arrival_tick = world.completed_tick();
            const auto incoming = world.arrivals().begin()->first;
            if (abandoned) {
                auth::SessionConfig config;
                config.tick_duration = auth::TickDuration{1, 30};
                auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
                expect(scripts.has_value(), "EX-13: abandoned-plan scheduler starts");
                if (!scripts) continue;
                ai::EngineContractAccess::abandon(engine, player, scripts.value());
                expect(engine.taskforce(1) == nullptr && player.reserved_pool_tokens.empty(),
                    "EX-13: plan exit releases its reservation while the ship is still incoming");
            }
            while (!world.arrivals().empty() && world.completed_tick() < arrival_tick + t::arrival_frames + 1) {
                host->snapshot = world.snapshot();
                host->view = foc::detail::build_view(world, *host->snapshot);
                const auto& pending = ai::EngineContractAccess::reinforce_service(engine, player,
                    static_cast<std::int64_t>(world.completed_tick()));
                expect(pending.empty(), "EX-13: no duplicate request during hyperspace");
                if (!abandoned) {
                    expect(engine.taskforce(1)->units.empty() && player.reserved_pool_tokens.contains(1),
                        "EX-13: incoming ship stays unattached and reserved");
                    expect(!engine.block(block_id)->finished, "EX-13: block waits while ship is incoming");
                }
                const auto* unit = host->view->find(incoming);
                expect(unit != nullptr && !ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: incoming ship is unavailable to freestore even after its plan releases the reservation");
                expect(world.step(executor).has_value(), "EX-13: hyperspace advances");
            }
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::EngineContractAccess::reinforce_service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
            if (abandoned) {
                const auto* unit = host->view->find(incoming);
                expect(world.arrivals().empty() && unit != nullptr && ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: unload exposes an abandoned plan's incoming ship to freestore");
            } else {
                expect(world.arrivals().empty() && engine.taskforce(1)->units.size() == 1
                    && !player.reserved_pool_tokens.contains(1) && engine.taskforce(1)->pooled.size() == 1,
                    "EX-13: unload attaches the exact ship once and releases only its reservation");
            }
            if (!baseline) baseline = world.completed_tick();
            else expect(world.completed_tick() == *baseline, "EX-13: attachment tick agrees on 1/2/4/8 workers");
        }
    }
}


void concurrent_reserved_arrivals() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: concurrent-arrival fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    auto view = std::make_shared<ai::WorldView>();
    host->view = view;
    foc::AiType type;
    type.type_id = 1;
    type.locomotor = true;
    host->types.emplace(1, &type);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::concurrent_fixture(engine);
    const auto& requested = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(requested.size() == 2, "SAE-11: distinct TaskForces may request independent purchases together");
    // Admission order differs from block order; identical type and ascending entity IDs
    // cannot identify the reserved TaskForce. The admitted purchase token can.
    ai::ViewUnit second;
    second.id = 50;
    second.owner = 1;
    second.type = second.purchase_type = 1;
    second.purchase_token = 3;
    auto first = second;
    first.id = 51;
    first.purchase_token = 1;
    view->units = {second, first};
    expect(!ai::EngineContractAccess::in_freestore(engine, first)
        && !ai::EngineContractAccess::in_freestore(engine, second),
        "SAE-11: admitted purchases remain reserved before their TaskForce attachment service");
    auto unreserved = first;
    unreserved.purchase_token = 999;
    expect(ai::EngineContractAccess::in_freestore(engine, unreserved),
        "SAE-11: a purchase token without a live reservation does not exclude a unit from freestore");
    ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{51}
        && engine.taskforce(2)->units == std::vector<eawr::sim::EntityId>{50},
        "SAE-11: inverted same-type arrivals join their exact reserved TaskForce");
    expect(player.reserved_pool_tokens.size() == 2 && player.reserved_pool_tokens.contains(2)
        && player.reserved_pool_tokens.contains(4),
        "SAE-11: each arrival releases only its own reserved purchase");
}


void reinforcement_service_budget() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // The blocked-ring and second-angle cases exercise prevention and shared static collision.
    for (const auto radius : {750, 1250}) {
        std::vector<std::string> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2},
                {3, 30, 1, {q(-1000), {}, {}}}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(radius)}};
            t::MotionTable motion;
            motion.avoidance = t::AvoidanceRules{q(24), q(1), q(100), q(1), q(15), q(1), q(1), q(1), q(1), q(1), q(1),
                3500, 6, 90, 45, q(50)};
            motion.footprints = {{1, t::SpaceLayer::corvette, q(10), q(20), q(20), false},
                {30, t::SpaceLayer::static_object, q(100), q(100), q(100), true}};
            auto made = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "SAE-10: ring fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: ring fixture buys pooled unit");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: purchase reaches pool before search");
            const auto before_search = world.record().commands.size();
            const auto wallet = world.ledgers().front().credits;
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}}}, {7, 0}).has_value(),
                "SAE-10: submit initial whole-ring search");
            const auto first = world.step(executor);
            expect(static_cast<bool>(first), "SAE-10: whole ring executes in one tick");
            if (!first) continue;
            const auto result = world.reinforcement_search_result(1, 7);
            expect(result.has_value(), "SAE-10: ring result returns to execution service");
            if (!result) continue;
            expect(result->predictions == 0, "SAE-10: ring queries materialize no private prediction samples");
            std::vector<std::string> trace{first.value().state_sha256};
            if (radius == 1250) {
                expect(!result->valid && result->candidates == 10 && result->next_attempt == 21,
                    "SAE-10: blocked radius-1000 ring tests all ten angles in one service and advances to 1500");
                expect(std::none_of(first.value().snapshot->events().begin(), first.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == t::EventKind::order_rejected
                        || event.kind == t::EventKind::order_accepted; }),
                    "SAE-10: a blocked search does not submit an ordinary command");
                expect(world.record().commands.size() == before_search,
                    "SAE-10: a blocked search is not replay-recorded");
                expect(world.ledgers().front().credits == wallet
                    && world.ledgers().front().pool == std::vector<t::TypeId>{1},
                    "SAE-10: a blocked search consumes neither credits nor its pooled unit");
                auto blocked = world.reinforcement_point(1, 1, result->position);
                expect(blocked && !blocked.value(), "WR-22: the shared placement predicate rejects the blocked ring point");
                expect(first.value().reinforcement_rejections_by_check[1] >= 10,
                    "SAE-10: every prevention-blocked angle gets a placement verdict");
                expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 2}, {}, t::ReinforcePayload{1, {}}}, {7, result->next_attempt}).has_value(),
                    "SAE-10: next service submits next ring");
                const auto second = world.step(executor);
                expect(second.has_value(), "SAE-10: next radius executes");
                if (!second) continue;
                const auto free = world.reinforcement_search_result(1, 7);
                expect(free && free->valid && free->candidates == 1 && free->position == *ai::reinforcement_candidate({}, 21),
                    "SAE-10: radius 1500 restarts at reverse facing and admits promptly");
                trace.push_back(second.value().state_sha256);
            } else {
                expect(result->valid && result->candidates == 2 && result->position == *ai::reinforcement_candidate({}, 12),
                    "SAE-10: prevention overlap skips requested/500 points and static first-angle blocker selects second angle at 1000");
                expect(first.value().reinforcement_collision_queries >= 2,
                    "SAE-10: shared collision views check the ring and ordinary command admission");
            }
            const auto replay = world.record();
            const auto bytes = t::write_replay(replay);
            expect(bytes.has_value(), "SAE-10: resolved command uses the existing replay format");
            if (bytes) {
                const auto decoded = t::parse_replay(bytes.value());
                expect(decoded && decoded.value() == replay, "SAE-10: resolved ordinary replay round-trips without search metadata");
            }
            expect(std::holds_alternative<t::ReinforcePayload>(replay.commands.back().payload),
                "SAE-10: record contains the ordinary resolved reinforcement");
            auto replayed = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(replayed.has_value(), "SAE-10: replay fixture starts");
            if (replayed) {
                for (const auto& command : replay.commands) expect(replayed.value().submit(command).has_value(), "SAE-10: resolved commands replay");
                for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) expect(replayed.value().step(executor).has_value(), "SAE-10: ordinary replay advances");
                expect(replayed.value().snapshot()->canonical_bytes() == world.snapshot()->canonical_bytes(),
                    "SAE-10: ordinary resolved command replay reproduces the searched world");
            }
            if (workers == 1) baseline = trace;
            else expect(trace == baseline, "SAE-10: ring search and world hashes agree on 1/2/4/8 workers");
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 10}, {}, t::ReinforcePayload{1, {}}},
                {9, 21}).has_value(), "SAE-10: no-pool search uses ordinary admission");
            const auto empty_pool = world.step(executor);
            const auto held = world.reinforcement_search_result(1, 9);
            expect(empty_pool && held && held->candidates == 0 && held->next_attempt == 21
                && std::none_of(empty_pool.value().snapshot->events().begin(), empty_pool.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == t::EventKind::order_rejected
                        || event.kind == t::EventKind::order_accepted; }),
                "SAE-03/10: absent pool entry does no ring work and retains its radius");
        }
    }
}


void reinforcement_gravity_well() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const auto decimal = [](std::string_view value) { return m::Fixed::from_decimal(value).value(); };
    // SAE-10: two prevention-blocked points from the lone-squadron economy probe.
    // A synthetic neutral well at the origin uses the authored 2200-unit prevention radius.
    const std::array<m::Vec3, 2> points{{{decimal("-795.585"), decimal("-427.722"), {}},
        {decimal("-600.219"), decimal("-887.974"), {}}}};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<std::string> trace;
        for (const auto& requested : points) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(2200)}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "SAE-10: blocked gravity-well fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: gravity fixture purchases its pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: gravity purchase reaches pool before search");
            auto host = ai::make_tactical_host();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::Engine engine(host, {}, {});
            auto& player = ai::EngineContractAccess::add_player(engine, 1);
            ai::EngineContractAccess::seed_reinforcements(engine, player, 1, true);
            ai::EngineContractAccess::reinforce_at(engine, requested);
            const auto block_token = ai::block_id(ai::first_plan_instance + 1, 17);
            std::uint32_t attempts = 0;
            for (std::int64_t service = 1; service <= 4 && world.arrivals().empty(); ++service) {
                const auto& commands = ai::EngineContractAccess::reinforce_service(engine, player, service);
                expect(commands.size() == 1 && commands.front().arguments.size() == 7,
                    "SAE-10: AI emits one ring request for the blocked pool entry");
                if (commands.empty()) break;
                const auto translated = foc::tactical_ai_detail::translate_order(commands.front());
                expect(translated && translated.value().reinforcement_search
                    && translated.value().reinforcement_search->token == block_token,
                    "SAE-10: command translation preserves the full plan block token");
                if (!translated || !translated.value().reinforcement_search) break;
                const auto tick = world.completed_tick();
                expect(world.submit_reinforcement_search({{tick, 1, static_cast<std::uint64_t>(service)}, {},
                    translated.value().payload}, *translated.value().reinforcement_search).has_value(),
                    "SAE-10: gravity request reaches ordinary command path");
                const auto stepped = world.step(executor);
                expect(stepped.has_value(), "SAE-10: gravity ring advances");
                if (!stepped) break;
                const auto result = world.reinforcement_search_result(1, block_token);
                expect(result && result->candidates <= 10 && result->predictions == 0,
                    "SAE-10: gravity search remains bounded per service with no private predictions");
                if (result) attempts += result->candidates;
                host->snapshot = stepped.value().snapshot;
                host->view = foc::detail::build_view(world, *host->snapshot);
                trace.push_back(stepped.value().state_sha256);
            }
            expect(!world.arrivals().empty() && attempts <= 40,
                "SAE-10: prevention-blocked gravity-well request reinforces within four services");
            for (const auto& command : world.record().commands) {
                if (const auto* reinforce = std::get_if<t::ReinforcePayload>(&command.payload)) {
                    expect(reinforce->position != requested, "SAE-10: blocked gravity request never retries the requested point");
                }
            }
        }
        if (workers == 1) baseline = trace;
        else expect(trace == baseline, "SAE-10: blocked gravity search agrees across 1/2/4/8 workers");
    }
}


void reinforcement_fog_persistence() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 20}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    const t::FogRules fog{q(-12000), q(12000), q(1000), 24, 24};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, fog, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: persistent fog fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(4);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: fog fixture buys its pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: fog purchase reaches pool before search");
    std::uint32_t attempt = 0;
    std::uint32_t candidates = 0;
    for (std::uint64_t service = 1; service <= 24; ++service) {
        expect(world.submit_reinforcement_search({{world.completed_tick(), 1, service}, {}, t::ReinforcePayload{1, {}}},
            {8, attempt}).has_value(), "SAE-10: persistent fog submits the next ring");
        expect(world.step(executor).has_value(), "SAE-10: fogged ring executes");
        const auto result = world.reinforcement_search_result(1, 8);
        expect(result && !result->valid && result->candidates == (service == 1 ? 1U : 10U),
            "SAE-10: radius zero checks once; subsequent fogged rings each check ten points");
        if (!result) break;
        attempt = result->next_attempt;
        candidates += result->candidates;
    }
    expect(candidates == 231 && attempt == 231 && world.ledgers().front().pool.size() == 1,
        "SAE-10: failed fog search passes the former distance cutoff and retains the pool entry");
}


// WST-16 (#1905): the AI's queries are stealth-blind, as in the debug build. A real session cloaks
// player 1's frigate; player 2's production nearest query and goal matcher still find it from the
// published view, for an AI and a human requester alike, and only the attack order on it is
// refused (WST-11).
void stealth_blind_queries() {
    namespace t = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto q = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{q(x), {}, {}}; };
    constexpr t::TypeId frigate = 1, corvette = 2;
    for (const bool requester_ai : {true, false}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        t::TacticalSetup start;
        start.seed = 1905;
        start.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
        start.units = {{1, frigate, 1, at(0), math::identity_quat(), {}}, {2, corvette, 2, at(300), math::identity_quat(), {}}};
        t::DurabilityTable health;
        health.rules = {math::Fixed::from_raw(math::Fixed::scale / 5), math::Fixed::from_raw(math::Fixed::scale * 2 / 5),
            math::Fixed::from_raw(math::Fixed::scale / 3)};
        health.profiles = {{frigate, q(1000), q(1), false, {}}, {corvette, q(1000), q(1), false, {}}};
        t::CombatProfile cloaked; cloaked.type_id = frigate; cloaked.max_attack_distance = q(700);
        cloaked.collision = t::CollisionBox{math::Vec3{q(-10), q(-10), q(-10)}, math::Vec3{q(10), q(10), q(10)}};
        t::CombatProfile hunter = cloaked; hunter.type_id = corvette;
        t::CombatTable combat; combat.profiles = {cloaked, hunter};
        t::AbilityProfile cloak;
        cloak.kind = t::AbilityKind::stealth; cloak.expiration_frames = 300; cloak.recharge_frames = 90;
        cloak.gui_activated_ability_name = "Stealth_Probe";
        t::SpecialAbilityProfile handler;
        handler.name = "Stealth_Probe"; handler.kind = t::SpecialAbilityKind::stealth;
        handler.style = t::SpecialActivationStyle::user_input;
        handler.service_interval = t::special_service_interval(t::SpecialAbilityKind::stealth);
        t::UnitAbilityProfile cloaker{frigate, {cloak}, false};
        cloaker.special = {handler};
        t::AbilityTable abilities; abilities.profiles = {cloaker};
        const std::vector<t::SensorProfile> sensors{{frigate, q(2000)}, {corvette, q(2000)}};
        auto made = t::TacticalSession::create(start, sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(made), "WST-16: stealth fixture starts");
        if (!made) { std::cerr << made.error().message << '\n'; return; }
        auto& session = made.value();
        expect(session.submit({{0, 1, 0}, {1}, t::AbilityPayload{t::AbilityKind::stealth, t::AbilityAction::activate}}).has_value(),
            "WST-16: the cloak command submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        // WST-09: hidden once more than 30 frames passed since the switch-on (detection frame 0).
        for (int frame = 0; frame < 40; ++frame) expect(session.step(executor).has_value(), "WST-16: fixture steps");
        auto host = ai::make_tactical_host();
        foc::AiType ship;
        ship.type_id = frigate; ship.name = "PROBE_FRIGATE"; ship.category_bits = 2;
        ship.combat_power = ai::real(1000); ship.max_attack_distance = ai::real(500); ship.locomotor = true;
        auto gun = ship; gun.type_id = corvette; gun.name = "PROBE_CORVETTE";
        host->setup.content.types = {ship, gun};
        host->setup.content.categories.emplace("CORVETTE", 2);
        host->setup.players = {foc::AiPlayer{1, "REBEL", false, false, ""}, foc::AiPlayer{2, "EMPIRE", false, requester_ai, ""}};
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type);
            host->types_by_name.emplace(type.name, &type);
        }
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        host->world = &session;
        host->snapshot = session.snapshot();
        host->view = foc::detail::build_view(session, *host->snapshot);
        const auto instances = host->snapshot->instances();
        const auto self = std::find_if(instances.begin(), instances.end(), [](const auto& item) { return item.entity_id == 1; });
        const auto* viewed = host->view->find(1);
        expect(self != instances.end() && self->stealth_invisible && viewed != nullptr
            && !foc::detail::object_fogged(*host->view, *viewed, 2),
            "WST-16: the cloaked frigate is stealth-invisible yet unfogged in the AI's published view");
        auth::ModuleManifest manifest;
        expect(manifest.add("NEAR.LUA",
            "function Check(source) Record(Find_Nearest(source, \"Corvette\", source.Get_Owner(), false)) end\n").has_value(),
            "WST-16: nearest fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WST-16: nearest scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(scripts.value().register_binding("Record", [](auth::BindingContext& context, const auth::ValueList& args) {
            context.issue_command("nearest", args);
            return eawr::core::Result<auth::ValueList>::success({});
        }).has_value(), "WST-16: nearest recorder registers");
        expect(errors.empty() && scripts.value().create_instance(1, "NEAR.LUA").has_value(), "WST-16: nearest instance starts");
        auth::ScriptEvent event;
        event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WST-16: nearest query submits");
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty() && report.value().commands.size() == 1,
            "WST-16: production nearest query runs");
        if (report && report.value().commands.size() == 1) {
            const auto& args = report.value().commands.front().arguments;
            const auto* found = args.empty() ? nullptr : std::get_if<auth::Handle>(&args.front().data);
            expect(found != nullptr && found->id == 1, "WST-16: Find_Nearest still returns the cloaked enemy");
        }
        ai::Engine engine(host, {}, {});
        ai::Target target; target.object = 1;
        expect(ai::EngineContractAccess::matches(engine, 2, {"ENEMY_UNIT"}, target),
            "WST-16: the goal matcher still admits the cloaked enemy");
        expect(session.submit({{session.completed_tick(), 2, 0}, {2}, t::AttackPayload{1}}).has_value(),
            "WST-16: the attack order submits");
        const auto stepped = session.step(executor);
        bool hidden = false;
        if (stepped) for (const auto& item : stepped.value().snapshot->events())
            hidden = hidden || (item.kind == t::EventKind::order_rejected && item.reason == t::RejectReason::target_hidden && item.unit == 2);
        expect(hidden, "WST-11/16: only the attack order on the cloaked enemy is refused");
    }
}


// #1891 (AB-44): the AI's ability verb takes every modelled ability kind and every action, so a
// script's MISSILE_SHIELD and SENSOR_JAMMING (and ION_CANNON_SHOT's command) reach the session;
// unmodelled kinds and out-of-range actions stay malformed.
void ai_ability_bound() {
    namespace math = eawr::sim::math;
    const auto command = [](const std::int64_t unit, const std::int64_t kind, const std::int64_t action) {
        auth::ScriptCommand result;
        result.issuer = 1;
        result.verb = std::string(foc::verb_ability);
        result.arguments = {foc::detail::number(Real(1)), foc::detail::handle(foc::handle_game_object, static_cast<std::uint64_t>(unit)),
            foc::detail::number(Real(static_cast<int>(kind))), foc::detail::number(Real(static_cast<int>(action)))};
        return result;
    };
    for (const auto kind : {tactical::AbilityKind::defend, tactical::AbilityKind::spoiler_lock,
             tactical::AbilityKind::ion_cannon_shot, tactical::AbilityKind::hunt, tactical::AbilityKind::missile_shield,
             tactical::AbilityKind::sensor_jamming, tactical::AbilityKind::laser_defense, tactical::AbilityKind::leech_shields}) {
        for (const auto action : {tactical::AbilityAction::activate, tactical::AbilityAction::autofire_off}) {
            const auto order = foc::tactical_ai_detail::translate_order(command(1, static_cast<std::int64_t>(kind),
                static_cast<std::int64_t>(action)));
            const auto* payload = order ? std::get_if<tactical::AbilityPayload>(&order.value().payload) : nullptr;
            expect(payload != nullptr && payload->ability == kind && payload->action == action,
                "#1891: the AI ability verb carries kind " + std::to_string(static_cast<int>(kind)));
        }
    }
    auto targeted = command(1, 23, 1);
    targeted.arguments.push_back(foc::detail::handle(foc::handle_game_object, 2));
    const auto translated = foc::tactical_ai_detail::translate_order(targeted);
    const auto* payload = translated ? std::get_if<tactical::AbilityPayload>(&translated.value().payload) : nullptr;
    expect(payload && payload->ability == tactical::AbilityKind::leech_shields && payload->target == 2,
        "WLS-04: the hosted AI command carries the shield-leech object target");
    targeted.arguments.back() = foc::detail::number(Real(2));
    expect(!foc::tactical_ai_detail::translate_order(targeted), "WLS-04: a numeric target is malformed");
    for (const auto& [kind, action] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {0, 1}, {255, 1}, {256, 1}, {-1, 1}, {15, 0}, {15, 5}}) {
        expect(!foc::tactical_ai_detail::translate_order(command(1, kind, action)),
            "#1891: an unmodelled kind or an out-of-range action stays malformed");
    }
    const auto q = [](const std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    tactical::AbilityProfile shield;
    shield.kind = tactical::AbilityKind::missile_shield;
    shield.effective_radius = q(100);
    shield.expiration_frames = 300;
    tactical::AbilityProfile jammer = shield;
    jammer.kind = tactical::AbilityKind::sensor_jamming;
    tactical::AbilityTable abilities;
    abilities.profiles = {{11, {shield}}, {12, {jammer}}};
    tactical::CombatTable combat;
    for (const tactical::TypeId type : {11U, 12U}) {
        tactical::CombatProfile profile;
        profile.type_id = type;
        combat.profiles.push_back(profile);
    }
    tactical::TacticalSetup setup;
    setup.seed = 1891;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{1, 11, 1, {}, math::identity_quat(), {}}, {2, 12, 1, {q(500), {}, {}}, math::identity_quat(), {}}};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto made = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat, {}, abilities);
        expect(made.has_value(), "#1891: projectile defence fixture starts");
        if (!made) continue;
        auto& world = made.value();
        std::uint64_t sequence = 0;
        for (const auto& [unit, kind] : std::vector<std::pair<std::int64_t, tactical::AbilityKind>>{
                 {1, tactical::AbilityKind::missile_shield}, {2, tactical::AbilityKind::sensor_jamming}}) {
            const auto order = foc::tactical_ai_detail::translate_order(command(unit, static_cast<std::int64_t>(kind),
                static_cast<std::int64_t>(tactical::AbilityAction::activate)));
            expect(static_cast<bool>(order), "#1891: the AI's projectile defence request translates");
            if (order) expect(world.submit({{0, order.value().issuer, sequence++}, order.value().units, order.value().payload}).has_value(),
                "#1891: the AI's projectile defence request submits");
        }
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int frame = 0; frame < 3; ++frame) {
            const auto stepped = world.step(executor);
            expect(stepped.has_value(), "#1891: projectile defence fixture steps");
            if (stepped) hashes.push_back(stepped.value().state_sha256);
        }
        const auto shielded = world.ability_state(1);
        const auto jamming = world.ability_state(2);
        expect(shielded && shielded->slots[0].active && jamming && jamming->slots[0].active,
            "#1891: MISSILE_SHIELD and SENSOR_JAMMING switched by the AI are on in the session");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "#1891: AI-switched defences hash alike at 1/2/4/8 workers");
    }
}


void reinforcement_population_wait() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 1}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: population wait fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(2);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy first pool entry");
    expect(world.submit({{0, 1, 1}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy second pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: both pool entries complete");
    expect(world.submit({{2, 1, 2}, {}, t::ReinforcePayload{1, {q(3000), {}, {}}}}).has_value(),
        "SAE-10: reinforce first unit to fill population");
    expect(world.step(executor).has_value(), "SAE-10: population fills");
    expect(world.submit_reinforcement_search({{3, 1, 3}, {}, t::ReinforcePayload{1, {}}}, {10, 21}).has_value(),
        "SAE-10: search waits for population room");
    const auto waiting = world.step(executor);
    const auto result = world.reinforcement_search_result(1, 10);
    expect(waiting && result && result->candidates == 0 && result->next_attempt == 21
        && world.ledgers().front().pool.size() == 1
        && std::none_of(waiting.value().snapshot->events().begin(), waiting.value().snapshot->events().end(),
            [](const auto& event) { return event.kind == t::EventKind::order_rejected
                || event.kind == t::EventKind::order_accepted; }),
        "SAE-03/10: no population room preserves the pool and current ring without placement work");
}

} // namespace foc_contract

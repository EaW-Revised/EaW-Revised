#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_selection_content.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdlib>
#include <array>
#include <charconv>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace {
namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace cp = eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using ai::real;
Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }

ai::AiData equations(const std::vector<std::pair<std::string, std::string>>& files,
    const ai::ConverterFunction& converters = [](std::string_view, std::string_view) -> std::optional<ai::Real> { return {}; }) {
    auto parsed = ai::EquationSet::parse(files, converters);
    if (!parsed) throw std::runtime_error(eawr::core::format_diagnostic(parsed.error()));
    ai::AiData data; data.equations = std::move(parsed).value();
    // The initial provider piece has no lifecycle/plan pump: its typed query
    // manager supplies targets only. A owns installing stock player goal sets.
    data.players["QUERY_COMPONENT"] = ai::PlayerType{};
    return data;
}

struct QueryHost {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    ai::Engine engine;
    auth::ScriptScheduler scripts;
    QueryHost(ai::GalacticPerceptionSetup setup, ai::AiData data)
        : world(std::make_shared<ai::GalacticWorld>(setup)), engine(host, std::move(data), {}),
          scripts(auth::ScriptScheduler::create({}, {}).value()) {
        host->ai_world = world; host->engine = &engine; host->setup.seed = setup.seed;
        for (const auto& player : setup.players)
            host->setup.players.push_back({player.player, player.faction, player.neutral, !player.neutral, "QUERY_COMPONENT", player.human});
        for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
        host->find_target_queries = std::make_shared<ai::GalacticFindTargetQueries>(engine, *world);
    }
    void initialize(const eawr::sim::PartitionExecutor& executor) {
        std::uint64_t sequence{};
        const auto initialized = engine.before_service(*world, scripts, sequence, &executor);
        if (!initialized) throw std::runtime_error(eawr::core::format_diagnostic(initialized.error()));
    }
    std::optional<ai::Real> query(std::string_view name, gc::PlanetId target = 10, gc::PlayerId player = 1) {
        return world->evaluate(engine, name, player, engine.target_of_object(target));
    }
};

ai::GalacticPerceptionSetup fixture_setup() {
    ai::GalacticPerceptionSetup setup;
    setup.seed = 42; setup.uses_fog = true;
    setup.maximum_ground_level = real(5); setup.maximum_space_level = real(5);
    setup.players = {{1, 1, "Empire", "Normal", false, true, false, 10},
        {2, 2, "Rebel", "Hard", false, false, true, 20}, {3, 3, "Pirates", "Easy", true, false, false, 0}};
    setup.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}, {20, "BETA", 2000, {}, {}, {}, {}, {}}, {30, "GAMMA", 3000, {}, {}, {}, {}, {}}};
    foc::AiType structure; structure.type_id = 100; structure.name = "A"; setup.types.push_back(structure);
    structure.type_id = 200; structure.name = "B"; setup.types.push_back(structure);
    setup.cache_intervals = {{"GROUNDBASELEVELUNNORMALIZED", real(1)}, {"STARBASELEVELUNNORMALIZED", real(0)},
        {"STRUCTURECOUNT", real(-1)}, {"HASSTRUCTURE", real(1)}};
    return setup;
}
std::shared_ptr<const gc::GalacticSnapshot> fixture(std::uint64_t frame, std::int64_t ground_level,
    std::int64_t space_level = 2, bool enroute = false) {
    gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = frame;
    gc::PlayerEconomySetup accounts; accounts.players = {{1, whole(123), whole(9999), 3, 5}, {2, whole(456), whole(9999), 2, 5}, {3, {}, whole(9999), 1, 5}};
    gc::ProductionSetup production;
    gc::PlanetProduction first; first.planet = 10; first.target_owner = first.starbase_owner = first.ground_occupier = first.orbit_occupier = 1;
    first.ground_level = ground_level; first.space_level = space_level; first.maximum_space_level = 5;
    first.ground_slots = 5; first.maximum_ground_forces = 6; first.landed_transports = 2;
    first.enabled = first.surface_accessible = true;
    first.objects = {{70, 100, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    if (ground_level >= 4) first.objects.push_back({71, 200, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure});
    production.planets.push_back(first);
    gc::Fleet fleet; fleet.id = 50; fleet.owner = 1; fleet.orbit = 10; fleet.members = {{51, 100}};
    if (enroute) fleet.motion.transit.emplace();
    auto movement = std::make_shared<gc::MovementSetup>(); movement->contents = {{51, {{52, 200}}}};
    std::vector<gc::Planet> planets{{10, {}, 1}, {20, {whole(100), {}, {}}, 2}, {30, {whole(200), {}, {}}, 3}};
    return std::make_shared<gc::GalacticSnapshot>(frame, clock, std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}, {3, 3, 0}},
        planets, std::vector<gc::Route>{{40, 10, 20, whole(1)}, {41, 20, 30, whole(1)}}, std::vector<gc::Fleet>{fleet},
        std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, movement, std::nullopt, std::vector<gc::Seat>{}, accounts, production);
}
ai::AiData fixture_data() {
    return equations({{"fixture.xml", R"(<Equations>
<Ground>Variable_Target.GroundbaseLevelUnnormalized</Ground>
<Space>Variable_Target.StarbaseLevelUnnormalized</Space>
<Normalized>Variable_Target.GroundbaseLevel + Variable_Target.StarbaseLevel</Normalized>
<NormalizedGround>Variable_Target.GroundbaseLevel</NormalizedGround>
<NormalizedSpace>Variable_Target.StarbaseLevel</NormalizedSpace>
<Count>Variable_Target.StructureCount {Parameter_Type="A", Parameter_Type="B"}</Count>
<A>Variable_Target.HasStructure {Parameter_Type="A"}</A>
<B>Variable_Target.HasStructure {Parameter_Type="B"}</B>
<Age>Variable_Target.EnemyForce.GroundTotal.TimeLastSeenUnnormalized</Age>
<FractionalAge>Variable_Target.EnemyForce.GroundTotal.TimeLastSeen</FractionalAge>
<RawSlots>Variable_Target.OpenCompanySlotsUnnormalized</RawSlots>
<OpenStructures>Variable_Target.OpenStructureSlots</OpenStructures>
<Routes>Variable_Target.TradeRoutes</Routes>
<NoRoutes>Variable_Target.ActiveTradeRoutes</NoRoutes>
<EnemyCredits>Variable_Enemy.CreditsUnnormalized</EnemyCredits>
<HumanCredits>Variable_Human.CreditsUnnormalized</HumanCredits>
<Unqualified>Variable_Target.EnemyForce.SpaceTotal</Unqualified>
<Hint>Variable_Target.Hints.Chokepoint</Hint>
<Story>Variable_Target.StoryTrigger {Parameter_Trigger="trigger"}</Story>
<Nested>Function_Ground.Evaluate + 2</Nested>
<Pick>Variable_Target.Type.Token</Pick>
</Equations>)"}});
}

std::string producer_preparation_contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto production = *fixture(1, 1)->production();
    production.maximum_human_queue = 4;
    gc::ProductionPlayer first_player; first_player.player = 1; first_player.population_capacity = whole(100);
    auto second_player = first_player; second_player.player = 2; second_player.human = true;
    production.players = {first_player, second_player};
    auto& location = production.planets.front();
    gc::ProductionOffer ground; ground.type = 100; ground.price = whole(100); ground.seconds = whole(10);
    ground.affiliated = ground.direct_build_allowed = true;
    auto space = ground; space.type = 200; space.domain = gc::ProductionDomain::space;
    location.offers = {ground, space};
    gc::ProductionEntry queued; queued.type = 100; queued.builder = 1; queued.last_remaining = whole(5);
    location.queues[0].push_back(queued);
    gc::PlayerEconomySetup accounts; accounts.players = {{1, whole(500), whole(9999), 3, 5}, {2, whole(500), whole(9999), 3, 5}};
    std::vector<gc::Planet> planets{{10, {}, 1}};
    std::vector<gc::Fleet> fleets;
    std::vector<gc::PopulationTotal> population{{1, whole(100), {}, {}, false}, {2, whole(100), {}, {}, false}};
    std::optional<gc::IncomeState> income{gc::IncomeState{}};
    income->inputs.offers = {{10, 100, whole(100), whole(10)}, {10, 200, whole(100), whole(10)}};
    gc::EconomicType reducer; reducer.type = 100;
    reducer.reductions = {{100, 9, Fixed::from_ratio(1, 4).value(), true, true}};
    income->inputs.types = {reducer}; income->inputs.planets = {{10}};
    income->inputs.sources = {{10, 100, 10, 1, true, true}};
    auto economic_setup = fixture_setup();
    for (const auto id : {100U, 200U, 1000U}) {
        ai::GalacticForceType force; force.type = id; force.categories = id == 200 ? 2U : 1U;
        force.power = real(id == 1000 ? 0 : 42);
        if (id == 100) force.starting_spawns[3] = {{200, 2}};
        economic_setup.force_types.push_back(std::move(force));
        ai::GalacticEconomyType type; type.type = id; type.generic_price = real(100); type.generic_seconds = real(10);
        type.maintenance = real(1); type.ground_unit = id == 100; type.space_unit = id == 200;
        economic_setup.economy_types.push_back(std::move(type));
    }
    economic_setup.faction_maintenance = {{1, real(1)}, {2, real(1)}};
    ai::GalacticWorld warm(economic_setup);
    ai::GalacticWorld deferred(economic_setup);
    deferred.enable_live_deltas(300, std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers));
    std::uint64_t frame{};
    std::ostringstream trace;
    const auto producer_key = [](const auto& row) {
        return std::tie(row.producer, row.planet, row.player, row.type, row.domain, row.admission,
            row.cost, row.build_seconds, row.queue_seconds, row.travel_seconds, row.base_level,
            row.required_level, row.logical_fps, row.reachable, row.distance, row.hyperspace_speed,
            row.faction_speed_factor, row.technology_weight);
    };
    const auto make_snapshot = [&] {
        gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = ++frame;
        auto generation = std::make_shared<gc::EconomyInputGeneration>();
        generation->census = generation->accounts = frame;
        return std::make_shared<gc::GalacticSnapshot>(frame, clock,
            std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}}, planets, std::vector<gc::Route>{}, fleets,
            std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, nullptr, std::nullopt,
            std::vector<gc::Seat>{}, accounts, production, std::nullopt,
            std::vector<gc::PoliticalControlEffect>{}, population, std::nullopt, income, std::nullopt,
            std::vector<gc::ObservedProductionOffer>{}, std::shared_ptr<const std::vector<gc::ObservedPlanet>>{},
            std::nullopt, std::shared_ptr<const std::vector<gc::CensusObject>>{}, std::nullopt, std::nullopt,
            std::nullopt, std::shared_ptr<const gc::PlanetAccessState>{}, 1, generation);
    };
    const auto check_case = [&](bool preparation_expected, eawr::sim::EntityId frontier = 100, bool admission_expected = true) {
        const auto snapshot = make_snapshot();
        expect(static_cast<bool>(warm.bind(snapshot, {}, executor, frame, frontier)), "warm producer preparation binds changed owning inputs");
        ai::GalacticWorld cold(economic_setup);
        expect(static_cast<bool>(cold.bind(snapshot, {}, executor, frame, frontier)), "independent cold producer oracle binds");
        expect(static_cast<bool>(deferred.bind(snapshot, {}, executor, frame, frontier)), "deferred producer catalog binds current snapshot");
        const auto a = warm.copy_input(), b = cold.copy_input();
        const auto deferred_input = deferred.copy_input();
        expect(!deferred.build_option(1, std::numeric_limits<eawr::sim::EntityId>::max(), 100),
            "deferred lookup cannot alias a nonplanet producer by narrowing its identity");
        expect(deferred_input->producers.size() == b->producers.size()
            && std::equal(deferred_input->producers.begin(), deferred_input->producers.end(), b->producers.begin(), b->producers.end(),
                [&](const auto& left, const auto& right) { return producer_key(left) == producer_key(right); }),
            "time-sliced and demand-prepared producer facts equal eager atomic oracle for every changed input");
        const std::array requested{gc::TypeId{100}};
        const auto checks_before = deferred.producer_demand_offer_checks();
        const auto selected = deferred_input->producers.read(1, 0, requested);
        expect(selected && std::all_of(selected.value().begin(), selected.value().end(), [](const auto& row) { return row.player == 1 && row.type == 100; }),
            "deferred query returns only requested player and types");
        expect(selected && deferred.producer_demand_offer_checks() - checks_before == selected.value().size(),
            "requested-type admission examines matching offers only");
        const auto repeated_before = deferred.producer_demand_offer_checks();
        const auto repeated = deferred_input->producers.read(1, 0, requested);
        expect(repeated && selected && repeated.value().size() == selected.value().size()
            && deferred.producer_demand_offer_checks() == repeated_before,
            "same completed-frame query evaluates admission only once");
        const std::array missing{std::numeric_limits<gc::TypeId>::max()};
        const auto missing_before = deferred.producer_demand_offer_checks();
        const auto absent = deferred_input->producers.read(1, 0, missing);
        expect(absent && absent.value().empty() && deferred.producer_demand_offer_checks() == missing_before,
            "unmatched type performs no offer admission checks");
        const std::array mixed{gc::TypeId{200}, gc::TypeId{100}, gc::TypeId{200}, std::numeric_limits<gc::TypeId>::max()};
        const auto mixed_rows = deferred_input->producers.read(1, 0, mixed);
        const auto eager_rows = b->producers.read(1, 0, mixed);
        expect(mixed_rows && eager_rows && mixed_rows.value().size() == eager_rows.value().size()
            && std::equal(mixed_rows.value().begin(), mixed_rows.value().end(), eager_rows.value().begin(),
                [&](const auto& left, const auto& right) { return producer_key(left) == producer_key(right); }),
            "reversed and duplicated requested types preserve exact eager rows");
        const std::array ordered{gc::TypeId{100}, gc::TypeId{200}};
        const auto first_mixed = deferred_input->producers.read(1, production.planets.front().planet, mixed, true);
        const auto first_ordered = deferred_input->producers.read(1, production.planets.front().planet, ordered, true);
        expect(first_mixed && first_ordered && first_mixed.value().size() == first_ordered.value().size()
            && std::equal(first_mixed.value().begin(), first_mixed.value().end(), first_ordered.value().begin(),
                [&](const auto& left, const auto& right) { return producer_key(left) == producer_key(right); }),
            "first allowed offer follows registration order rather than requested-type order");
        expect(a->credits == b->credits && a->producers.size() == b->producers.size()
            && std::equal(a->producers.begin(), a->producers.end(), b->producers.begin(), b->producers.end(),
                [&](const auto& left, const auto& right) { return producer_key(left) == producer_key(right); }),
            "cached producer costs, times and live admission equal independently rebuilt oracle");
        for (const auto& row : a->producers) {
            const auto* option = warm.build_option(row.player, row.producer, row.type);
            const auto* oracle = cold.build_option(row.player, row.producer, row.type);
            expect(option && oracle && *option == *oracle, "mode-neutral build options equal cold oracle");
            const auto* lazy_option = deferred.build_option(row.player, row.producer, row.type);
            expect(lazy_option && oracle && *lazy_option == *oracle, "deferred build option reads current immutable admission facts");
            trace << row.player << ':' << row.type << ':' << row.cost.raw() << ':' << row.build_seconds.raw()
                << ':' << row.queue_seconds.raw() << ':' << static_cast<unsigned>(row.admission) << ';';
        }
        expect((warm.work().producer_offer_preparations != 0) == preparation_expected,
            "producer preparation work follows changed price/time inputs rather than frame or queue clocks");
        expect((warm.producer_admission_preparations() != 0) == admission_expected,
            "producer admission work follows changed owning inputs rather than snapshot clocks");
        const auto query = [&](const ai::GalacticWorld& world, std::string_view body) {
            auto parsed = ai::parse_equation("ECONOMY_DELTA", body, {});
            if (!parsed) throw std::runtime_error(parsed.error().message);
            ai::Target target; target.object = 10; target.name = "ALPHA";
            ai::Context context; context.player = 1; context.enemy = 2; context.target = &target;
            auto host = std::make_shared<foc::detail::Host>(); ai::Engine engine(host, {}, {}); ai::AiRandom random;
            return world.perception_read_only(engine, parsed.value(), context, random);
        };
        for (const auto* body : {"Variable_Self.HasUnit {Parameter_Type=\"A\", Parameter_Count=2}",
            "Variable_Target.StructureCount {Parameter_Type=\"A\", Parameter_Only_Consider_Complete=0}",
            "Variable_Target.HasStructure {Parameter_Type=\"A\", Parameter_Only_Consider_Complete=0}",
            "Variable_Self.SpaceTotalUnnormalized", "Variable_Self.GroundTotalUnnormalized",
            "Variable_Self.MaintenanceUnnormalized", "Variable_Enemy.CreditsUnnormalized"}) {
            const auto expected = query(cold, body);
            expect(expected.has_value() && query(deferred, body) == expected && query(warm, body) == expected,
                "queue/credit economic projection equals cold owned-force, maintenance, queue and credit getters");
        }
        expect(warm.state_hash().value() == cold.state_hash().value(), "derived producer reuse does not change native state");
    };
    {
        ai::GalacticWorld sliced(fixture_setup());
        sliced.enable_live_deltas(2, std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers));
        auto first = make_snapshot();
        expect(static_cast<bool>(sliced.bind(first, {}, executor, frame, 100))
            && sliced.work().producer_offer_preparations == 2,
            "fixed frame slice prepares one of two producer chunks");
        auto second = make_snapshot();
        expect(static_cast<bool>(sliced.bind(second, {}, executor, frame, 100))
            && sliced.work().producer_offer_preparations == 2,
            "next fixed slice completes the other chunk without a whole-catalog sweep");
        ai::GalacticWorld atomic(fixture_setup());
        expect(static_cast<bool>(atomic.bind(second, {}, executor, frame, 100)), "atomic deadline oracle binds");
        const auto a = sliced.copy_input(), b = atomic.copy_input();
        expect(a->producers.size() == b->producers.size()
            && std::equal(a->producers.begin(), a->producers.end(), b->producers.begin(), b->producers.end(),
                [&](const auto& left, const auto& right) { return producer_key(left) == producer_key(right); }),
            "completed deterministic slices equal atomic rebuild at the deadline");
    }
    check_case(true); check_case(false, 100, false);
    const auto retained_input = deferred.copy_input();
    const std::array queued_type{gc::TypeId{100}};
    location.queues[0].front().last_remaining = whole(4); check_case(false);
    expect(deferred.work().prepared_objects == 0 && deferred.work().prepared_planets == 0
        && deferred.work().economy_objects == 0 && deferred.work().economy_rebuilds == 0,
        "queue-clock delta retains physical and owned-force economic publication");
    const auto retained_rows = retained_input->producers.read(1, 10, queued_type);
    const auto current_rows = deferred.copy_input()->producers.read(1, 10, queued_type);
    expect(retained_rows && current_rows && !retained_rows.value().empty() && !current_rows.value().empty()
        && retained_rows.value().front().queue_seconds == whole(5) && current_rows.value().front().queue_seconds == whole(4),
        "queue delta publishes fresh admission clocks without changing a retained input");
    const auto retained_account_input = deferred.copy_input();
    accounts.players[1].credits = {}; check_case(false);
    expect(deferred.work().prepared_objects == 0 && deferred.work().prepared_planets == 0
        && deferred.work().economy_objects == 0 && deferred.work().economy_planets == 0 && deferred.work().economy_rebuilds == 0
        && retained_account_input->credits.at(2) == whole(500) && deferred.copy_input()->credits.at(2) == Fixed{},
        "credit delta publishes current money without rebuilding physical rows or changing retained credits");
    accounts.players[0].current_tech = 0; check_case(false);
    expect(deferred.work().prepared_objects != 0,
        "technology changes still rebuild starting-spawn force inputs");
    location.queues[0].push_back(queued); check_case(false);
    expect(deferred.work().economy_objects == 0, "queue append changes counts without owned-force preparation");
    location.queues[0].front().builder = 2; check_case(false);
    expect(deferred.work().economy_objects == 0, "queue ownership delta updates player counts without changing physical owners");
    location.queues[0].erase(location.queues[0].begin()); check_case(false);
    production.players[0].locked = {100}; check_case(false);
    production.players[0].locked.clear(); location.objects.clear(); check_case(false);
    population[0].uses_cap = true; population[0].used = whole(100); check_case(false);
    check_case(false, std::numeric_limits<eawr::sim::EntityId>::max() - 1);
    location.offers.front().required_structures = {{200}}; check_case(true);
    location.offers.front().required_orbiting_units = {100}; check_case(true);
    gc::Fleet fleet; fleet.id = 50; fleet.owner = 2; fleet.orbit = 10; fleet.members = {{51, 100}};
    fleets.push_back(fleet); check_case(false);
    fleets.front().motion.position.x = whole(17); check_case(false, 100, false);
    fleets.front().motion.facing.y = whole(1); ++fleets.front().motion.orbit_entry;
    check_case(false, 100, false);
    location.queues[0].clear(); check_case(false);
    income->inputs.offers.front().price = whole(240); check_case(true);
    income->inputs.offers.front().seconds = whole(24); check_case(true);
    income->inputs.types.front().reductions.front().reduction = Fixed::from_ratio(1, 2).value(); check_case(true);
    income->inputs.sources.front().enabled = false; check_case(true);
    income->inputs.sources.front().enabled = true; check_case(true);
    planets.front().owner = location.target_owner = location.starbase_owner = 2; check_case(true);
    income->inputs.types.front().reductions.front().galaxy_wide = false; check_case(true);
    income->inputs.planets.front().intimidation = true; check_case(true);
    const auto published = warm.copy_input();
    const auto bases = income->inputs.offers;
    income->inputs.offers.clear();
    const auto broken_snapshot = make_snapshot();
    expect(!warm.bind(broken_snapshot, {}, executor, frame, 100) && warm.copy_input() == published,
        "failed changed offer preparation does not publish partial rows or replace the reusable cache");
    income->inputs.offers = bases; check_case(false, 100, false);
    income.reset(); check_case(true); check_case(false, 100, false);
    location.offers.front().price = whole(123); check_case(true);
    auto duplicate = location; duplicate.offers.front().price = whole(456);
    production.planets.push_back(std::move(duplicate)); check_case(true); check_case(true);
    return trace.str();
}

std::string story_campaign_contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto data = equations({{"story.xml", "<Equations><Story>Game.IsStoryCampaign</Story></Equations>"}});
    std::string trace;
    for (const auto authored : {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
        auto setup = fixture_setup(); setup.is_story_campaign = authored;
        QueryHost host(setup, data);
        expect(static_cast<bool>(host.world->bind(fixture(1, 1), {}, executor, 1, 100)),
            "story campaign query binds actual immutable mode input");
        host.initialize(executor);
        const auto result = host.query("Story");
        expect(authored ? result == real(*authored ? 1 : 0) : !result,
            "WGA-423 returns the authored campaign Boolean; unbound provider remains unavailable");
        const auto saved = host.world->capture().value();
        QueryHost restored(setup, data);
        expect(static_cast<bool>(restored.world->bind(fixture(1, 1), {}, executor, 1, 100)),
            "restored story campaign query rebinds the same prepared content");
        restored.initialize(executor);
        expect(static_cast<bool>(restored.world->restore(saved)) && restored.query("Story") == result
            && restored.world->state_hash().value() == host.world->state_hash().value(),
            "story campaign numeric result and native hash survive fresh-owner restoration");
        trace += host.world->state_hash().value();
    }
    return trace;
}
std::string difficulty_seat_contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto data = equations({{"difficulty.xml", R"(<Equations>
<Normal>Variable_Self.IsDifficulty{Parameter_Difficulty_Level_Type=1}</Normal>
<Hard>Variable_Self.IsDifficulty{Parameter_Difficulty_Level_Type=2}</Hard>
<Missing>Variable_Self.IsDifficulty</Missing>
<AliasNumber>Variable_Self.IsDifficulty{Parameter_Difficulty=1}</AliasNumber>
<AliasName>Variable_Self.IsDifficulty{Parameter_Difficulty="NORMAL"}</AliasName>
</Equations>)"}});
    auto setup = fixture_setup();
    QueryHost host(setup, data);
    expect(static_cast<bool>(host.world->bind(fixture(1, 1), {}, executor, 1, 100)),
        "seat difficulty query binds actual player facts");
    host.initialize(executor);
    expect(host.query("Normal", 10, 1) == real(1) && host.query("Hard", 10, 1) == real(0)
        && !host.query("Missing", 10, 1), "WGA-424 AI seat compares its selected enum and refuses missing parameter");
    expect(host.query("Hard", 10, 2) == real(0) && host.query("Normal", 10, 3) == real(0),
        "WGA-424 human and non-AI seats return numeric zero despite a retained difficulty name");
    expect(!host.query("AliasNumber", 10, 1) && !host.query("AliasName", 10, 1),
        "WGA-424 requires Parameter_Difficulty_Level_Type and refuses unsourced numeric/named aliases");
    return host.world->state_hash().value();
}
std::string contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    QueryHost host(fixture_setup(), fixture_data());
    ai::GalacticConnectivity connectivity; connectivity.revision = 1; connectivity.execution_available = true;
    connectivity.execution_routes = {{1, 10, 20, false, real(7), real(20), real(0)}};
    connectivity.goal_reach = {{1, 10, "Friendly_Only", {30}}};
    const auto bind = [&](std::uint64_t frame, std::int64_t ground, std::int64_t space = 2, bool transit = false) {
        const auto result = host.world->bind(fixture(frame, ground, space, transit), connectivity, executor, frame, 100);
        if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    };
    bind(0, 1); host.initialize(executor);
    host.world->visibility_changed(1, 10, true);
    expect(host.query("Ground") == real(1), "frame-zero getter evaluates");
    bind(0, 2);
    expect(host.query("Ground") == real(2), "WGA-65 cache frame zero never hits");
    bind(1, 1);
    host.world->visibility_changed(1, 10, true);
    expect(host.engine.target_list(1) && host.engine.target_list(1)->size() == 3, "WGA-06 exactly one production target per planet");
    expect(host.world->containing_planet(10) == 10 && host.world->containing_planet(50) == 10
        && host.world->containing_planet(51) == 10 && host.world->containing_planet(52) == 10
        && host.world->containing_planet(70) == 10 && !host.world->containing_planet(999), "WGA-06/FT-05 direct, fleet, nested and structure containment");
    const auto* provider = host.host->find_target_queries.get();
    const auto* alpha = host.engine.target_of_object(10); const auto* beta = host.engine.target_of_object(20); const auto* gamma = host.engine.target_of_object(30);
    expect(provider->source_target(host.engine, 52) == alpha, "FT-05 source object resolves through containing planet");
    const auto path = provider->execution_path(host.engine, 1, *alpha, *beta);
    expect(path && path->source_valid && path->distance == real(7) && path->threat == real(20) && path->prefix_threat == real(0), "FT-08 pure decoded execution-route facts preserved");
    const auto empty = provider->execution_path(host.engine, 1, *alpha, *gamma);
    expect(empty && empty->empty && empty->source_valid, "FT-15 known complete generation represents absent route as empty");
    expect(provider->execution_path(host.engine, 1, *alpha, *alpha)->same_target, "FT-08 same planet has zero cost");
    expect(host.world->goal_reachable(1, 10, 30, "friendly_only") == true
        && host.world->goal_reachable(1, 10, 20, "friendly_only") == false
        && !host.world->goal_reachable(1, 10, 20, "Any").has_value(), "WGA-18 membership is separate from execution threat/cost");
    expect(host.world->target_matches(1, {"ENEMY"}, beta) && host.world->target_matches(1, {"NEUTRAL"}, gamma), "application owner flags independent of execution path");
    {
        auth::ModuleManifest manifest;
        expect(static_cast<bool>(manifest.add("REACH.LUA", R"(
function ProbeReach(player, origin)
    Unrestricted = FindTarget.Reachable_Target(player, "Pick", "Friendly | Enemy | Neutral", "Friendly_Only")
    Filtered = FindTarget.Reachable_Target(player, "Pick", "Friendly | Enemy | Neutral", "Friendly_Only", 1, origin)
    UnrestrictedObject = Unrestricted.Get_Game_Object()
    FilteredObject = Filtered.Get_Game_Object()
end
)")), "source-separated consumer fixture mounts");
        auto scripts = auth::ScriptScheduler::create({}, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host.host, errors);
        expect(errors.empty() && scripts.create_instance(9, "REACH.LUA"), "production reach binding and provider install");
        auth::ScriptEvent call; call.key = {1, auth::first_simulation_producer, 9, 0}; call.target = 9;
        call.kind = auth::ScriptEvent::Kind::call; call.name = "ProbeReach";
        call.arguments = {foc::detail::handle(foc::handle_player, 1), foc::detail::handle(foc::handle_game_object, 52)};
        expect(static_cast<bool>(scripts.submit_event(call)), "source-separated consumer admitted");
        const auto result = scripts.service(executor);
        expect(result && result.value().diagnostics.empty(), "source-separated production query has no fault");
        const auto unrestricted = scripts.read_global(9, "UnrestrictedObject").value();
        const auto filtered = scripts.read_global(9, "FilteredObject").value();
        expect(unrestricted && filtered && foc::detail::as_handle(*unrestricted, foc::handle_game_object)
            && foc::detail::as_handle(*filtered, foc::handle_game_object)
            && foc::detail::as_handle(*unrestricted, foc::handle_game_object)->id == 30
            && foc::detail::as_handle(*filtered, foc::handle_game_object)->id == 10,
            "FT-05/08 no-source candidate score differs from source-separated reach pick using fixture route records");
    }
    expect(host.query("EnemyCredits") == real(456) && host.query("HumanCredits") == real(456), "self/enemy/human roots use selected manager player facts");
    expect(!host.query("Unqualified"), "U-01 mixed force capability explicitly unavailable");
    host.world->visibility_changed(1, 10, true);
    expect(host.query("Ground") == real(1) && host.query("A") == real(1) && host.query("Count") == real(1), "visible sourced raw getters cache full bindings");
    const auto ground_fraction = real(1) / real(5);
    const auto space_fraction = real(2) / real(5);
    expect(host.query("NormalizedGround") == ground_fraction && host.query("NormalizedSpace") == space_fraction,
        "WGA-62 base getters independently use authored global denominators");
    // WGA-284 getter audit: these base ratios divide directly as doubles.
    expect(host.query("Normalized") == ground_fraction + space_fraction, "base getter double ratios precede equation arithmetic");
    expect(host.query("RawSlots") == real(4) && host.query("OpenStructures") == ai::to_single(real(4) / real(5)), "WGA-62 slots keep raw capacity and structure fraction distinct");
    expect(host.query("Routes", 20) == real(1) && host.query("NoRoutes", 10) == real(0), "WGA-62 route denominators and zero active routes");
    expect(host.world->insert_hint({1, "alpha", "chokepoint", real(3)}) && !host.world->insert_hint({1, "ALPHA", "CHOKEPOINT", real(9)})
        && host.query("Hint") == real(3), "WGA-62 uppercase planet/hint and first duplicate insertion wins");
    host.world->set_story_flag(1, 10, "trigger"); host.world->set_story_flag(1, 0, "trigger");
    expect(host.query("Story") == real(1) && host.world->check_story_flag(1, 10, "TrIgGeR", false), "story equation and direct checks share canonical name identity");
    auto story_state = host.world->state();
    for (auto& flag : story_state.flags) flag.name = "tRiGgEr";
    expect(static_cast<bool>(host.world->restore(story_state)) && host.query("Story") == real(1), "story restore uses the equation name identity");
    story_state.flags.push_back({1, 10, "TRIGGER"});
    expect(!host.world->restore(std::move(story_state)) && host.query("Story") == real(1), "story restore rejects case-equivalent duplicate identities atomically");
    expect(host.world->check_story_flag(1, 10, "trigger", true) && !host.world->check_story_flag(1, 10, "trigger", true)
        && host.world->check_story_flag(1, 0, "trigger", false) && !host.world->check_story_flag(2, 0, "trigger", false), "WGA-62 targeted/untargeted/player stores and preclear value");
    expect(host.query("Story") == real(0), "story equation sees targeted clear without consuming the untargeted flag");
    bind(31, 2, 3);
    expect(host.query("Ground") == real(1) && host.query("Space") == real(3), "WGA-65 expiry equality passes and interval zero bypasses visible lookup");
    bind(32, 2);
    expect(host.query("Ground") == real(2), "WGA-65 strictly later frame expires visible binding");
    host.world->visibility_changed(1, 10, false); // refreshes A and Ground before hiding
    bind(62, 4);
    expect(host.query("Ground") == real(2) && host.query("B") == real(1) && host.query("Count") == real(1), "WGA-65 hidden remembered hit, indefinite interval and never-observed nonzero parameter getter miss");
    expect(host.query("Age") == real(1) && host.query("FractionalAge") == ai::to_single(real(30) / real(62)), "WGA-65 age uses synchronization frame, not parent mode seconds");
    const auto parent_frame = host.engine.frame();
    const ai::GalacticParentContext parent{&host.engine, host.world.get(), 10, true};
    expect(ai::evaluate_in_galactic_context(parent, "Nested", 1) == real(4)
        && host.engine.frame() == parent_frame && host.world->completed_tick() == 62, "WGA-80/81 immediate retained parent query does not pump services");
    expect(!ai::evaluate_in_galactic_context({&host.engine, host.world.get(), 10, false}, "Ground", 1)
        && !ai::evaluate_in_galactic_context(parent, "Ground", 3), "WGA-80 invalid child/manager player refuses numeric result");
    const auto saved = host.world->state();
    auto broken = saved; broken.cache.push_back(broken.cache.front());
    expect(!host.world->restore(std::move(broken)) && host.world->state().cache.size() == saved.cache.size(), "malformed native component refuses atomically");
    QueryHost restored(fixture_setup(), fixture_data());
    expect(static_cast<bool>(restored.world->bind(fixture(62, 4), connectivity, executor, 62, 100)), "restored component rebinds same boundary");
    restored.initialize(executor);
    expect(static_cast<bool>(restored.world->restore(saved)) && restored.query("Ground") == host.query("Ground"), "cached continuation survives typed component restore");
    auto reordered = saved;
    std::reverse(reordered.cache.begin(), reordered.cache.end());
    std::reverse(reordered.visibility.begin(), reordered.visibility.end());
    const auto remembered_ground = restored.query("Ground");
    const auto remembered_count = restored.query("Count");
    expect(static_cast<bool>(restored.world->restore(reordered))
        && restored.query("Ground") == remembered_ground && restored.query("Count") == remembered_count,
        "restore replaces warmed lookup positions independently of archived record order");
    const auto native = host.world->capture().value();
    expect(static_cast<bool>(restored.world->restore(native)) && restored.world->state_hash().value() == host.world->state_hash().value(), "WGA-195 envelope restores complete perception component with equal canonical hash");
    const auto hash = host.world->state_hash().value();
    for (const auto bytes : {0U, 4U, 30U}) {
        auto truncated = native; truncated.components.at("GALACTIC_PERCEPTION").resize(bytes);
        expect(!host.world->restore(truncated) && host.world->state_hash().value() == hash, "truncated component refuses without publishing partial records");
    }
    auto broken_native = native; broken_native.components.at("GALACTIC_PERCEPTION").push_back(0);
    expect(!host.world->restore(broken_native) && host.world->state_hash().value() == hash, "trailing component bytes refuse atomically");
    broken_native = native; broken_native.version = 2;
    expect(!host.world->restore(broken_native), "unsupported envelope version refuses");
    broken_native = native; broken_native.components.clear();
    expect(!host.world->restore(broken_native), "missing mandatory perception component refuses");
    bind(63, 4, 2, true);
    expect(!host.world->containing_planet(50) && !host.world->containing_planet(52), "transit clears old containing planet recursively");
    std::ostringstream trace;
    const auto state = host.world->state();
    for (const auto& cached : state.cache) {
        trace << cached.key.player << ':' << cached.key.planet << ':' << cached.value.repr << ':' << cached.frame << ';';
        for (const auto& token : cached.key.tokens) trace << token << '.';
        for (const auto& binding : cached.key.bindings) {
            trace << binding.parameter;
            if (const auto* text = std::get_if<std::string>(&binding.value)) trace << *text;
            else trace << std::get<ai::Real>(binding.value).repr;
        }
    }
    const auto work = host.world->work();
    expect(work.prepared_planets == 3 && work.prepared_routes == 2 && work.prepared_objects == 8, "deterministic preparation work budget counts each input once");
    return trace.str();
}

std::string target_contrast_contracts(unsigned workers) {
    auto setup = fixture_setup(); setup.uses_fog = false;
    setup.types.clear();
    for (const auto& [id, bits, power] : {std::tuple{200ULL, 2ULL, 5000}, std::tuple{300ULL, 4ULL, 9000},
        std::tuple{500ULL, 2ULL, 160}, std::tuple{600ULL, 2ULL, 16777216}, std::tuple{700ULL, 2ULL, 1}}) {
        foc::AiType type; type.type_id = id; type.category_bits = bits; setup.types.push_back(type);
        ai::GalacticForceType force; force.type = id; force.categories = bits; force.power = real(power); setup.force_types.push_back(force);
    }
    eawr::platform::ThreadWorkerAdapter executor(workers);
    QueryHost host(setup, fixture_data());
    gc::PlayerEconomySetup accounts; accounts.players = {{1, {}, whole(1000), 3, 5}, {2, {}, whole(1000), 3, 5}};
    gc::ProductionSetup production;
    gc::PlanetProduction base; base.planet = 20; base.target_owner = base.starbase_owner = 2; base.starbase_type = 300;
    base.objects = {{70, 200, 2, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    production.planets.push_back(base);
    gc::PoliticalControlSetup control; gc::ControlPlanet surface; surface.planet = 20;
    surface.built_structures = {{500, {}, 0, 1}, {500, {}, 5, 2}}; control.planets.push_back(surface);
    const auto snapshot = [&](std::uint64_t frame) {
        gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = frame;
        return std::make_shared<gc::GalacticSnapshot>(frame, clock, std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}, {3, 3, 0}},
            std::vector<gc::Planet>{{10, {}, 1}, {20, {}, 2}, {30, {}, 3}}, std::vector<gc::Route>{}, std::vector<gc::Fleet>{},
            std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, std::shared_ptr<const gc::MovementSetup>{},
            std::nullopt, std::vector<gc::Seat>{}, accounts, production, control);
    };
    expect(static_cast<bool>(host.world->bind(snapshot(10), {}, executor, 10, 1000)), "contrast facts bind actual defensive structures");
    host.initialize(executor);
    ai::PlayerAi selected; selected.player = 1;
    const auto context = host.world->goal_context(selected, host.engine.target_of_object(20));
    auto state = host.world->state();
    for (const auto channel : {gc::ProductionDomain::ground, gc::ProductionDomain::space}) for (const auto category : {0ULL, 1ULL, 2ULL}) {
        ai::GalacticCacheKey key{context.player, context.enemy, context.human, 20,
            {"VARIABLE_TARGET", "ENEMYFORCE", channel == gc::ProductionDomain::ground ? "GROUNDTOTALUNNORMALIZED" : "SPACETOTALUNNORMALIZED"}, {}};
        if (category) key.bindings.push_back({"PARAMETER_CATEGORY", ai::Real(category)});
        const auto value = channel == gc::ProductionDomain::ground ? (category == 0 ? 10 : category == 1 ? 7 : 3)
            : (category == 0 ? 20 : category == 1 ? 5 : 15);
        state.cache.push_back({key, real(value), real(15), 1});
    }
    expect(static_cast<bool>(host.world->restore(state)), "contrast facts restore remembered category-bound force");
    const std::array<std::uint64_t, 2> categories{2, 1};
    const auto copied = ai::galactic_target_contrast(*host.world, context, categories, real(2));
    expect(copied && copied.value().size() == 6, "contrast facts preserve both domains and supplied category order");
    if (!copied) throw std::runtime_error(copied.error().message);
    const auto& rows = copied.value();
    expect(rows[0].channel == gc::ProductionDomain::ground && rows[0].category == 0 && rows[0].force == real(5330)
        && rows[1].channel == gc::ProductionDomain::space && rows[1].category == 0 && rows[1].force == real(9020),
        "remembered raw force gains direct defence without scaling category-zero totals");
    expect(rows[2].category == 2 && rows[2].force == real(10646) && rows[3].category == 1 && rows[3].force == real(14)
        && rows[4].category == 2 && rows[4].force == real(30) && rows[5].category == 1 && rows[5].force == real(10),
        "classified force applies category masks and multiplier independently in each domain");
    selected.player = 2;
    const auto friendly = host.world->goal_context(selected, host.engine.target_of_object(20));
    expect(host.world->target_enemy_force(friendly, gc::ProductionDomain::ground) == ai::Real{}
        && host.world->target_enemy_force(friendly, gc::ProductionDomain::space) == ai::Real{},
        "allied planet skips direct defensive supplements");
    control.planets.front().built_structures = {{600, {}, 0, 1}, {700, {}, 0, 1}, {700, {}, 0, 1}};
    expect(static_cast<bool>(host.world->bind(snapshot(15), {}, executor, 15, 1000)), "persistent structure precision fixture binds");
    expect(host.world->target_enemy_force(context, gc::ProductionDomain::ground) == real(16782226),
        "persistent structure subtotal single-rounds each addition before joining cached and special power");
    production.planets.front().starbase_type = 0; production.planets.front().objects.clear(); control.planets.front().built_structures.clear();
    expect(static_cast<bool>(host.world->bind(snapshot(20), {}, executor, 20, 1000)), "changed direct defences bind within raw-force cache lifetime");
    expect(host.world->target_enemy_force(context, gc::ProductionDomain::ground) == real(10)
        && host.world->target_enemy_force(context, gc::ProductionDomain::space) == real(20),
        "direct defensive facts refresh while remembered raw force survives");
    state = host.world->state();
    for (auto& row : state.cache) if (row.key.player == 1 && row.key.planet == 20 && row.key.bindings.empty()
        && row.key.tokens.back() == "GROUNDTOTALUNNORMALIZED") row.value = real(-10);
    expect(static_cast<bool>(host.world->restore(state)), "nonpositive total cache fixture restores");
    const auto cleared = ai::galactic_target_contrast(*host.world, context, categories, real(2));
    expect(cleared && cleared.value()[0].force == real(-10) && cleared.value()[2].force == ai::Real{} && cleared.value()[3].force == ai::Real{},
        "nonpositive domain total clears classified demand despite positive remembered category rows");
    expect(!host.world->target_enemy_force({}, gc::ProductionDomain::ground), "missing target context refuses");
    return host.world->state_hash().value();
}

std::string force_contracts(unsigned workers) {
    auto setup = fixture_setup(); setup.uses_fog = false; setup.maximum_fleet_distance = real(100);
    setup.types.clear();
    for (const auto& [id, name, bits, power] : {
        std::tuple{100ULL, "A", 1ULL, 1250}, {200ULL, "B", 2ULL, 5000},
        {300ULL, "BASE", 4ULL, 9000}, {400ULL, "COMPANY", 8ULL, 126}, {500ULL, "PAYLOAD", 16ULL, 160}}) {
        foc::AiType type; type.type_id = id; type.name = name; type.category_bits = bits; setup.types.push_back(type);
        ai::GalacticForceType facts; facts.type = id; facts.categories = bits; facts.power = real(power); setup.force_types.push_back(facts);
    }
    setup.force_types[2].starting_spawns[2] = {{100, 2}}; // selected tech 3 falls back to the first nonempty row
    for (auto& planet : setup.planets) planet.enemy_indigenous.emplace();
    const auto data = equations({{"force.xml", R"(<Equations>
<Raw>Variable_Target.FriendlyForce.SpaceTotalUnnormalized</Raw>
<Hero>Variable_Target.FriendlyForce.SpaceTotalUnnormalized {Parameter_Category=2}</Hero>
<Filtered>Variable_Target.FriendlyForce.SpaceTotalUnnormalized {Parameter_Category=1}</Filtered>
<NonHero>Function_Raw.Evaluate - Function_Hero.Evaluate</NonHero>
<Ground>Variable_Target.FriendlyForce.GroundTotalUnnormalized</Ground>
<Base>Variable_Target.FriendlyForce.StarbaseTotalUnnormalized</Base>
<EnemyBase>Variable_Target.EnemyForce.StarbaseTotalUnnormalized</EnemyBase>
<Normalized>Variable_Target.FriendlyForce.SpaceTotal {Parameter_Category=1}</Normalized>
<Disposition>Variable_Target.FriendlyForce.SpaceTotalNBTD {Parameter_Category=1}</Disposition>
<TargetTotal>Variable_Target.FriendlyForce.SpaceTotalNBTT {Parameter_Category=1}</TargetTotal>
<TargetMax>Variable_Target.FriendlyForce.SpaceTotalNBPTM {Parameter_Category=1}</TargetMax>
<Global>Game.FriendlyForce.SpaceTotalUnnormalized {Parameter_Category=2}</Global>
<Nearby>Variable_Target.EnemyForce.NearbySpaceTotal</Nearby>
<Attenuated>Variable_Target.EnemyForce.NearbySpaceTotal {Parameter_Attenuator=0}</Attenuated>
<Presence>Variable_Target.EnemyForce.HasSpaceForce {Parameter_Category=0}</Presence>
<FilteredPresence>Variable_Target.EnemyForce.HasSpaceForce {Parameter_Faction="Empire"}</FilteredPresence>
<GroundPresence>Variable_Target.FriendlyForce.HasGroundForce</GroundPresence>
<Bits>Variable_Target.FriendlyForce.HasSpaceUnitsBitfield {Parameter_Type="A", Parameter_Type="B", Parameter_Category=2, Parameter_Category=0}</Bits>
<GroundBits>Variable_Target.FriendlyForce.HasGroundUnitsBitfield {Parameter_Type="PAYLOAD", Parameter_Category=16}</GroundBits>
<InvalidBits>Variable_Target.FriendlyForce.HasSpaceUnitsBitfield</InvalidBits>
<Connected>Variable_Target.IsConnectedTo {Parameter_Faction="Rebel"}</Connected>
<SelfConnected>Variable_Target.IsConnectedTo {Parameter_Type="ALPHA"}</SelfConnected>
<Trade>Variable_Target.IsConnectedTo {Parameter_Faction="Pirates", Parameter_Attenuator=1}</Trade>
<Isolated>Variable_Target.ConnectsIsolatedPlanetsByForce</Isolated>
<Largest>Variable_Target.ConnectsLargestIslands</Largest>
<Difficulty>Variable_Self.IsDifficulty {Parameter_Difficulty_Level_Type=1}</Difficulty>
</Equations>)"}});
    eawr::platform::ThreadWorkerAdapter executor(workers);
    QueryHost host(setup, data);
    gc::PlayerEconomySetup accounts; accounts.players = {{1, whole(100), whole(1000), 3, 5}, {2, whole(100), whole(1000), 3, 5}};
    gc::ProductionSetup production; gc::PlanetProduction base; base.planet = 10; base.starbase_type = 300; base.starbase_owner = 1; production.planets.push_back(base);
    gc::Fleet first; first.id = 50; first.owner = 1; first.orbit = 10; first.members = {{51, 100}, {52, 200}};
    gc::Fleet second; second.id = 60; second.owner = 2; second.orbit = 20; second.members = {{61, 100}};
    auto movement = std::make_shared<gc::MovementSetup>(); movement->contents = {{70, {{71, 500}}}};
    gc::PoliticalControlSetup control; gc::ControlPlanet surface; surface.planet = 10; surface.landed = {{70, 400, 1}}; control.planets.push_back(surface);
    gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = 1;
    const auto snapshot = std::make_shared<gc::GalacticSnapshot>(1, clock, std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}, {3, 3, 0}},
        std::vector<gc::Planet>{{10, {}, 1}, {20, {whole(100), {}, {}}, 2}, {30, {whole(200), {}, {}}, 3}},
        std::vector<gc::Route>{{40, 10, 30, whole(1)}}, std::vector<gc::Fleet>{first, second}, std::vector<gc::GalacticEvent>{},
        std::nullopt, std::nullopt, movement, std::nullopt, std::vector<gc::Seat>{}, accounts, production, control);
    ai::GalacticConnectivity connectivity; connectivity.island_paths = {{1, 10, true, false}};
    expect(static_cast<bool>(host.world->bind(snapshot, connectivity, executor, 1, 100)), "force preparation binds completed mixed-domain fixture"); host.initialize(executor);
    expect(host.query("Raw") == real(8750) && host.query("Hero") == real(5000) && host.query("NonHero") == real(3750), "WGA-182/183 raw metric includes starting base garrisons and preserves explicit hero subtraction");
    expect(host.query("Ground") == real(126) && host.query("Base") == real(9000) && host.query("EnemyBase") == ai::Real{}, "WGA-184/190 landed company original metric and base metric have distinct domains");
    expect(host.query("Normalized") == real(3750) / real(10000) && host.query("Disposition") == real(3750) / real(8750)
        && host.query("TargetTotal") == real(3750) / real(8750) && host.query("TargetMax") == real(3750) / real(8750), "WGA-188/189 category-filtered numerator keeps unfiltered game/target normalization denominator");
    expect(host.query("Global") == real(8750), "WGA-189 game aggregation drops incoming category bindings");
    expect(host.query("Nearby") == real(1250) / real(10000) && host.query("Attenuated") == ai::Real{}, "WGA-192 nearby raw force weighting and global denominator");
    expect(host.query("Presence", 20) == real(1) && host.query("Presence") == ai::Real{} && host.query("FilteredPresence", 20) == ai::Real{}
        && host.query("GroundPresence") == real(1), "WGA-185/186 force presence ignores power/category but honors disposition and faction");
    expect(host.query("Bits") == real(15) && host.query("GroundBits") == real(3) && !host.query("InvalidBits"), "WGA-187 type slots precede category slots and ground bitfields use live payload");
    expect(host.query("Connected") == real(1) && host.query("SelfConnected") == ai::Real{} && host.query("Trade") == real(1), "WGA-192/193 connectivity excludes self and includes distant direct trade routes");
    expect(host.query("Isolated") == real(1) && host.query("Largest") == ai::Real{} && !host.query("Largest", 20), "WGA-194 known island flags stay distinct from missing producer facts");
    expect(host.query("Difficulty") == real(1), "stock difficulty parameter name resolves selected Normal context");
    expect(host.world->work().force_terms == 6 && host.world->work().neighbourhood_pairs == 9, "partitioned force/neighbourhood work is deterministically bounded");
    expect(static_cast<bool>(host.world->bind(snapshot, connectivity, executor, 1, 100)) && host.world->work().neighbourhood_pairs == 0,
        "unchanged topology reuses neighbourhoods without another pair scan");
    const auto saved = host.world->capture().value();
    QueryHost restored(setup, data);
    expect(static_cast<bool>(restored.world->bind(snapshot, connectivity, executor, 1, 100)), "force contributor restore first rebinds immutable copied world"); restored.initialize(executor);
    expect(static_cast<bool>(restored.world->restore(saved)) && restored.query("Normalized") == host.query("Normalized")
        && restored.world->state_hash().value() == host.world->state_hash().value(), "force cache continuation and canonical component hash survive restore");
    auto malformed = host.world->state(); malformed.cache.front().key.enemy = 99;
    expect(!host.world->restore(malformed), "unresolved selected enemy identity refuses atomically");
    malformed = host.world->state(); malformed.visibility.front().last_observed = 2;
    expect(!host.world->restore(malformed), "observation newer than bound synchronizer refuses");
    auto fog_setup = setup; fog_setup.uses_fog = true;
    fog_setup.cache_intervals.emplace("CONNECTSISOLATEDPLANETSBYFORCE", ai::Real{});
    QueryHost visible_flags(fog_setup, data);
    expect(static_cast<bool>(visible_flags.world->bind(snapshot, connectivity, executor, 1, 100)), "island cache control binds"); visible_flags.initialize(executor);
    visible_flags.world->visibility_changed(1, 10, true);
    expect(visible_flags.query("Isolated") == real(1), "visible island flag reads current copied producer fact");
    visible_flags.world->visibility_changed(1, 10, false);
    connectivity.island_paths.front().least_force = false;
    expect(static_cast<bool>(visible_flags.world->bind(snapshot, connectivity, executor, 2, 100))
        && visible_flags.query("Isolated") == ai::Real{}, "WGA-191 zero-interval island reads do not retain a hidden fog-cache hit");
    return host.world->state_hash().value() + visible_flags.world->state_hash().value();
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
    const auto opened = filesystem.open(path); if (!opened) throw std::runtime_error(eawr::core::format_diagnostic(opened.error()));
    return std::string(reinterpret_cast<const char*>(opened.value().data()), opened.value().size());
}

using Categories = std::map<std::string, std::uint64_t, std::less<>>;
Categories stock_categories(const eawr::vfs::Vfs& filesystem) {
    const auto document = ai::parse_xml("categories", read(filesystem, "data/xml/enum/gameobjectcategorytype.xml")).value();
    Categories out;
    for (const auto& child : document.children) {
        auto text = ai::trimmed(child.text); const bool hex = text.starts_with("0x"); if (hex) text.erase(0, 2);
        std::uint64_t value{}; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::runtime_error("invalid registered stock category");
        out.emplace(ai::upper_case(child.name), value);
    }
    return out;
}
ai::GalacticPerceptionSetup stock_setup(const cp::CampaignSetupPlan& planned,
    const eawr::data::galactic::GalacticData& data, const Categories& categories) {
    ai::GalacticPerceptionSetup setup; setup.seed = 42;
    const auto* distance = data.constant("MaximumFleetMovementDistance");
    if (!distance || !distance->number) throw std::runtime_error("authored fleet neighbourhood radius missing");
    setup.maximum_fleet_distance = eawr::script::numeric::from_fixed(*distance->number);
    for (const auto& binding : planned.player_bindings()) {
        const auto* faction = data.faction(binding.faction);
        const bool human = planned.local_observer() == binding.player.player_id;
        ai::GalacticPlayerFacts facts{binding.player.player_id, binding.player.player_id, binding.faction, "Normal",
            faction->is_neutral.value_or(false), !human, human, 0};
        for (const auto& home : planned.homes()) if (home.player == facts.player && home.planet) facts.home = *home.planet;
        setup.players.push_back(std::move(facts));
    }
    for (const auto& location : planned.locations()) if (location.planet)
        setup.planets.push_back({location.planet->id, location.original_type, ai::crc32(ai::upper_case(location.original_type)), {}, {}, {}, {}, {}});
    const auto identity = [&](std::string_view name) {
        for (const auto& binding : planned.type_bindings()) if (ai::upper_case(binding.original_type) == ai::upper_case(name)) return binding.id;
        throw std::runtime_error("stock garrison type lacks original identity");
    };
    for (const auto& binding : planned.type_bindings()) {
        const auto* type = planned.original_type(binding.id); if (!type) continue;
        foc::AiType facts; facts.type_id = binding.id; facts.name = ai::upper_case(type->name);
        for (const auto& name : type->category_mask) facts.category_bits |= categories.at(ai::upper_case(name));
        ai::GalacticForceType force; force.type = facts.type_id; force.categories = facts.category_bits;
        force.transport = !type->company_units.empty();
        force.power = type->ai_combat_power ? eawr::script::numeric::from_fixed(*type->ai_combat_power) : ai::Real{};
        for (std::size_t tech = 0; tech < force.starting_spawns.size(); ++tech)
            for (const auto& spawn : type->starting_spawned_units[tech]) force.starting_spawns[tech].push_back({identity(spawn.type), spawn.count});
        setup.types.push_back(std::move(facts)); setup.force_types.push_back(std::move(force));
    }
    return setup;
}

void installed() {
    const auto root = game_root(); if (!root) { std::cout << "SKIPPED: stock perception requires EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
    const auto* source = data.campaign(campaign_name); if (!source) throw std::runtime_error("stock campaign family absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.local_observer = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto planned = cp::plan_live_campaign_setup(filesystem, data, types, campaign_name, options, "Normal_Default").value();
    const auto services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, planned, "Normal_Default").value();
    auto campaign = cp::CampaignRoot::create(planned, services).value();
    const auto categories = stock_categories(filesystem);
    const auto setup = stock_setup(planned, data, categories);
    const auto target_content = cp::prepare_ai_selection_content(filesystem, catalog.catalog, data, types, planned).value();
    for (const auto name : {"TARTAN_PATROL_CRUISER", "ACCLAMATOR_ASSAULT_SHIP"}) {
        const auto ship = std::find_if(target_content->ai.types.begin(), target_content->ai.types.end(),
            [&](const auto& type) { return type.name == name; });
        expect(ship != target_content->ai.types.end() && ship->unit_ai && ship->targeting,
            "stock campaign ship metadata preserves decoded tracking and targeting behaviors");
    }
    const auto target_setup = cp::prepare_ai_perception_content(filesystem, data, planned, *target_content, "Normal").value();
    const auto converter = [&](std::string_view name, std::string_view text) -> std::optional<ai::Real> {
        if (ai::upper_case(name) == "GAMEOBJECTCATEGORYTYPE") {
            const auto found = categories.find(ai::upper_case(text));
            if (found != categories.end()) return real(static_cast<std::int32_t>(found->second));
        }
        if (ai::upper_case(name) == "DIFFICULTYLEVELTYPE") {
            if (ai::upper_case(text) == "EASY") { return real(0); }
            if (ai::upper_case(text) == "NORMAL") { return real(1); }
            if (ai::upper_case(text) == "HARD") { return real(2); }
        }
        return {};
    };
    const std::vector<std::pair<std::string, std::string>> stock_equations{
        {"data/xml/ai/perceptualequations/basicgalacticequations.xml", read(filesystem, "data/xml/ai/perceptualequations/basicgalacticequations.xml")},
        {"data/xml/ai/perceptualequations/informationgalacticequations.xml", read(filesystem, "data/xml/ai/perceptualequations/informationgalacticequations.xml")},
        {"data/xml/ai/perceptualequations/offensivegalaticequations.xml", read(filesystem, "data/xml/ai/perceptualequations/offensivegalaticequations.xml")},
        {"data/xml/ai/perceptualequations/defensivegalacticequations.xml", read(filesystem, "data/xml/ai/perceptualequations/defensivegalacticequations.xml")}};
    // Explicit four-planet placement control for WGA-142. The stock definitions,
    // types, producers and live campaign initialization remain production code.
    auto control_data = data;
    auto control_campaign = *source; control_campaign.name = "EAWR_PERCEPTION_CONTROL";
    control_campaign.is_story_campaign.reset(); // Exercise the production constructor default.
    control_campaign.starting_active_player = "Underworld";
    control_campaign.locations = control_campaign.planets = {"Kamino", "Coruscant", "Ryloth", "Shola"};
    control_campaign.trade_routes.clear(); control_campaign.starting_forces.clear();
    control_campaign.home_locations = {{"Empire", "Kamino"}, {"Rebel", "Ryloth"}, {"Underworld", "Shola"}};
    control_campaign.starting_forces = {{"Empire", "Kamino", "Empire_Star_Base_1"}, {"Empire", "Coruscant", "Empire_Star_Base_1"},
        {"Rebel", "Ryloth", "Rebel_Star_Base_1"}, {"Underworld", "Shola", "Underworld_Star_Base_1"}};
    control_campaign.special_case_production.clear(); control_campaign.markup_filenames.clear();
    control_campaign.corruption_level_overrides.clear(); control_data.campaigns.push_back(control_campaign);
    for (std::size_t i = 0; i < control_campaign.planets.size(); ++i) for (auto& planet : control_data.planets)
        if (ai::upper_case(planet.name) == ai::upper_case(control_campaign.planets[i])) planet.galactic_position = eawr::data::galactic::Position{whole(static_cast<std::int64_t>(i) * 20), {}, {}};
    auto control_options = options;
    for (const auto& binding : control_options.players) if (binding.faction == "Underworld") control_options.local_observer = binding.player.player_id;
    for (auto& seat : control_options.seats) {
        const bool human = seat.player == control_options.local_observer;
        seat.kind = human ? gc::SeatKind::human : gc::SeatKind::ai;
        seat.human_side_id = human ? seat.player : 0;
    }
    const auto control_plan = cp::plan_live_campaign_setup(filesystem, control_data, types, control_campaign.name, control_options, "Normal_Default").value();
    const auto control_selection = cp::prepare_ai_selection_content(filesystem, catalog.catalog, control_data, types, control_plan).value();
    const auto default_story = cp::prepare_ai_perception_content(filesystem, control_data, control_plan, *control_selection, "Normal").value();
    expect(default_story->is_story_campaign == false,
        "WGS-10/WGA-423 production preparation turns an absent authored story tag into available false");
    const auto control_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, control_data, types, control_plan, "Normal_Default").value();
    auto control_root = cp::CampaignRoot::create(control_plan, control_services).value();
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        QueryHost host(setup, equations(stock_equations, converter));
        auto& galaxy = campaign.game()->galaxy();
        expect(static_cast<bool>(host.world->bind(galaxy.snapshot(), {}, executor, 0, galaxy.next_entity_id())), "installed completed live campaign binds production world");
        host.initialize(executor);
        expect(host.world->work().prepared_planets == galaxy.snapshot()->planets().size(), "installed canonical targets match actual campaign planets");
        const auto copied = host.world->copy_input();
        expect(copied && !copied->producers.empty() && !copied->credits.empty(), "installed copied producer/credit facts come from real queues/admission");
        std::ostringstream trace;
        QueryHost target_facts(*target_setup, equations(stock_equations, converter));
        expect(static_cast<bool>(target_facts.world->bind(galaxy.snapshot(), {}, executor, 1, galaxy.next_entity_id())),
            "installed target facts use the production original-content/world provider");
        target_facts.initialize(executor);
        std::vector<std::uint64_t> contrast_categories;
        for (const auto& [name, value] : categories) { (void)name; if (value) contrast_categories.push_back(value); }
        std::size_t target_reads{}, expanded_garrisons{};
        for (const auto& owner : target_setup->players) if (!owner.neutral) {
            ai::PlayerAi selected; selected.player = owner.player;
            for (const auto& planet : galaxy.snapshot()->planets()) {
                const auto context = target_facts.world->goal_context(selected, target_facts.engine.target_of_object(planet.id));
                const auto facts = ai::galactic_target_contrast(*target_facts.world, context, contrast_categories, real(1));
                if (!facts) throw std::runtime_error(facts.error().message);
                ++target_reads;
                trace << owner.player << ':' << planet.id << ':';
                for (const auto& row : facts.value()) trace << row.category << ':' << row.force.repr << ';';
            }
            const auto account = std::find_if(galaxy.snapshot()->player_economy()->players.begin(), galaxy.snapshot()->player_economy()->players.end(),
                [&](const auto& row) { return row.player == owner.player; });
            if (account == galaxy.snapshot()->player_economy()->players.end()) throw std::runtime_error("installed target-facts account absent");
            for (const auto& planet : galaxy.snapshot()->production()->planets) if (planet.starbase_owner == owner.player && planet.starbase_type) {
                const auto garrisons = ai::galactic_proposal_garrisons(*target_content, planet.starbase_type,
                    static_cast<std::uint32_t>(account->current_tech), planet.planet);
                if (!garrisons) throw std::runtime_error(garrisons.error().message);
                expanded_garrisons += garrisons.value().size();
                for (const auto& row : garrisons.value()) trace << row.type << ':' << row.power.repr << ':' << row.terrain_effectiveness.repr << ';';
            }
        }
        expect(target_reads > 0 && expanded_garrisons > 0, "installed stock has actual target-force and authored base-garrison witnesses");
        trace << target_facts.world->state_hash().value();
        for (const auto& player : setup.players) if (!player.neutral) {
            expect(host.query("Is_Empire", player.home, player.player) == real(player.faction == "Empire" ? 1 : 0), "installed stock faction equation selects actual player root");
            for (const auto& planet : galaxy.snapshot()->planets()) {
                const auto score = host.query("Is_Home_Planet", planet.id, player.player);
                if (player.home) expect(score == real(planet.id == player.home ? 1 : 0), "installed stock home equation matches actual campaign home/planet identity");
                const auto slice = host.query("Is_Good_Slice_Target", planet.id, player.player);
                const auto other = std::find_if(setup.players.begin(), setup.players.end(), [&](const auto& p) { return p.player == planet.owner; });
                if (other != setup.players.end()) expect(slice == real(other->faction != "Pirates" && other->faction != "Neutral" ? 1 : 0), "installed repeated faction bindings retain complete parameter list");
                trace << player.player << ':' << planet.id << ':' << (score ? score->repr : 0) << ':' << (slice ? slice->repr : 0) << ';';
            }
        }
        // Real stock Lua closure and production bindings; this initial piece runs
        // definitions and query consumers, not the E-owned freestore action service.
        auth::ModuleManifest manifest;
        for (const auto path : {"data/scripts/freestore/galacticfreestore.lua", "data/scripts/freestore/galacticherofreestore.lua",
            "data/scripts/library/pgcommands.lua", "data/scripts/library/pgbasedefinitions.lua", "data/scripts/library/pgbase.lua", "data/scripts/library/pgdebug.lua"})
            expect(static_cast<bool>(manifest.add(path, read(filesystem, path))), "installed unmodified Lua module mounts");
        expect(static_cast<bool>(manifest.add("EAWR/QUERY_PROOF.LUA", R"(require("GalacticFreeStore")
function QueryProof(player, target)
    StockIsEmpire = EvaluatePerception("Is_Empire", player, target)
    StockHome = EvaluatePerception("Is_Home_Planet", player, target)
end
)")), "query consumer harness mounts");
        auth::SessionConfig config; config.seed = 42; config.galactic_clock = true;
        config.script_directories = {"data/scripts/library/", "data/scripts/freestore/"};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, host.host, errors);
        expect(errors.empty(), "production Lua query bindings install");
        const auto empire = std::find_if(setup.players.begin(), setup.players.end(), [](const auto& p) { return p.faction == "Empire"; });
        if (empire == setup.players.end() || !empire->home) throw std::runtime_error("stock Empire home absent");
        const auto instance = scripts.create_instance(7, "EAWR/QUERY_PROOF.LUA");
        if (!instance) throw std::runtime_error(eawr::core::format_diagnostic(instance.error()));
        auth::ScriptEvent definitions; definitions.key = {1, auth::first_simulation_producer, 7, 0}; definitions.target = 7;
        definitions.kind = auth::ScriptEvent::Kind::call; definitions.name = "Base_Definitions";
        expect(static_cast<bool>(scripts.submit_event(definitions)), "stock freestore definitions admitted");
        auth::ScriptEvent query = definitions; query.key.sequence = 1; query.name = "QueryProof";
        query.arguments = {foc::detail::handle(foc::handle_player, empire->player), foc::detail::handle(ai::handle_ai_target, host.engine.target_of_object(empire->home)->id)};
        expect(static_cast<bool>(scripts.submit_event(query)), "stock equation Lua query admitted");
        const auto result = scripts.service(executor);
        if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
        if (!result.value().diagnostics.empty()) for (const auto& diagnostic : result.value().diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        expect(result.value().diagnostics.empty(), "stock definitions/query consumers have no Lua faults or stand-in values");
        for (const auto name : {"StockIsEmpire", "StockHome"}) {
            const auto value = scripts.read_global(7, name).value();
            expect(value && std::get_if<ai::Real>(&value->data) && std::get<ai::Real>(value->data) == real(1), "stock equation evaluates through production Lua host");
        }
        trace << scripts.state_digest();
        const auto controlled_setup = stock_setup(control_plan, control_data, categories);
        QueryHost defence(controlled_setup, equations(stock_equations, converter));
        auto& controlled = control_root.game()->galaxy();
        expect(static_cast<bool>(defence.world->bind(controlled.snapshot(), {}, executor, 1, controlled.next_entity_id())), "installed space defence binds real initialized campaign bases and garrisons"); defence.initialize(executor);
        const auto controlled_empire = std::find_if(controlled_setup.players.begin(), controlled_setup.players.end(), [](const auto& p) { return p.faction == "Empire"; });
        if (controlled_empire == controlled_setup.players.end()) throw std::runtime_error("controlled Empire absent");
        expect(controlled_empire->ai && !controlled_empire->human, "space-defence control uses an actual Normal Empire AI seat");
        std::size_t defence_targets{};
        for (const auto& location : control_plan.locations()) if (location.planet && (ai::upper_case(location.original_type) == "KAMINO" || ai::upper_case(location.original_type) == "CORUSCANT")) {
            ++defence_targets;
            const auto id = location.planet->id;
            expect(defence.query("Needs_Magic_Space_Defense", id, controlled_empire->player) == real(1)
                && defence.query("Needs_Magic_Space_Defense_Easy", id, controlled_empire->player) == real(0)
                && defence.query("Needs_Magic_Space_Defense_Hard", id, controlled_empire->player) == real(0), "WGA-142 stock Normal Kamino/Coruscant space-defence condition evaluates real world force/connectivity/difficulty");
            const auto raw = defence.query("Friendly_Space_Unit_Raw_Total", id, controlled_empire->player);
            expect(raw && *raw > ai::Real{} && *raw < real(3000), "stock base garrisons contribute positive nonhero power below authored Normal threshold");
            trace << id << ':' << (raw ? raw->repr : 0) << ';';
        }
        expect(defence_targets == 2, "both named stock space-defence targets are present in the live placement control");
        const auto native = defence.world->capture().value();
        QueryHost resumed(controlled_setup, equations(stock_equations, converter));
        expect(static_cast<bool>(resumed.world->bind(controlled.snapshot(), {}, executor, 1, controlled.next_entity_id())), "installed contributor load rebinds actual world first"); resumed.initialize(executor);
        expect(static_cast<bool>(resumed.world->restore(native)) && resumed.world->state_hash().value() == defence.world->state_hash().value(), "installed perception component roundtrips actual stock query state");
        trace << defence.world->state_hash().value();
        if (workers == 1) reference = trace.str(); else expect(trace.str() == reference, "installed query/Lua state equals on 1/2/4/8 workers");
    }
    std::cout << "Stock perception component: real campaign/queues/forces, stock equations and freestore definitions; live route/action/lifecycle qualification remains open.\n";
}
} // namespace

int main(int argc, char**) {
    try {
        if (argc > 1) installed();
        else {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto trace = contracts(workers) + force_contracts(workers)
                    + story_campaign_contracts(workers) + difficulty_seat_contracts(workers)
                    + producer_preparation_contracts(workers);
                const auto target_trace = target_contrast_contracts(workers);
                if (workers == 1) reference = trace + target_trace; else expect(trace + target_trace == reference, "perception cache/context/route contracts equal on 1/2/4/8 workers");
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

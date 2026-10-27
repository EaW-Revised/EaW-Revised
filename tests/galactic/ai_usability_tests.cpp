#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_usability.hpp"
#include "galactic_ai_connecting_heap.hpp"
#include "galactic_ai_execution_state.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/sim/galactic/encoding.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace ai = eawr::script::foc::ai;
namespace ga = eawr::script::foc::galactic_ai;
using galactic_test::expect;
using ai::real;
using eawr::testing::result_diagnostic::take;
void done(eawr::core::Result<void> result) {
    eawr::testing::must(std::move(result));
}
eawr::sim::math::Fixed f(std::int64_t n) { return take(eawr::sim::math::Fixed::from_integer(n)); }
gc::GalacticSetup fixture() {
    gc::GalacticSetup s; s.seed = 71; s.debug_commands = true;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}, {{2}, 2, gc::SeatKind::human, 1, true, 2}, {{3}, 1, gc::SeatKind::script, 2, true, 0}};
    s.rules.maximum_fleet_movement_distance = f(10); s.rules.fiscal_cycle_frames = 100;
    for (gc::PlanetId id = 1; id <= 5; ++id) s.planets.push_back({id, {f(static_cast<std::int64_t>(id - 1) * 10), {}, {}}, id % 2 ? 1U : 2U});
    s.fleets = {{50, 2, 2, {{51, 100}}}};
    s.control.emplace(); auto& control = *s.control; control.change_seconds = f(7); control.max_ground_forces = 2;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    control.types = {{100, 0, false, false}};
    for (const auto& a : control.players) for (const auto& b : control.players) control.relations.push_back({a.player, b.player, a.player == b.player});
    s.observer.emplace(); auto& observer = *s.observer; observer.human_observers = {2}; observer.solo_reference_human = 2;
    observer.reveal_delay_seconds = f(1000); observer.automatic_reveal_range = f(0);
    observer.players = {{1, true, false, 0, 100, {}}, {2, false, false, 0, 100, {}}, {3, false, false, 0, 100, {}}};
    observer.types = {{100, false, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; control.planets.push_back(row);
        gc::ObserverPlanet facts; facts.planet = planet.id; facts.allegiance = facts.target_owner = planet.owner;
        facts.current_owner = facts.current_controller = planet.owner; observer.planets.push_back(facts);
    }
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        observer.records.push_back({player.player_id, planet.id, true, 0, 0, gc::bit(gc::Intelligence::ownership), {}});
    return s;
}
eawr::core::Result<gc::GalacticSession> world_session(gc::GalacticSetup s) {
    // This fixture's actors are explicitly ordinary type 100. Supply their
    // owning saved records together with the required census services.
    s.player_economy.emplace(); s.fiscal.emplace(); s.production.emplace(); s.census.emplace(); s.fleet_control.emplace();
    s.production->maximum_human_queue = 1;
    s.fleet_control->types = {{100}}; s.census->types = {{100, {}}};
    for (const auto& player : s.players) {
        const bool human = std::any_of(s.seats.begin(), s.seats.end(), [&](const auto& seat) {
            return seat.player == player.player_id && seat.kind == gc::SeatKind::human;
        });
        gc::GalacticPlayerEconomy economy; economy.player = player.player_id;
        s.player_economy->players.push_back(economy);
        s.fiscal->players.push_back({player.player_id, false, f(1)});
        s.production->players.push_back({player.player_id, human, !human, human, {}, {}, {}, {}, {}});
        s.census->players.push_back({player.player_id, 0, false, {}});
    }
    const auto actor = [&](eawr::sim::EntityId id, gc::PlayerId owner, gc::PlanetId planet, eawr::sim::EntityId parent, gc::ActorCombatRole role) {
        gc::CensusObject row{id, 100, owner, planet, parent, false, false, 100};
        row.combat_role = role; row.abilities.present = role == gc::ActorCombatRole::ordinary; row.garrison = false;
        s.census->objects.push_back(row);
    };
    for (const auto& planet : s.planets) {
        s.fiscal->planets.push_back({planet.id, {}});
        gc::PlanetProduction row; row.planet = planet.id; s.production->planets.push_back(row);
        actor(planet.id, planet.owner, planet.id, 0, gc::ActorCombatRole::ordinary);
    }
    for (const auto& fleet : s.fleets) {
        actor(fleet.id, fleet.owner, fleet.orbit, fleet.orbit, gc::ActorCombatRole::fleet);
        for (const auto& member : fleet.members) actor(member.id, fleet.owner, fleet.orbit, fleet.id, gc::ActorCombatRole::ordinary);
    }
    if (s.control) for (const auto& row : s.control->planets) for (const auto& force : row.landed)
        actor(force.id, force.owner, row.planet, row.planet, gc::ActorCombatRole::ordinary);
    std::sort(s.census->objects.begin(), s.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return gc::GalacticSession::create(s);
}
ai::GalacticPerceptionSetup perception(bool fog) {
    ai::GalacticPerceptionSetup out; out.seed = 71; out.uses_fog = fog;
    out.players = {{1, 1, "EMPIRE", "NORMAL", false, true, false, 1},
        {2, 2, "REBEL", "NORMAL", false, false, true, 2}, {3, 3, "PIRATES", "NORMAL", true, false, false, 0}};
    for (gc::PlanetId id = 1; id <= 5; ++id) out.planets.push_back({id, "P" + std::to_string(id), 100 + id, {}, {}, {}, std::vector<std::pair<std::uint64_t, ai::Real>>{}, {}});
    ai::GalacticForceType force; force.type = 100; force.categories = 1; force.power = real(42); force.object_traits = ai::GalacticObjectTraits{}; out.force_types.push_back(force);
    out.maximum_fleet_distance = real(10); return out;
}
std::shared_ptr<const cp::GalacticAiUsabilityContent> content(const ai::GalacticPerceptionSetup& setup) {
    auto out = std::make_shared<cp::GalacticAiUsabilityContent>(); out->players = setup.players;
    out->planet_order = {1, 2, 3, 4, 5}; out->uses_fog = setup.uses_fog; out->logical_fps = setup.logical_fps; return out;
}
struct Host {
    std::shared_ptr<eawr::script::foc::detail::Host> host = std::make_shared<eawr::script::foc::detail::Host>();
    ai::GalacticWorld world;
    ai::Engine engine;
    explicit Host(const ai::GalacticPerceptionSetup& setup, ai::AiData data = {}) : world(setup), engine(host, std::move(data), {}) {}
    std::optional<ai::Real> query(std::string_view body, gc::PlayerId player = 1, gc::PlanetId planet = 2, std::optional<gc::PlayerId> enemy = {}, bool read_only = false) {
        const auto equation = take(ai::parse_equation("CONTRACT", body, {}));
        ai::Target target; target.object = planet; target.name = "OBJECT_" + std::to_string(planet);
        ai::Context context; context.player = player; context.target = &target; context.enemy = enemy;
        ai::AiRandom random; return read_only ? world.perception_read_only(engine, equation, context, random) : world.perception(engine, equation, context, random);
    }
};
// The former B reducer consumes a canonical deep copy. Keep it as the independent
// reference for the locked projection, including its legacy visibility fallback.
ai::Real copied_cache_deadline(const ai::GalacticPerceptionState& state, gc::PlayerId owner,
    std::uint32_t fps, bool fog) {
    auto through = ai::Real(std::numeric_limits<std::int64_t>::max());
    for (const auto& cached : state.cache) {
        if (cached.key.player != owner || cached.key.tokens.size() != 3
            || cached.key.tokens[1] != "ENEMYFORCE"
            || (cached.key.tokens[2] != "HASSPACEFORCE" && cached.key.tokens[2] != "SPACETOTALUNNORMALIZED"
                && cached.key.tokens[2] != "GROUNDTOTALUNNORMALIZED")) continue;
        if (fog) {
            const auto seen = std::find_if(state.visibility.begin(), state.visibility.end(), [&](const auto& row) {
                return row.player == owner && row.planet == cached.key.planet;
            });
            const auto mask = ai::galactic_visibility_mask(cached.key.tokens);
            const auto qualified = std::find_if(state.visibility.begin(), state.visibility.end(), [&](const auto& row) {
                return row.player == owner && row.planet == cached.key.planet && row.mask == mask;
            });
            if (qualified != state.visibility.end() ? !qualified->visible : seen == state.visibility.end() || !seen->visible) continue;
        }
        if (cached.interval == ai::Real(-1) || cached.interval == ai::Real{}) continue;
        through = std::min(through, cached.frame == 0 ? ai::Real{} : ai::Real(cached.frame) + cached.interval * ai::Real(fps));
    }
    return through;
}
std::string deadline_projection(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(world_session(fixture()));
    std::string result;
    for (const auto interval : {real(-1), real(0), real(1) / real(2), real(7)}) {
        auto setup = perception(fog);
        for (const auto* name : {"ENEMYFORCE.HASSPACEFORCE", "ENEMYFORCE.SPACETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDTOTALUNNORMALIZED"})
            setup.cache_intervals[name] = interval;
        Host host(setup);
        done(host.world.bind(session.snapshot(), {}, executor, 500, session.next_entity_id()));
        ai::GalacticPerceptionState state;
        const auto append = [&](gc::PlayerId owner, gc::PlanetId planet, const char* token, std::uint64_t frame) {
            ai::GalacticCacheKey key; key.player = owner; key.planet = planet;
            key.tokens = {"VARIABLE_TARGET", "ENEMYFORCE", token};
            state.cache.push_back({std::move(key), real(42), interval, frame});
        };
        append(1, 2, "HASSPACEFORCE", 11);
        append(1, 2, "SPACETOTALUNNORMALIZED", 9);
        append(1, 3, "GROUNDTOTALUNNORMALIZED", 13);
        append(1, 3, "HASSPACEFORCE", 0);
        append(1, 4, "SPACETOTALUNNORMALIZED", 15);
        append(1, 4, "GROUNDTOTALUNNORMALIZED", 0);
        append(1, 5, "SPACETOTALUNNORMALIZED", 0);
        append(2, 2, "HASSPACEFORCE", 0);
        auto binding = state.cache[1]; binding.key.enemy = 2; binding.key.bindings = {{"PARAMETER_CATEGORY", real(3)}};
        binding.frame = 19; state.cache.push_back(std::move(binding));
        ai::GalacticCacheKey ownership; ownership.player = 1; ownership.planet = 1;
        ownership.tokens = {"VARIABLE_TARGET", "TARGETPLAYERID"};
        state.cache.push_back({std::move(ownership), real(1), real(0), 0});
        state.visibility = {
            {1, 2, true, 0, gc::bit(gc::Intelligence::fleet_count)},
            {1, 2, false, 0, 0},
            {1, 2, true, 0, gc::bit(gc::Intelligence::fleet_presence)},
            {1, 3, true, 0, gc::bit(gc::Intelligence::ground_count)},
            {1, 3, false, 0, gc::bit(gc::Intelligence::ownership)},
            {1, 4, false, 0, gc::bit(gc::Intelligence::ground_count)},
            {1, 4, true, 0, gc::bit(gc::Intelligence::ownership)}};
        for (unsigned permutation = 0; permutation < 3; ++permutation) {
            if (permutation == 1) { std::reverse(state.cache.begin(), state.cache.end()); std::reverse(state.visibility.begin(), state.visibility.end()); }
            if (permutation == 2) state.visibility.clear();
            done(host.world.restore(state));
            const auto before = take(host.world.capture()); const auto revision = host.world.state_revision();
            for (const auto owner : {1U, 2U, 3U}) for (const auto fps : {1U, 30U, 59U}) {
                const auto expected = copied_cache_deadline(host.world.state(), owner, fps, fog);
                expect(host.world.force_cache_deadline(owner, fps, fog) == expected,
                    "locked force deadline equals canonical copy with full visibility precedence");
                expect(host.world.force_cache_deadline(owner, fps, fog) == expected,
                    "warmed force deadline index preserves expiry and restore order");
            }
            const auto deadline = host.world.force_cache_deadline(1, 30, fog);
            const auto expected = interval == real(-1) || interval == real(0) || (fog && permutation == 2)
                ? ai::Real(std::numeric_limits<std::int64_t>::max())
                : fog ? real(9) + interval * real(30) : real(0);
            expect(deadline == expected, "force deadline excludes hidden, unrelated and immortal rows and retains frame zero");
            expect(take(host.world.capture()).components == before.components && host.world.state_revision() == revision,
                "deadline projection changes neither observations nor native bytes/revision");
            result += std::to_string(deadline.repr) + take(host.world.state_hash());
        }
    }
    return result;
}
std::string trace(const ai::GalacticConnectivity& value, bool archived_only = false) {
    std::ostringstream out; out << value.revision << ';';
    for (const auto id : value.stealth_fleets) out << "stealth:" << id << ';';
    for (const auto& row : value.goal_reach) { if (archived_only && row.policy == "Single_Hop_Disconnected") continue; out << row.player << ':' << row.source << ':' << row.policy << ':'; for (const auto id : row.production_sites) out << id << ','; out << ';'; }
    for (const auto& row : value.island_paths) out << row.player << ':' << row.planet << ':' << row.least_force << ':' << row.largest << ';';
    return out.str();
}
std::string connectivity_players(const eawr::sim::PartitionExecutor& executor, bool fog) {
    auto config = fixture();
    for (auto& seat : config.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
    auto session = take(world_session(config));
    auto setup = perception(fog);
    for (auto& player : setup.players) { player.ai = true; player.human = false; player.neutral = false; }
    for (const auto* name : {"TARGETPLAYERID", "ENEMYFORCE.HASSPACEFORCE",
            "ENEMYFORCE.SPACETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDTOTALUNNORMALIZED"})
        setup.cache_intervals[name] = real(1) / real(2);
    Host host(setup);
    cp::GalacticAiUsabilityProvider provider(content(setup));
    std::string result;
    for (unsigned tick = 0; tick <= 32; ++tick) {
        if (tick) (void)take(session.step(executor));
        if (tick == 7) (void)take(session.set_planet_access(3, true, true));
        if (tick == 19) (void)take(session.set_planet_access(3, false, true));
        done(host.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
        const auto output = take(provider.connectivity_publication(session, host.world, host.engine, executor));
        expect(output->goal_reach.size() == setup.players.size() * config.planets.size() * 6,
            "every AI player publishes every target and policy");
        expect(output->island_sizes.size() == setup.players.size(), "every AI player publishes its island sizes");
        result += trace(*output);
        for (const auto& row : output->island_sizes)
            result += std::to_string(row.player) + ":" + std::to_string(row.largest) + ":" + std::to_string(row.second) + ";";
        const auto native = take(provider.capture());
        expect(take(provider.record_sync_hash()) == take(provider.record_sync_hash(true)),
            "ordered player commit keeps the warm and cold provider digests equal");
        expect(take(host.world.record_sync_hash()) == take(host.world.record_sync_hash(true)),
            "player perception writes keep the warm and cold world digests equal");
        result += take(provider.state_hash()) + take(provider.record_sync_hash())
            + take(host.world.state_hash()) + take(host.world.record_sync_hash());
        for (const auto& component : native.components)
            result.append(reinterpret_cast<const char*>(component.second.data()), component.second.size());
        const auto work = provider.work();
        expect(take(provider.connectivity_publication(session, host.world, host.engine, executor)) == output,
            "unchanged player preparation keeps the same immutable publication");
        expect(provider.work().generations == work.generations, "an idle publication does not rerun player refreshes");
    }
    return result;
}
// An earlier owner's changed force value invalidates later ordinary ownership
// reads, even when the later owner's own force rows have not expired. Cover a
// change in refresh and a change only in finish; splitting those phases lost
// this dependency in 8aa394290 while remaining equal across worker counts.
std::string staggered_connectivity_refresh(const eawr::sim::PartitionExecutor& executor, bool finishing) {
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    for (auto& seat : config.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
    auto session = take(world_session(config));
    auto setup = perception(false);
    for (auto& player : setup.players) { player.ai = true; player.human = false; player.neutral = false; }
    setup.cache_intervals["TARGETPLAYERID"] = real(0);
    setup.cache_intervals["ENEMYFORCE.HASSPACEFORCE"] = finishing ? real(3) : real(1) / real(2);
    for (const auto* token : {"ENEMYFORCE.SPACETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDTOTALUNNORMALIZED"})
        setup.cache_intervals[token] = real(1) / real(2);
    Host host(setup); cp::GalacticAiUsabilityProvider provider(content(setup));
    const std::array equations{
        take(ai::parse_equation("PRESENCE", "Variable_Target.EnemyForce.HasSpaceForce", {})),
        take(ai::parse_equation("SPACE", "Variable_Target.EnemyForce.SpaceTotalUnnormalized", {})),
        take(ai::parse_equation("GROUND", "Variable_Target.EnemyForce.GroundTotalUnnormalized", {}))};
    const auto seed = [&](gc::PlayerId player) {
        ai::PlayerAi selected; selected.player = player;
        for (const auto& planet : config.planets) {
            ai::Target target; target.object = planet.id; target.name = "OBJECT_" + std::to_string(planet.id);
            const auto context = host.world.goal_context(selected, &target);
            for (const auto& equation : equations) {
                ai::AiRandom random;
                expect(host.world.perception(host.engine, equation, context, random).has_value(),
                    "staggered force seed uses a qualified owning context");
            }
        }
    };
    (void)take(session.step(executor));
    done(host.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    seed(1);
    if (finishing) {
        const std::array<eawr::sim::EntityId, 1> losses{51};
        done(session.apply_battle_losses(losses));
    } else done(session.apply_actor_stealth(51, 0, f(1), true));
    while (session.completed_tick() < 10) (void)take(session.step(executor));
    ai::GalacticConnectivity inputs;
    if (!finishing) inputs.stealth_fleets.insert(50);
    done(host.world.bind(session.snapshot(), std::move(inputs), executor, session.completed_tick(), session.next_entity_id()));
    seed(3);
    const auto cached = [](const ai::GalacticPerceptionState& state, gc::PlayerId player, std::string_view token) {
        return std::find_if(state.cache.begin(), state.cache.end(), [&](const auto& row) {
            return row.key.player == player && row.key.planet == 2 && !row.key.tokens.empty()
                && row.key.tokens.back() == token;
        });
    };
    const auto first = take(provider.connectivity_publication(session, host.world, host.engine, executor));
    const auto before = host.world.state();
    const auto token = finishing ? "SPACETOTALUNNORMALIZED" : "HASSPACEFORCE";
    const auto early = cached(before, 1, token), late = cached(before, 3, token);
    expect(early != before.cache.end() && early->frame == 1 && early->value == (finishing ? real(42) : real(1)),
        "early owner keeps its pre-change force value through its real expiry");
    expect(late != before.cache.end() && late->frame == 10 && late->value == real(0),
        "later owner has the current force value with a later expiry");
    const auto observer_generation = session.snapshot()->observer_input_generation();
    while (session.completed_tick() < 17) (void)take(session.step(executor));
    expect(session.snapshot()->observer_input_generation() == observer_generation,
        "staggered expiry runs on a quiet observer generation");
    const auto output = take(provider.connectivity_publication(session, host.world, host.engine, executor));
    const auto after = host.world.state();
    const auto changed = cached(after, 1, token), retained = cached(after, 3, token);
    expect(changed != after.cache.end() && changed->frame == 17 && changed->value == real(0),
        "early owner refreshes the changed force value at its first expired read");
    expect(retained != after.cache.end() && retained->frame == 10,
        "later owner's force value remains a served remembered row");
    std::size_t owners{}, observations{};
    for (const auto& row : after.cache) if (row.key.player == 3
        && row.key.tokens == std::vector<std::string>{"VARIABLE_TARGET", "TARGETPLAYERID"}) {
        ++owners; expect(row.frame == 17, "earlier force invalidation repeats later zero-interval ownership reads");
    }
    for (const auto& row : after.visibility) if (row.player == 3 && row.mask == gc::bit(gc::Intelligence::ownership)) {
        ++observations; expect(row.last_observed == 17, "later ownership observations retain the legacy synchronization stamp");
    }
    expect(owners == config.planets.size() && observations == config.planets.size(),
        "every later-owner planet retains its ordinary ownership observation");
    expect(take(provider.record_sync_hash()) == take(provider.record_sync_hash(true))
        && take(host.world.record_sync_hash()) == take(host.world.record_sync_hash(true)),
        "corrective player publication keeps warm and cold record digests equal");
    // The seed also created strength rows for allied planets which the site
    // pass never reads. Their expired deadlines legitimately request another
    // refresh. Establish ordinary, unexpired inputs before testing idle reuse.
    expect(host.world.force_cache_deadline(1, setup.logical_fps, false) < real(17),
        "unused seeded force rows still have an expired deadline");
    for (const auto& player : setup.players) {
        seed(player.player);
        expect(host.world.force_cache_deadline(player.player, setup.logical_fps, false) >= real(17),
            "idle reuse requires every seeded force deadline to remain valid");
    }
    const auto settled = take(provider.connectivity_publication(session, host.world, host.engine, executor));
    const auto work = provider.work();
    expect(take(provider.connectivity_publication(session, host.world, host.engine, executor)) == settled
        && provider.work().generations == work.generations, "joined revision publication does not spend another refresh");
    return trace(*first) + trace(*output) + trace(*settled) + take(provider.state_hash()) + take(host.world.state_hash());
}
std::string provider_frontiers(const eawr::sim::PartitionExecutor& executor) {
    auto config = fixture();
    for (auto& seat : config.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
    auto session = take(world_session(config));
    (void)take(session.step(executor));
    auto setup = perception(false);
    for (auto& player : setup.players) { player.ai = true; player.human = false; player.neutral = false; }
    Host host(setup); cp::GalacticAiUsabilityProvider provider(content(setup));
    done(host.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    (void)take(provider.connectivity_publication(session, host.world, host.engine, executor));
    const auto frozen = take(provider.state_hash());
    auto changed = take(world_session(config));
    (void)take(changed.step(executor));
    (void)take(changed.set_planet_access(2, true, true));
    done(changed.apply_actor_stealth(51, 0, f(1), true));
    Host changed_host(setup), sibling_host(setup);
    ai::GalacticConnectivity changed_inputs; changed_inputs.stealth_fleets.insert(50);
    done(changed_host.world.bind(changed.snapshot(), std::move(changed_inputs), executor,
        changed.completed_tick(), changed.next_entity_id()));
    done(sibling_host.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    const std::array<gc::PlayerId, 2> players{1, 3};
    const auto run = [&]() {
        done(provider.prepare_decision_frontiers(executor, players));
        done(executor.execute_phase("test-usability-frontiers", players.size(), [&](std::size_t index) {
            const auto scope = provider.player_frontier(players[index]);
            eawr::sim::InlineExecutor inline_executor;
            if (index == 0) {
                const auto own = take(provider.movement(1, *changed.snapshot(), changed_host.world, changed_host.engine, inline_executor));
                const auto foreign = take(provider.movement(3, *changed.snapshot(), changed_host.world, changed_host.engine, inline_executor));
                expect(!own.access[1].ai_usable && !foreign.access[1].ai_usable,
                    "a chain's own and foreign queries use its changed access snapshot");
                const auto generations = provider.work().generations;
                (void)take(provider.movement(3, *changed.snapshot(), changed_host.world, changed_host.engine, inline_executor));
                expect(provider.work().generations == generations,
                    "a repeated foreign query reuses this chain's private replacement");
            } else {
                const auto own = take(provider.movement(3, *session.snapshot(), sibling_host.world, sibling_host.engine, inline_executor));
                expect(own.access[1].ai_usable,
                    "a sibling's ordinary cache is isolated from foreign access and stealth writes");
            }
            expect(take(provider.record_sync_hash()) == take(provider.record_sync_hash(true)),
                "private provider warm and cold digests describe the same frozen rows and overlay");
        }));
        expect(take(provider.state_hash()) == frozen, "player cache writes do not publish before the join");
    };
    run();
    done(provider.discard_decision_frontiers());
    expect(take(provider.state_hash()) == frozen, "discard leaves canonical provider bytes unchanged");
    run();
    {
        const auto scope = provider.player_frontier(1);
        expect(!provider.commit_decision_frontiers() && !provider.discard_decision_frontiers(),
            "the barrier cannot release an active player block");
    }
    done(provider.commit_decision_frontiers());
    expect(take(provider.record_sync_hash()) == take(provider.record_sync_hash(true)),
        "canonical provider join retains warm and cold identity");
    const auto joined = take(provider.state_hash());
    const std::array<gc::PlayerId, 1> single{1};
    done(provider.prepare_decision_frontiers(executor, single));
    {
        const auto scope = provider.player_frontier(1);
        expect(take(provider.state_hash()) == joined, "one chain keeps the ordinary provider state");
    }
    done(provider.commit_decision_frontiers());
    return joined;
}
void connectivity_player_order() {
    eawr::sim::InlineExecutor inline_frontiers;
    const auto frontier_reference = provider_frontiers(inline_frontiers);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(provider_frontiers(executor) == frontier_reference,
            "provider frontier join is identical on 1/2/4/8 workers");
    }
    eawr::test_support::ReverseOrderExecutor reverse_frontiers(8);
    expect(provider_frontiers(reverse_frontiers) == frontier_reference,
        "reverse player blocks retain canonical provider bytes");
    for (const bool fog : {false, true}) {
        eawr::sim::InlineExecutor inline_executor;
        const auto reference = connectivity_players(inline_executor, fog);
        eawr::test_support::ReverseOrderExecutor one_worker(1);
        expect(connectivity_players(one_worker, fog) == reference,
            "ordinary single-worker preparation preserves reverse-order identity");
        for (const auto& phase : one_worker.phases)
            expect(phase.name.find("galactic-ai-connectivity-") != 0,
                "one worker admits no player prepass or corrective wave");
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(connectivity_players(executor, fog) == reference,
                "player publications, archive bytes and record digests equal on 1/2/4/8 workers");
        }
        eawr::test_support::ReverseOrderExecutor reverse(8);
        expect(connectivity_players(reverse, fog) == reference,
            "reverse player partitions preserve published rows, archive bytes and record digests");
        bool players = false;
        for (const auto& phase : reverse.phases) {
            expect(phase.descending(), "every connectivity prepass visits reverse partitions");
            if (phase.name == "galactic-ai-connectivity-players") players = players || phase.partitions == 3;
        }
        expect(players, "connectivity dispatches one partition for each of the three AI players");
    }
    for (const bool finishing : {false, true}) {
        eawr::sim::InlineExecutor inline_executor;
        const auto reference = staggered_connectivity_refresh(inline_executor, finishing);
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(staggered_connectivity_refresh(executor, finishing) == reference,
                "staggered revision dependencies preserve exact hashes on 1/2/4/8 workers");
        }
        eawr::test_support::ReverseOrderExecutor reverse(8);
        expect(staggered_connectivity_refresh(reverse, finishing) == reference,
            "staggered revision dependencies preserve exact hashes in reverse player order");
    }
}
std::string route_publications(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(world_session(fixture())); Host host(perception(fog));
    done(host.world.bind(session.snapshot(), {}, executor, 1, session.next_entity_id()));
    std::map<gc::PlayerId, std::shared_ptr<const std::vector<ai::GalacticExecutionRoute>>> publications;
    std::vector<ai::GalacticExecutionRoute> flat;
    for (const auto owner : {1U, 2U}) {
        auto rows = std::make_shared<std::vector<ai::GalacticExecutionRoute>>();
        for (const auto& source : session.snapshot()->planets()) for (const auto& target : session.snapshot()->planets())
            rows->push_back({owner, source.id, target.id, false, real(source.id + target.id), real(owner), {}});
        flat.insert(flat.end(), rows->begin(), rows->end()); publications.emplace(owner, std::move(rows));
    }
    done(host.world.publish_execution_routes(true, 10, flat));
    const auto native = take(host.world.state_hash());
    done(host.world.publish_execution_route_generations(true, 10, publications));
    for (const auto& row : flat) {
        const auto* found = host.world.execution_route(row.player, row.source, row.candidate);
        expect(found && found->distance == row.distance && found->threat == row.threat,
            "player-indexed route publication equals flat decoded matrix");
        const auto& generation = *publications.at(row.player);
        expect(found >= generation.data() && found < generation.data() + generation.size(),
            "route reader borrows the player's immutable generation");
    }
    auto invalid = std::make_shared<std::vector<ai::GalacticExecutionRoute>>(*publications.at(1));
    invalid->pop_back(); auto refused = publications; refused[1] = invalid;
    expect(!host.world.publish_execution_route_generations(true, 11, refused), "incomplete route generation refuses atomically");
    expect(host.world.execution_route(1, 1, 2)->distance == real(3), "failed route publication retains the previous generation");
    auto changed = std::make_shared<std::vector<ai::GalacticExecutionRoute>>(*publications.at(1));
    changed->front().distance = real(99); auto next = publications; next[1] = changed;
    const auto* foreign = host.world.execution_route(2, 1, 1);
    done(host.world.publish_execution_route_generations(true, 11, next));
    expect(host.world.execution_route(1, 1, 1)->distance == real(99) && publications.at(1)->front().distance == real(2)
        && host.world.execution_route(2, 1, 1) == foreign, "one player refresh leaves retained and foreign generations immutable");
    expect(take(host.world.state_hash()) == native, "derived route layout does not alter native perception bytes");
    return native;
}
// #4199 cut 5: a refresh that is only a perception expiry replays the previous access rows and connecting-path pass,
// while every changed input reruns them. Replayed outputs must equal a clean rebuild by an independent provider.
std::string expiry_only_replay(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(world_session(fixture()));
    auto setup = perception(fog); auto immutable = content(setup);
    cp::GalacticAiUsabilityProvider provider(immutable); Host host(setup);
    done(host.world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    const auto body = [](const ai::GalacticConnectivity& value) { const auto text = trace(value); return text.substr(text.find(';') + 1); };
    const auto clean = [&] {
        cp::GalacticAiUsabilityProvider fresh(immutable); Host other(setup);
        done(other.world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
        const auto output = take(fresh.connectivity(session, other.world, other.engine, executor));
        expect(fresh.work().access_reuses == 0 && fresh.work().island_reuses == 0, "a first demand replays nothing");
        return body(output);
    };
    const auto first = take(provider.connectivity(session, host.world, host.engine, executor));
    const auto initial = provider.work();
    expect(initial.access_reuses == 0 && initial.island_reuses == 0 && initial.searches > 0 && initial.edges > 0,
        "the first demand computes the access rows and the connecting passes");
    expect(body(first) == clean(), "the first demand equals an independent clean rebuild");
    // A perception write that moves only the perception revision: a force term on an allied planet.
    const auto revision = host.world.movement_input_revision();
    expect(host.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized", 1, 1).has_value()
        && host.world.movement_input_revision() != revision, "the fixture moves only the perception revision");
    const auto second = take(provider.connectivity(session, host.world, host.engine, executor));
    const auto replayed = provider.work();
    expect(replayed.generations > initial.generations
        && replayed.access_reuses == replayed.generations - initial.generations
        && replayed.island_reuses > 0 && replayed.site_searches == initial.site_searches && replayed.edges == initial.edges,
        "an expiry-only refresh replays the access rows and the connecting pass without searching");
    expect(body(second) == body(first) && body(second) == clean(), "the replayed output equals a clean rebuild");
    // The rebuilt publication points at the site lists of the previous one: no list is copied or reallocated.
    std::size_t shared_lists = 0;
    expect(first.goal_reach.size() == second.goal_reach.size(), "a replayed publication keeps the row domain");
    for (std::size_t row = 0; row < first.goal_reach.size() && row < second.goal_reach.size(); ++row) {
        const auto& before = first.goal_reach[row].production_sites; const auto& after = second.goal_reach[row].production_sites;
        if (before.empty()) continue;
        expect(before.begin() == after.begin(), "an unchanged site list is shared between publications, not copied");
        ++shared_lists;
    }
    expect(shared_lists > 0, "the fixture publishes non-empty site lists");
    // The archive bytes of rows holding shared lists equal those of a provider whose rows each own their list.
    const auto native = take(provider.capture());
    cp::GalacticAiUsabilityProvider restored(immutable);
    done(restored.restore(native));
    expect(take(restored.capture()).components == native.components,
        "serialized rows from shared and unshared lists are byte-identical");
    // A changed access input reruns both, and still equals a clean rebuild.
    expect(take(session.set_planet_access(2, true, true)), "restricting a planet changes the access generation");
    const auto third = take(provider.connectivity(session, host.world, host.engine, executor));
    const auto changed = provider.work();
    expect(changed.access_reuses == replayed.access_reuses && changed.island_reuses == replayed.island_reuses
        && changed.edges > replayed.edges, "a changed access input recomputes the rows and the connecting pass");
    // Not compared with a clean rebuild: retained site lists stay valid until their sourced expiry by design (GFS-A14).
    return trace(first) + trace(third);
}
// The incremental refresh (presence and island reads served again by remembered rows, retained reach rows, shared
// flattened rows) must publish exactly what a provider that rebuilds everything from the live owners publishes, on
// every tick, with identical provider and perception archive bytes (the perception record digest included).
std::string incremental_equals_clean(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    auto session = take(world_session(config));
    auto setup = perception(fog); auto immutable = content(setup);
    cp::GalacticAiUsabilityProvider incremental(immutable), clean(immutable); clean.set_incremental(false);
    Host fast(setup), reference(setup);
    for (auto* host : {&fast, &reference}) done(host->world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    std::string log;
    std::size_t compared = 0;
    const auto compare = [&](std::string_view when) {
        const auto left = take(incremental.connectivity_publication(session, fast.world, fast.engine, executor));
        const auto right = take(clean.connectivity_publication(session, reference.world, reference.engine, executor));
        const std::string label = std::string(when) + ": ";
        expect(trace(*left) == trace(*right), label + "incremental rows equal a clean rebuild");
        expect(left->island_sizes.size() == right->island_sizes.size()
            && std::equal(left->island_sizes.begin(), left->island_sizes.end(), right->island_sizes.begin(),
                [](const auto& a, const auto& b) { return a.player == b.player && a.largest == b.largest && a.second == b.second; }),
            label + "incremental component sizes equal a clean rebuild");
        expect(take(incremental.capture()).components == take(clean.capture()).components, label + "provider archive bytes equal");
        expect(take(incremental.state_hash()) == take(clean.state_hash())
            && take(incremental.record_sync_hash()) == take(clean.record_sync_hash()), label + "provider hashes and record digest equal");
        expect(take(fast.world.capture()).components == take(reference.world.capture()).components
            && take(fast.world.state_hash()) == take(reference.world.state_hash()),
            label + "perception archive bytes and record digest equal");
        log += trace(*left); ++compared;
    };
    compare("initial");
    for (unsigned tick = 1; tick <= 460; ++tick) {
        (void)take(session.step(executor));
        // Another viewer's observation change dirties the AI's cache without touching its own remembered rows; the AI's
        // own change must not be replayed over.
        if (tick % 3 == 0) for (auto* host : {&fast, &reference}) host->world.visibility_changed(2, 1 + tick % 5, (tick / 3) % 2 == 0, 0);
        if (tick % 71 == 0) for (auto* host : {&fast, &reference}) host->world.visibility_changed(1, 1 + tick % 5, (tick / 71) % 2 == 0, 0);
        if (tick == 130) (void)take(session.set_planet_access(2, true, true));
        if (tick == 200) (void)take(session.set_planet_access(2, false, std::nullopt));
        if (tick == 300) done(session.apply_actor_stealth(51, 0, f(1), true));
        if (tick == 360) done(session.apply_actor_stealth(51, 51, {}, false));
        // A second reader renews some remembered rows early or late.
        if (tick % 53 == 0) for (auto* host : {&fast, &reference}) static_cast<void>(host->query("Variable_Target.EnemyForce.HasSpaceForce", 1, 1 + tick % 5));
        compare("tick " + std::to_string(tick));
    }
    const auto work = incremental.work(); const auto base = clean.work();
    expect(compared == 461 && work.generations == base.generations && work.searches == base.searches && work.edges == base.edges,
        "both providers do the same searches");
    expect(work.presence_reuses > 0 && work.island_read_reuses > 0 && work.row_reuses > 0 && work.block_row_reuses > 0,
        "the incremental provider replays presence reads, island reads and flattened rows");
    expect(base.presence_reuses == 0 && base.island_read_reuses == 0 && base.row_reuses == 0 && base.block_row_reuses == 0, "the clean provider replays nothing");
    return log + take(incremental.state_hash()) + take(fast.world.state_hash());
}
std::string contracts(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture();
    for (auto& row : config.observer->records) if (row.player == 2 && row.planet == 2)
        row.mask |= gc::bit(gc::Intelligence::fleet_count);
    auto session = take(world_session(config));
    auto setup = perception(fog); auto immutable = content(setup); cp::GalacticAiUsabilityProvider provider(immutable); Host host(setup);
    done(host.world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    expect(take(provider.usability(1, *session.snapshot(), 2)), "WGA-256 recorded local reveal permits foreign planet");
    expect(take(session.set_planet_access(2, true, true)) && !take(provider.usability(1, *session.snapshot(), 2)), "restriction precedes local reveal");
    const auto generation = session.snapshot()->planet_access()->generation;
    expect(!take(session.set_planet_access(2, true, true)) && session.snapshot()->planet_access()->generation == generation, "unchanged access preserves generation");
    expect(!session.set_planet_access(999, false, true), "unknown access mutation refuses");
    expect(take(provider.usability(2, *session.snapshot(), 2)), "non-AI bypass precedes access restriction");
    auto multiplayer = config; multiplayer.seats[0].kind = gc::SeatKind::human; multiplayer.seats[0].human_side_id = 1;
    auto multiplayer_session = take(world_session(multiplayer));
    (void)take(multiplayer_session.set_planet_access(2, true, false));
    expect(take(provider.usability(1, *multiplayer_session.snapshot(), 2)), "WGA-127 multiplayer branch bypasses restriction and reveal");
    (void)take(session.set_planet_access(2, false, false));
    expect(!take(provider.usability(1, *session.snapshot(), 2)), "explicit story reveal false overrides observer discovery");
    (void)take(session.set_planet_access(2, false, std::nullopt));
    const auto first = take(provider.connectivity(session, host.world, host.engine, executor));
    const auto hash_at_view = session.state_sha256();
    bool ai_count = false, human_count = false;
    for (const auto& view : session.snapshot()->observations()) if (view.planet == 2) {
        if (view.player == 1) ai_count = view.knowledge.fleet_count;
        if (view.player == 2) human_count = view.knowledge.fleet_count;
    }
    expect(ai_count != human_count && take(provider.usability(1, *session.snapshot(), 2))
        && session.state_sha256() == hash_at_view, "different viewing perspectives preserve local reveal authority and state hash");
    const auto movement = take(provider.movement(1, *session.snapshot(), host.world, host.engine, executor));
    expect(movement.access.size() == 5 && movement.access[1].relation == gc::PathRelation::hostile
        && movement.access[1].ai_relation == gc::PathRelation::hostile && movement.access[1].ai_enemy_space_force == true,
        "WGA-258 real owner, target ownership and presence are independently populated");
    expect(movement.orbital_occupants.at(2) == 2, "WGM-66 orbital occupant comes from live control");
    const auto sites = [&](std::string_view policy, gc::PlanetId target) -> std::vector<gc::PlanetId> {
        for (const auto& row : first.goal_reach) if (row.source == target && row.policy == policy) return row.production_sites.get();
        return std::vector<gc::PlanetId>{};
    };
    expect(sites("Friendly_Only", 2).empty() && sites("Enemy_Destination", 2) == std::vector<gc::PlanetId>{1}, "hostile target picks first strictly largest adjacent allied component");
    expect(sites("Single_Hop_Disconnected", 2) == std::vector<gc::PlanetId>({1, 3, 5}), "single-hop policy unions reachable adjacent components");
    expect(first.island_paths.size() == 5 && first.island_paths[1].least_force && first.island_paths[3].least_force
        && first.island_paths[1].largest, "WGA-260..262 qualified chain connectors and largest pair");
    expect(first.island_sizes.size() == 1 && first.island_sizes[0].player == 1 && first.island_sizes[0].largest == 1
        && first.island_sizes[0].second == 1, "WGA-476 three single-planet allied components publish the two largest site counts");
    const auto publication = take(provider.connectivity_publication(session, host.world, host.engine, executor));
    const auto publication_trace = trace(*publication);
    for (unsigned i = 0; i < 20; ++i)
        expect(take(provider.connectivity_publication(session, host.world, host.engine, executor)) == publication,
            "unchanged connectivity retains one immutable publication without copying rows");
    done(host.world.bind_publication(session.snapshot(), publication, executor, session.completed_tick(), session.next_entity_id()));
    expect(trace(*publication) == publication_trace, "world binding retains the completed immutable generation");
    const auto work = provider.work();
    for (unsigned i = 0; i < 20; ++i) expect(trace(take(provider.connectivity(session, host.world, host.engine, executor))) == trace(first), "unchanged output stable");
    expect(provider.work().generations == work.generations && provider.work().searches == work.searches, "WGA-265 unchanged requests prepare no planets or searches");
    // Advance a quiet world after the initial frame-zero cache boundary.
    auto quiet = take(world_session(config)); Host quiet_host(setup);
    cp::GalacticAiUsabilityProvider quiet_provider(immutable);
    (void)take(quiet_provider.connectivity(quiet, quiet_host.world, quiet_host.engine, executor));
    (void)take(quiet.step(executor));
    (void)take(quiet_provider.connectivity(quiet, quiet_host.world, quiet_host.engine, executor));
    const auto quiet_work = quiet_provider.work();
    for (unsigned i = 0; i < 8; ++i) {
        (void)take(quiet.step(executor));
        (void)take(quiet_provider.connectivity(quiet, quiet_host.world, quiet_host.engine, executor));
    }
    expect(quiet_provider.work().generations == quiet_work.generations && quiet_provider.work().searches == quiet_work.searches,
        "quiet advancing ticks reuse prepared inputs and searches before sourced expiries");
    // A perception change dirties the next request. A rebuild that lands on the published reach rows keeps that storage,
    // so the runtime's route publication copies a pointer and the world's rebind finds the same rows without a walk.
    const auto held = take(quiet_provider.connectivity_publication(quiet, quiet_host.world, quiet_host.engine, executor));
    quiet_host.world.visibility_changed(1, 2, false, 0);
    const auto rebuilt = take(quiet_provider.connectivity_publication(quiet, quiet_host.world, quiet_host.engine, executor));
    expect(rebuilt->goal_reach == held->goal_reach, "an unchanged perception rebuild publishes identical reach rows");
    expect(rebuilt == held || rebuilt->goal_reach.shares(held->goal_reach), "identical reach rows share one storage");
    ai::GalacticConnectivity copied = *held;
    expect(copied.goal_reach.shares(held->goal_reach), "copying a publication shares its reach rows");
    if (!copied.goal_reach.empty()) {
        const auto before = copied.goal_reach[0].player;
        copied.goal_reach.front().player = before + 1;
        expect(!copied.goal_reach.shares(held->goal_reach) && held->goal_reach[0].player == before,
            "editing a copy detaches it and leaves the published rows unchanged");
    }
    const auto native = take(provider.capture()); const auto saved_world = take(host.world.capture());
    cp::GalacticAiUsabilityProvider restored(immutable); Host loaded(setup);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(saved_world)); done(restored.restore(native));
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(first, true)
        && take(restored.state_hash()) == take(provider.state_hash()), "retained flags/sites/deadlines and derived output restore exactly");
    auto damaged = native; damaged.components.at("GALACTIC_USABILITY").push_back(0);
    const auto before = take(restored.state_hash()); expect(!restored.restore(damaged) && take(restored.state_hash()) == before, "trailing native bytes refuse atomically");
    damaged = native; damaged.components.at("GALACTIC_USABILITY").resize(8); expect(!restored.restore(damaged), "truncated native component refuses");
    // Qualified mask contrast: ownership is visible while fleet count is hidden.
    host.world.set_synchronized_frame(1);
    expect(host.query("Variable_Target.TargetPlayerId") == real(2), "target ownership getter reads live target ID");
    expect(host.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized") == real(42), "hidden first miss evaluates real force, rather than zero");
    host.world.set_synchronized_frame(31);
    const auto age = host.query("Variable_Target.EnemyForce.SpaceTotal.TimeLastSeen");
    expect(age == (fog ? ai::to_single(real(31) / real(31)) : real(0)), "force age uses its count visibility, independently of visible ownership");
    expect(host.query("Variable_Self.AverageAgeOfSpaceIntelligence") == (fog ? real(1) : real(0))
        && host.query("Variable_Self.AverageAgeOfGroundIntelligence") == (fog ? real(1) : real(0)), "average age over actual foreign planets uses sourced observation clocks");
    expect(!host.query("Variable_Enemy.AverageAgeOfSpaceIntelligence", 1, 2, 2), "average age rejects another selected evaluator");
    // Current owner and target allegiance are different inputs.
    auto neutral = config; neutral.observer->planets[1].target_owner = 3;
    auto neutral_session = take(world_session(neutral)); Host neutral_host(setup); cp::GalacticAiUsabilityProvider neutral_provider(immutable);
    done(neutral_host.world.bind(neutral_session.snapshot(), {}, executor, 0, neutral_session.next_entity_id()));
    const auto classified = take(neutral_provider.movement(1, *neutral_session.snapshot(), neutral_host.world, neutral_host.engine, executor));
    expect(classified.access[1].relation == gc::PathRelation::hostile && classified.access[1].ai_relation == gc::PathRelation::neutral, "WGM-19 remembers target classification separately from actual enemy allegiance");
    provider.refuse_live_stealth(); expect(!provider.movement(1, *session.snapshot(), host.world, host.engine, executor), "#2676 live stealth boundary refuses");
    restored.refuse_story_path_inputs(); expect(!restored.connectivity(session, loaded.world, loaded.engine, executor), "WST-52 live story-map boundary refuses");
    return trace(first) + take(loaded.world.state_hash()) + session.state_sha256();
}
std::string stealth_publication(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    config.observer->types[0].stealth_object = true; // authored metadata cannot override the live scalar
    for (auto& row : config.observer->records) if (row.player == 1 && row.planet == 2)
        row.mask |= gc::bit(gc::Intelligence::fleet_count);
    auto session = take(world_session(config)); session.set_hash_verification(true);
    auto setup = perception(fog); auto immutable = content(setup); Host host(setup);
    cp::GalacticAiUsabilityProvider provider(immutable);
    done(session.apply_actor_stealth(51, 0, f(1), true));
    // WGA-65: synchronized frame zero is never a cache hit. Start at frame
    // one so removal must retain the real seven-second evaluator deadline.
    (void)take(session.step(executor));
    done(host.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    expect(!provider.movement(1, *session.snapshot(), host.world, host.engine, executor),
        "unbound live stealth projection refuses a movement read");
    const auto first = take(provider.connectivity(session, host.world, host.engine, executor));
    expect(first.stealth_fleets == std::set<eawr::sim::EntityId>{50}
        && host.query("Variable_Target.EnemyForce.HasSpaceForce") == real(0)
        && host.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized") == real(42),
        "live fleet stealth masks presence while retaining actual raw strength");
    const auto work = provider.work();
    for (unsigned repeat = 0; repeat < 10; ++repeat)
        expect(trace(take(provider.connectivity(session, host.world, host.engine, executor))) == trace(first), "idle stealth publication is stable");
    expect(provider.work().stealth_preparations == work.stealth_preparations, "idle requests do not scan fleet stealth again");
    const auto native = take(provider.capture()); const auto world_saved = take(host.world.capture());
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(world_saved)); done(restored.restore(native));
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(first, true)
        && take(restored.state_hash()) == take(provider.state_hash()), "restore rebuilds stealth from the world owner without duplicate archived flags");
    done(session.apply_actor_stealth(51, 51, {}, false));
    const auto removed = take(provider.connectivity(session, host.world, host.engine, executor));
    expect(removed.stealth_fleets.empty() && removed.revision > first.revision,
        "live removal replaces the published predicate despite authored stealth metadata");
    expect(host.query("Variable_Target.EnemyForce.HasSpaceForce") == real(0), "stealth mutation retains the presence evaluator's sourced cache deadline");
    while (session.completed_tick() <= std::uint64_t(setup.logical_fps) * 8) (void)take(session.step(executor));
    (void)take(provider.connectivity(session, host.world, host.engine, executor));
    expect(host.query("Variable_Target.EnemyForce.HasSpaceForce") == real(1), "expired presence reads the removed live fleet predicate");
    auto unqualified = take(gc::GalacticSession::create(fixture())); Host missing(setup);
    cp::GalacticAiUsabilityProvider refused(immutable); const auto unchanged = take(refused.state_hash());
    expect(!refused.connectivity(unqualified, missing.world, missing.engine, executor)
        && take(refused.state_hash()) == unchanged && !missing.world.available(),
        "missing live actor owner refuses before publication without a type fallback");
    return trace(removed) + take(provider.state_hash()) + take(host.world.state_hash()) + session.state_sha256();
}
std::string expired_connectivity(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    if (fog) for (auto& record : config.observer->records) if (record.player == 1)
        record.mask |= gc::bit(gc::Intelligence::fleet_count) | gc::bit(gc::Intelligence::fleet_presence);
    auto session = take(world_session(config));
    auto setup = perception(fog); auto immutable = content(setup);
    Host continuous(setup); cp::GalacticAiUsabilityProvider uninterrupted(immutable);
    (void)take(session.step(executor));
    const auto initial = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    // Save after both the ten-second site lists and visible force rows expire,
    // without admitting another B producer turn in between.
    while (session.completed_tick() <= std::uint64_t(setup.logical_fps) * 15 + 1)
        (void)take(session.step(executor));
    continuous.world.set_synchronized_frame(session.completed_tick());
    const auto saved = take(uninterrupted.capture()); const auto perception_state = take(continuous.world.capture());
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(perception_state)); done(restored.restore(saved));
    const auto world_revision = loaded.world.state_revision();
    const auto input_revision = loaded.world.movement_input_revision();
    const auto world_hash = take(loaded.world.state_hash());
    const auto barrier = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(barrier, true) == trace(initial, true) && take(restored.capture()).components == saved.components,
        "expired B restore keeps exact archived lists, flags, publication revision and producer frames");
    expect(take(loaded.world.capture()).components == perception_state.components
        && take(loaded.world.state_hash()) == world_hash && loaded.world.state_revision() == world_revision
        && loaded.world.movement_input_revision() == input_revision && restored.work().searches == 0,
        "B restore rebuilds only derived lookaside/output without parent publishes or graph searches");
    // The same admitted ordinary frame refreshes both timelines. Load must not
    // advance it early, postpone it, or substitute archived ordinary reads.
    const auto expected = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    const auto resumed = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(expected) == trace(resumed) && take(uninterrupted.capture()).components == take(restored.capture()).components
        && take(continuous.world.capture()).components == take(loaded.world.capture()).components,
        "first ordinary B tick services the identical saved deadline and reproduces uninterrupted native bytes");
    expect(take(uninterrupted.capture()).components != saved.components,
        "the expired fixture actually services producer frames only after the restore barrier");
    (void)take(session.step(executor));
    expect(trace(take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor)))
        == trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)))
        && take(continuous.world.state_hash()) == take(loaded.world.state_hash())
        && take(uninterrupted.state_hash()) == take(restored.state_hash()),
        "B continued output and parent/producer hashes remain equal after the first deadline");
    return trace(resumed) + take(restored.state_hash()) + take(loaded.world.state_hash()) + session.state_sha256();
}
// #2971: the usability phase queries AI planet tokens in every partition. Its
// owner refreshes the copied economy input before the fan-out, so the supplier
// (owner caches, not thread-safe) only ever runs on the calling thread.
std::string partitioned_supplier(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(world_session(fixture()));
    auto setup = perception(fog); cp::GalacticAiUsabilityProvider provider(content(setup)); Host host(setup);
    done(host.world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    const auto runtime = ai::fresh_economy_runtime(setup.players);
    const auto owner = std::this_thread::get_id();
    std::atomic<std::uint32_t> calls{}, foreign{};
    host.world.bind_economy_supplier([&] {
        ++calls; if (std::this_thread::get_id() != owner) ++foreign;
        return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::success(runtime);
    });
    const auto output = take(provider.connectivity(session, host.world, host.engine, executor));
    expect(calls != 0 && foreign == 0, "usability partitions read the owner's copied economy input and never call the supplier");
    host.world.bind_economy_supplier({});
    return trace(output) + std::to_string(calls.load()) + take(host.world.state_hash());
}
std::string refreshed_force_clock(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    auto session = take(world_session(config)); auto setup = perception(false);
    auto immutable = content(setup); Host continuous(setup);
    cp::GalacticAiUsabilityProvider uninterrupted(immutable);
    (void)take(session.step(executor));
    const auto initial = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    // Another ordinary consumer renews all presence clocks after B's consumed
    // seven-second expiry, while its ten-second site lists are still valid.
    while (session.completed_tick() <= std::uint64_t(setup.logical_fps) * 7 + 1)
        (void)take(session.step(executor));
    continuous.world.set_synchronized_frame(session.completed_tick());
    for (const auto& planet : session.snapshot()->planets())
        expect(continuous.query("Variable_Target.EnemyForce.HasSpaceForce", 1, planet.id).has_value(), "late reader uses qualified presence getter");
    const auto saved = take(uninterrupted.capture()); const auto world_saved = take(continuous.world.capture());
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(world_saved)); done(restored.restore(saved));
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(initial, true)
        && take(restored.capture()).components == saved.components && take(loaded.world.capture()).components == world_saved.components,
        "B restore retains its consumed expiry instead of adopting a later reader's fresh clock");
    const auto expected = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    const auto resumed = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(expected, true) == trace(resumed, true) && take(restored.capture()).components == take(uninterrupted.capture()).components
        && take(loaded.world.capture()).components == take(continuous.world.capture()).components,
        "first ordinary B tick retains the same earlier consumed expiry despite unchanged force values");
    expect(take(uninterrupted.capture()).components != saved.components, "the consumed-expiry fixture actually admits a refresh");
    for (const auto& row : resumed.goal_reach) if (row.policy == "Single_Hop_Disconnected")
        expect(row.production_sites.empty(), "B-U07 original unsaved policy 4 remains empty through its saved TTL");
    while (session.completed_tick() <= std::uint64_t(setup.logical_fps) * 10 + 1)
        (void)take(session.step(executor));
    const auto ordinary = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    const auto due = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(ordinary) == trace(due) && take(restored.capture()).components == take(uninterrupted.capture()).components,
        "policy-4 output rejoins uninterrupted play at its first strictly due refresh; saved stamps were not advanced early");
    return trace(due) + take(restored.state_hash()) + take(loaded.world.state_hash());
}
std::string pending_access_restore(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(world_session(fixture())); auto setup = perception(false);
    auto immutable = content(setup); Host continuous(setup);
    cp::GalacticAiUsabilityProvider uninterrupted(immutable);
    (void)take(session.step(executor));
    const auto initial = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    // A trusted world mutation after the last producer turn must survive the
    // replacement of process-local generations by fresh load owners.
    (void)take(session.step(executor)); (void)take(session.set_planet_access(3, true, true));
    continuous.world.set_synchronized_frame(session.completed_tick());
    const auto saved = take(uninterrupted.capture()); const auto world_saved = take(continuous.world.capture());
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(world_saved)); done(restored.restore(saved));
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(initial, true)
        && take(restored.capture()).components == saved.components && take(loaded.world.capture()).components == world_saved.components,
        "pending native access change does not admit a producer refresh inside the load barrier");
    const auto expected = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    const auto resumed = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(expected, true) == trace(resumed, true)
        && take(uninterrupted.capture()).components == take(restored.capture()).components
        && take(continuous.world.capture()).components == take(loaded.world.capture()).components,
        "first ordinary tick preserves pre-save pending-input invalidation across fresh process generations");
    expect(take(restored.capture()).components != saved.components, "pending input fixture actually refreshes producer state");
    return trace(resumed, true) + take(restored.state_hash()) + take(loaded.world.state_hash());
}
std::string pending_site_restore(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.fiscal_cycle_frames = 10000;
    auto session = take(world_session(config)); auto setup = perception(false);
    auto immutable = content(setup); Host continuous(setup);
    cp::GalacticAiUsabilityProvider uninterrupted(immutable);
    (void)take(session.step(executor));
    const auto initial = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    while (session.completed_tick() <= std::uint64_t(setup.logical_fps) * 7 + 1)
        (void)take(session.step(executor));
    done(continuous.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    (void)take(uninterrupted.movement(1, *session.snapshot(), continuous.world, continuous.engine, executor));
    const auto saved = take(uninterrupted.capture()); const auto world_saved = take(continuous.world.capture());
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(world_saved)); done(restored.restore(saved));
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(initial, true)
        && take(restored.capture()).components == saved.components
        && take(loaded.world.capture()).components == world_saved.components,
        "load retains a pending site publication without repeating already consumed movement inputs");
    const auto expected = take(uninterrupted.connectivity(session, continuous.world, continuous.engine, executor));
    const auto resumed = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    expect(trace(expected, true) == trace(resumed, true)
        && take(uninterrupted.capture()).components == take(restored.capture()).components
        && take(continuous.world.capture()).components == take(loaded.world.capture()).components,
        "first ordinary site publication preserves consumed ownership and observation stamps");
    return trace(resumed, true) + take(restored.state_hash()) + take(loaded.world.state_hash());
}
std::string reconstruction(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto setup = perception(fog); setup.force_types[0].power = real(1250);
    const auto observed_fixture = [&] {
        auto value = fixture();
        if (fog) for (auto& record : value.observer->records) if (record.player == 1)
            record.mask |= gc::bit(gc::Intelligence::fleet_count);
        return value;
    };
    auto old_session = take(world_session(observed_fixture())); Host continuous(setup);
    done(continuous.world.bind(old_session.snapshot(), {}, executor, 1, old_session.next_entity_id()));
    const auto input = [&](Host& host, const ai::DerivedExecutionRestore* marker) {
        ga::ExecutionRouteInput out; out.player = 1; out.maximum_distance = real(15);
        for (const gc::PlanetId id : {1ULL, 2ULL}) {
            ai::Target target; target.object = id;
            ai::Context context; context.player = 1; context.target = &target;
            const auto power = marker ? host.world.reconstruction_enemy_space(*marker, context)
                : host.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized", 1, id);
            if (!power) throw std::runtime_error("qualified reconstruction force missing");
            out.planets.push_back({id, old_session.snapshot()->planets()[static_cast<std::size_t>(id - 1)].position, {}, *power, {}, true});
        }
        return out;
    };
    auto uninterrupted = ga::ExecutionRoutes{}; uninterrupted.available = true; uninterrupted.next_refresh = 1001;
    uninterrupted.rows = take(ga::build_execution_routes(input(continuous, nullptr), executor)).rows;
    auto changed = observed_fixture(); changed.fleets[0].members.push_back({52, 100});
    auto current = take(world_session(changed));
    done(continuous.world.bind(current.snapshot(), {}, executor, 1000, current.next_entity_id()));
    const auto saved_world = take(continuous.world.capture()); const auto hash = take(continuous.world.state_hash());
    Host loaded(setup); done(loaded.world.bind(current.snapshot(), {}, executor, 1000, current.next_entity_id()));
    done(loaded.world.restore(saved_world));
    Host observed(setup); done(observed.world.bind(current.snapshot(), {}, executor, 1000, current.next_entity_id()));
    done(observed.world.restore(saved_world)); observed.world.set_synchronized_frame(1);
    const auto revision = loaded.world.state_revision();
    expect(loaded.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized", 1, 2, {}, true) == real(2500),
        "read-only child lookup honors expiry while reconstruction honors archived strength");
    expect(loaded.query("Game.EnemyForce.SpaceTotalUnnormalized", 1, 2, {}, true) == real(2500)
        && loaded.query("Variable_Target.EnemyForce.SpaceTotal", 1, 2, {}, true) == real(1), "global and normalized read-only force recursion publishes no cache");
    expect(loaded.query("Variable_Self.AverageAgeOfSpaceIntelligence", 1, 2, {}, true) == real(0)
        && loaded.query("Variable_Target.TargetPlayerId", 1, 2, {}, true) == real(2), "read-only missing age/ownership getters evaluate qualified facts");
    expect(take(loaded.world.state_hash()) == hash && loaded.world.state_revision() == revision,
        "read-only hit, expiry and missing nested getters preserve perception rows and revision");
    auto fogged = setup; fogged.uses_fog = true; ai::AiData equations;
    equations.equations = take(ai::EquationSet::parse({{"read-only.xml",
        "<Equations><Leaf>Game.EnemyForce.SpaceTotalUnnormalized</Leaf><Root>Function_Leaf.Evaluate + Variable_Self.AverageAgeOfSpaceIntelligence</Root></Equations>"}}, {}));
    Host cold(fogged, std::move(equations)); done(cold.world.bind(current.snapshot(), {}, executor, 1000, current.next_entity_id()));
    auto empty = cold.world.state(); empty.cache.clear(); empty.visibility.clear(); done(cold.world.restore(std::move(empty)));
    const auto cold_native = take(cold.world.capture()); const auto cold_revision = cold.world.state_revision();
    ai::Target target; target.object = 2; target.name = "OBJECT_2";
    expect(cold.world.evaluate_read_only(cold.engine, "ROOT", 1, &target) == real(2501), "nested function and absent age visibility use read-only mode throughout");
    expect(take(cold.world.capture()).components == cold_native.components && cold.world.state_revision() == cold_revision,
        "cold read-only nested query never creates cache or visibility rows");
    ga::ExecutionRetainedPlayer row; row.initialized = row.finder_present = true;
    row.next_refresh = 1001; row.node_order = {1, 2}; row.maximum_distance = real(15);
    ga::ExecutionStateContributor codec; done(codec.replace({{1, row}}));
    auto pool = std::make_shared<ga::FreestoreInput>(); pool->pool = std::make_shared<ga::FreestorePool>();
    ga::ExecutionRetainedState retained; std::map<gc::PlayerId, ga::ExecutionRoutes> routes;
    ga::LiveExecutionStateContributor owner(retained, routes, pool, [&](auto player, const ai::DerivedExecutionRestore* marker) {
        expect(player == 1 && marker, "only derived E restoration admits archived query");
        ai::Target missing; missing.object = 5; ai::Context context; context.player = 1; context.target = &missing;
        expect(loaded.world.reconstruction_enemy_space(*marker, context) == real(0), "uncached reconstruction getter is real and does not publish");
        for (const gc::PlanetId id : {1ULL, 2ULL}) {
            ai::Target at; at.object = id; ai::Context comparison; comparison.player = 1; comparison.target = &at;
            expect(loaded.world.reconstruction_enemy_space(*marker, comparison)
                == observed.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized", 1, id),
                "archived reconstruction equals ordinary cached lookup for observed fogged planets before expiry");
        }
        return eawr::core::Result<ga::ExecutionRouteInput>::success(input(loaded, marker));
    }, [](const auto&) { return eawr::core::Result<void>::success(); }, executor);
    done(owner.restore(take(codec.capture())));
    expect(take(loaded.world.state_hash()) == hash && take(owner.state_hash()) == take(codec.state_hash()),
        "expired archived cache and native execution recapture exactly after load");
    const auto route_trace = [](const ga::ExecutionRoutes& value) {
        std::ostringstream out; out << value.next_refresh << ':' << value.available << ';';
        for (const auto& r : value.rows) out << r.player << ':' << r.source << ':' << r.candidate << ':' << r.empty
            << ':' << r.distance.repr << ':' << r.threat.repr << ':' << r.prefix_threat.repr << ';';
        return out.str();
    };
    expect(route_trace(routes.at(1)) == route_trace(uninterrupted), "restore uses archived force, not expired current strength");
    const auto refresh = [&](Host& host, ga::ExecutionRoutes& matrix, std::uint64_t frame) {
        host.world.set_synchronized_frame(frame);
        return take(ga::refresh_execution_routes(matrix, frame, true, executor, [&](const auto& worker) {
            auto rows = take(ga::build_execution_routes(input(host, nullptr), worker)).rows;
            return eawr::core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::success(std::move(rows));
        }));
    };
    expect(!refresh(loaded, routes.at(1), 1000) && take(loaded.world.state_hash()) == hash, "saved E deadline delays ordinary expiry publication");
    expect(refresh(loaded, routes.at(1), 1001) && refresh(continuous, uninterrupted, 1001), "first admitted E deadline refreshes both controls");
    expect(loaded.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized") == real(2500)
        && route_trace(routes.at(1)) == route_trace(uninterrupted)
        && take(loaded.world.state_hash()) == take(continuous.world.state_hash()), "first-deadline live force, routes and native cache continue equally");
    return route_trace(routes.at(1)) + take(owner.state_hash()) + take(loaded.world.state_hash());
}
std::string discovery_counter_reuse(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.fleets.front().orbit = 1;
    auto session = take(world_session(config));
    auto setup = perception(fog); Host owner(setup);
    auto immutable = content(setup); cp::GalacticAiUsabilityProvider provider(immutable);
    (void)take(provider.connectivity(session, owner.world, owner.engine, executor));
    (void)take(session.step(executor));
    const auto first = take(provider.connectivity(session, owner.world, owner.engine, executor));
    const auto generation = session.snapshot()->observer_input_generation();
    const auto initial_snapshot = session.snapshot();
    const auto observations = initial_snapshot->observations();
    const auto native = take(provider.capture()); const auto work = provider.work();
    const auto counter = [&](const gc::GalacticSnapshot& snapshot) {
        for (const auto& record : snapshot.observer()->records)
            if (record.player == 2 && record.planet == 1) return record.reveal_counter;
        throw std::runtime_error("counter fixture record absent");
    };
    const auto before = counter(*session.snapshot()); const auto hash = session.state_sha256();
    for (unsigned i = 0; i < 8; ++i) {
        (void)take(session.step(executor));
        expect(trace(take(provider.connectivity(session, owner.world, owner.engine, executor))) == trace(first),
            "counter-only real ticks retain connectivity");
    }
    expect(counter(*session.snapshot()) == before + 8 && session.state_sha256() != hash,
        "human foreign-orbit reveal counter advances and remains canonical");
    expect(session.snapshot()->observer_input_generation() == generation
        && std::equal(observations.begin(), observations.end(),
            session.snapshot()->observations().begin(), session.snapshot()->observations().end()),
        "counter-only ticks preserve completed observer projection");
    expect(provider.work().generations == work.generations && provider.work().searches == work.searches
        && take(provider.capture()).components == native.components,
        "counter-only ticks prepare no inputs/searches or native producer writes");
    Host loaded(setup); cp::GalacticAiUsabilityProvider restored(immutable);
    done(loaded.world.bind(session.snapshot(), {}, executor, session.completed_tick(), session.next_entity_id()));
    done(loaded.world.restore(take(owner.world.capture()))); done(restored.restore(native));
    (void)take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    const auto restored_work = restored.work();
    expect(trace(take(restored.connectivity(session, loaded.world, loaded.engine, executor)), true) == trace(first, true)
        && restored.work().generations == restored_work.generations && restored.work().searches == restored_work.searches
        && take(restored.capture()).components == native.components,
        "counter-only fingerprint drift does not refresh first ordinary demand after restore");
    return trace(first) + session.state_sha256() + take(provider.state_hash()) + take(restored.state_hash());
}
void indigenous(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.control->planets[1].landed = {{90, 100, 2}};
    gc::ProductionSetup production; production.nested_creation.emplace();
    for (const auto& a : config.players) for (const auto& b : config.players)
        production.nested_creation->relations.push_back({a.player_id, b.player_id, a.player_id != b.player_id && a.player_id != 3 && b.player_id != 3});
    const auto snapshot = [&](bool relations) {
        auto input = production; if (!relations) input.nested_creation.reset();
        return std::make_shared<gc::GalacticSnapshot>(0, gc::GalacticClock{}, config.players, config.planets,
            std::vector<gc::Route>{}, config.fleets, std::vector<gc::GalacticEvent>{}, std::nullopt,
            std::nullopt, nullptr, std::nullopt, std::vector<gc::Seat>{}, std::nullopt, input, config.control);
    };
    auto setup = perception(false);
    setup.planets[1].enemy_indigenous.reset();
    setup.planets[1].indigenous_potential = std::vector<ai::GalacticIndigenousPotential>{
        {100, 1, real(7), {1, 2}}, {100, 1, real(-3), {2}},
        {100, 1, real(100), {3}}, {100, 2, real(100), {2}}, {100, 1, real(100), {1}}};
    Host host(setup); done(host.world.bind(snapshot(true), {}, executor, 1, 100));
    expect(host.query("Variable_Target.EnemyForce.GroundTotalUnnormalized {Parameter_Category=1}") == real(46), "WGA-273/274 signed ordered contribution, multi-affiliation once, explicit neutral exclusion and categories");
    expect(host.query("Variable_Target.FriendlyForce.GroundTotalUnnormalized", 2) == real(42), "friendly evaluator does not add potential indigenous power");
    expect(host.query("Variable_Target.EnemyForce.GroundTotalUnnormalized {Parameter_Category=2}") == real(0), "zero ordinary subtotal bypasses authored potential");
    Host missing(setup); done(missing.world.bind(snapshot(false), {}, executor, 1, 100));
    expect(!missing.query("Variable_Target.EnemyForce.GroundTotalUnnormalized"), "missing explicit enemy relation input refuses");
    setup.planets[1].indigenous_potential->clear(); Host empty(setup);
    done(empty.world.bind(snapshot(false), {}, executor, 1, 100));
    expect(empty.query("Variable_Target.EnemyForce.GroundTotalUnnormalized") == real(42), "known empty authored list needs no faction predicate");
}
std::string negative_connecting_hops(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.planets.resize(4); config.control->planets.resize(4);
    config.observer->planets.resize(4);
    std::erase_if(config.observer->records, [](const auto& row) { return row.planet == 5; });
    config.planets[2].owner = 2; config.planets[3].owner = 1;
    for (std::size_t i = 0; i < config.planets.size(); ++i) {
        const auto owner = config.planets[i].owner;
        config.control->planets[i].ground_base.owner = owner;
        auto& facts = config.observer->planets[i]; facts.allegiance = facts.target_owner = owner;
        facts.current_owner = facts.current_controller = owner;
    }
    config.control->planets[1].landed = {{90, 100, 2}};
    config.control->planets[2].landed = {{91, 100, 2}};
    auto setup = perception(false); setup.planets.resize(4);
    // Qualified copied potential for the synthetic graph; authored faction/
    // signed eligibility is independently exercised by indigenous().
    for (std::size_t i : {1U, 2U}) setup.planets[i].enemy_indigenous
        = std::vector<std::pair<std::uint64_t, ai::Real>>{{1, real(-2000)}};
    auto immutable = std::make_shared<cp::GalacticAiUsabilityContent>(*content(setup)); immutable->planet_order.pop_back();
    auto session = take(world_session(config)); Host host(setup);
    (void)take(session.step(executor));
    done(host.world.bind(session.snapshot(), {}, executor, 1, session.next_entity_id()));
    expect(host.query("Variable_Target.EnemyForce.GroundTotalUnnormalized", 1, 2) == real(-1958)
        && host.query("Variable_Target.EnemyForce.GroundTotalUnnormalized", 1, 3) == real(-1958),
        "legal signed potential retains negative ground totals without clamping");
    cp::GalacticAiUsabilityProvider provider(immutable);
    const auto output = take(provider.connectivity(session, host.world, host.engine, executor));
    expect(output.island_paths.size() == 4 && output.island_paths[1].least_force && output.island_paths[2].least_force,
        "four-node negative connecting cycle terminates with the qualified simple chain");
    expect(provider.work().edges <= 30, "closed connecting nodes bound five searches to the six directed chain edges each");
    const auto native = take(provider.capture()); cp::GalacticAiUsabilityProvider restored(immutable);
    done(restored.restore(native));
    expect(trace(take(restored.connectivity(session, host.world, host.engine, executor)), true) == trace(output, true)
        && take(restored.capture()).components == native.components, "negative connecting outputs restore without searching again");
    (void)take(session.set_planet_access(2, true, true));
    Host restricted_host(setup); cp::GalacticAiUsabilityProvider restricted(immutable);
    const auto expensive = take(restricted.connectivity(session, restricted_host.world, restricted_host.engine, executor));
    expect(!take(restricted.usability(1, *session.snapshot(), 2)) && expensive.island_paths[1].least_force
        && expensive.island_paths[2].least_force && restricted.work().edges <= 30,
        "connecting search can enqueue a restricted endpoint's finite sentinel instead of rejecting the unique chain");
    config.fleets[0].orbit = 4; auto defended = take(world_session(config)); Host defended_host(setup);
    auto reordered = std::make_shared<cp::GalacticAiUsabilityContent>(*immutable); reordered->planet_order = {4, 1, 2, 3};
    cp::GalacticAiUsabilityProvider defended_provider(reordered);
    const auto self_goal = take(defended_provider.connectivity(defended, defended_host.world, defended_host.engine, executor));
    bool empty_goal{};
    for (const auto& row : self_goal.goal_reach) if (row.source == 4 && row.policy == "Friendly_Only") empty_goal = row.production_sites.empty();
    expect(empty_goal && self_goal.island_paths[1].largest && self_goal.island_paths[2].largest,
        "requested representative is itself a connector goal even with empty cached Friendly_Only membership");
    return trace(output) + trace(self_goal) + take(provider.state_hash()) + take(host.world.state_hash());
}
void connecting_heap_order() {
    struct Entry { int priority{}; int identity{}; int cost{}; };
    cp::GalacticAiConnectingHeap<Entry> equal;
    for (const int id : {9, 2, 7, 1}) equal.push({5, id, 100 - id});
    for (const int expected : {9, 1, 7, 2})
        expect(equal.pop().identity == expected, "WGA-327 equal insertion stays below parent; root-last extraction ignores ID/cost/FIFO");
    expect(equal.empty(), "heap drains through the singleton case");
    cp::GalacticAiConnectingHeap<Entry> children;
    for (const auto entry : {Entry{0, 10, 0}, Entry{1, 20, 0}, Entry{1, 30, 0}, Entry{2, 40, 0}}) children.push(entry);
    for (const int expected : {10, 20, 30, 40})
        expect(children.pop().identity == expected, "WGA-327 equally improving children select left before right");
    cp::GalacticAiConnectingHeap<Entry> rise;
    for (const auto entry : {Entry{4, 10, 0}, Entry{3, 20, 0}, Entry{2, 30, 0}, Entry{-1, 40, 0}, Entry{-1, 50, 0}}) rise.push(entry);
    for (const int expected : {40, 50, 30, 20, 10})
        expect(rise.pop().identity == expected, "WGA-327 strict lower total priority rises across multiple levels");
}
std::string equal_connecting_routes(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    std::string result;
    // Two scales retain exact equal costs without depending on retail names.
    for (const std::int64_t distance : {10, 20}) {
        auto config = fixture(); config.fleets.clear();
        config.rules.maximum_fleet_movement_distance = f(1);
        config.planets[0].position = {};
        config.planets[4].position = {f(distance * 2), {}, {}};
        for (std::size_t i : {1U, 2U, 3U}) {
            config.planets[i].position = {f(distance), {}, {}};
            config.planets[i].owner = 2;
            config.control->planets[i].ground_base.owner = 2;
            auto& facts = config.observer->planets[i];
            facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = 2;
        }
        config.routes = {{10, 1, 2, f(1)}, {11, 1, 3, f(1)}, {12, 1, 4, f(1)},
            {13, 3, 5, f(1)}, {14, 4, 5, f(1)}};
        for (const auto& route : config.routes) config.control->routes.push_back({route.id, {}});
        auto setup = perception(false); setup.maximum_fleet_distance = real(1);
        auto immutable = std::make_shared<cp::GalacticAiUsabilityContent>(*content(setup));
        immutable->planet_order = {5, 1, 2, 3, 4};
        auto session = take(world_session(config)); Host host(setup);
        cp::GalacticAiUsabilityProvider provider(immutable);
        const auto output = take(provider.connectivity(session, host.world, host.engine, executor));
        const auto largest = [&](gc::PlanetId id) {
            const auto at = std::find_if(output.island_paths.begin(), output.island_paths.end(),
                [&](const auto& row) { return row.player == 1 && row.planet == id; });
            return at != output.island_paths.end() && at->largest;
        };
        // Start 1 enqueues equal-priority 2/3/4. Dead-end 2 expands first;
        // root replacement then extracts 4 before 3, reaching allied goal 5.
        // The former tuple queue selected 3 instead (WGA-321/324/327/328).
        expect(largest(4) && !largest(2) && !largest(3),
            "WGA-327 equal-cost diamond selects heap-last bridge 4 rather than ascending-ID bridge 3");
        result += trace(output) + take(provider.state_hash()) + take(host.world.state_hash()) + session.state_sha256();
    }
    return result;
}
void connecting_capacity(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture(); config.rules.maximum_fleet_movement_distance = f(15);
    config.planets.clear(); config.control->planets.clear(); config.fleets.clear();
    config.observer->planets.clear(); config.observer->records.clear();
    auto setup = perception(false); setup.planets.clear(); setup.maximum_fleet_distance = real(15);
    for (gc::PlanetId id = 1; id <= 14; ++id) {
        const gc::PlayerId owner = id == 1 || id == 14 ? 1U : 2U;
        const auto x = id == 1 ? 0 : id == 14 ? 30 : id == 13 ? 20 : 10;
        config.planets.push_back({id, {f(x), {}, {}}, owner});
        gc::ControlPlanet control; control.planet = id; control.type = 100 + id;
        control.surface_accessible = true; control.ground_base = {owner, 1};
        if (owner == 2) control.landed = {{1000 + id, 100, owner}};
        config.control->planets.push_back(control);
        gc::ObserverPlanet facts; facts.planet = id; facts.allegiance = facts.target_owner = owner;
        facts.current_owner = facts.current_controller = owner; config.observer->planets.push_back(facts);
        for (const auto& player : config.players)
            config.observer->records.push_back({player.player_id, id, true, 0, 0, gc::bit(gc::Intelligence::ownership), {}});
        setup.planets.push_back({id, "P" + std::to_string(id), 100 + id, {}, {}, {},
            std::vector<std::pair<std::uint64_t, ai::Real>>{{1, owner == 2 ? real(-2000) : real(0)}}, {}});
    }
    auto immutable = std::make_shared<cp::GalacticAiUsabilityContent>(*content(setup)); immutable->planet_order.clear();
    std::sort(config.observer->records.begin(), config.observer->records.end(), [](const auto& a, const auto& b) {
        return std::tie(a.player, a.planet) < std::tie(b.player, b.planet);
    });
    for (const auto& planet : config.planets) immutable->planet_order.push_back(planet.id);
    auto session = take(world_session(config)); Host host(setup);
    cp::GalacticAiUsabilityProvider provider(immutable); const auto before = take(provider.capture());
    const auto result = provider.connectivity(session, host.world, host.engine, executor);
    expect(!result && result.error().message.find("B-U06") != std::string::npos,
        "dense signed connecting graph refuses the original three-times-node allocation ceiling");
    expect(take(provider.capture()).components == before.components && provider.work().edges <= 3 * 14 * 13,
        "capacity refusal is bounded and leaves producer native state unchanged");
}
std::string multiplayer_view_independence(unsigned workers, bool fog) {
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto config = fixture();
    config.seats.push_back({{4}, 3, gc::SeatKind::human, 3, true, 3});
    config.observer->human_observers = {2, 3}; config.observer->solo_reference_human = 0;
    for (auto& row : config.observer->records) {
        row.discovered = row.player != 1;
        row.mask = row.player == 2 ? gc::all_intelligence : 0;
    }
    auto session = take(world_session(config));
    auto setup = perception(fog); auto immutable = content(setup);
    Host host(setup), reference(setup);
    for (auto* value : {&host, &reference}) done(value->world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    cp::GalacticAiUsabilityProvider cached(immutable), clean(immutable); clean.set_incremental(false);
    const auto before = take(cached.connectivity(session, host.world, host.engine, executor));
    const auto native = take(cached.capture());
    const auto perception_state = take(host.world.capture());
    const auto owner_hash = session.snapshot()->sha256();
    for (const auto player : {2U, 3U, 2U, 3U}) {
        const auto publication = session.view_snapshot({player});
        expect(publication && publication->view_player() == player, "MP view switch resolves each canonical human");
        const auto decision = take(cached.connectivity(session, host.world, host.engine, executor));
        expect(trace(decision) == trace(before) && take(cached.capture()).components == native.components,
            "MP AI decisions and native caches ignore UI context selection");
    }
    const auto movement = take(cached.movement(1, *session.snapshot(), host.world, host.engine, executor));
    for (const auto& row : movement.access)
        expect(!row.revealed && row.ai_usable, "MP AI uses its own canonical discovery and preserves the MP usability override");
    const auto independent = take(clean.connectivity(session, reference.world, reference.engine, executor));
    expect(trace(independent) == trace(before) && take(clean.capture()).components == native.components,
        "MP cache-on and cache-off decisions and archived bytes agree");
    cp::GalacticAiUsabilityProvider restored(immutable); Host loaded(setup);
    done(loaded.world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
    done(loaded.world.restore(perception_state)); done(restored.restore(native));
    (void)session.view_snapshot({3});
    const auto reconstructed = take(restored.connectivity(session, loaded.world, loaded.engine, executor));
    // WGA-262/329 intentionally omits policy 4's site list while saving its TTL.
    // Normalize only that sourced load behavior; every other output field and
    // all native bytes must still match across the UI view switch.
    auto loaded_expected = before; loaded_expected.goal_reach.clear();
    for (auto row : before.goal_reach) {
        if (row.policy == "Single_Hop_Disconnected") row.production_sites.clear();
        loaded_expected.goal_reach.push_back(std::move(row));
    }
    expect(trace(reconstructed) == trace(loaded_expected),
        "MP reconstruction differs only in the native-unsaved policy-4 site list");
    expect(take(restored.capture()).components == native.components,
        "MP reconstruction preserves every archived native cache byte across a view switch");
    expect(session.snapshot()->sha256() == owner_hash,
        "MP save reconstruction and view selection preserve canonical observer state");
    return trace(before);
}

void access_checkpoint(const eawr::sim::PartitionExecutor& executor) {
    const auto config = fixture(); auto game = take(cp::GameSession::create(config));
    auto access = *game.galaxy().snapshot()->planet_access(); ++access.generation;
    access.planets[3].restricted = true; access.planets[3].local_revealed = false;
    auto encoded = std::vector<std::uint8_t>{}; gc::detail::encode_planet_access(encoded, access);
    done(game.restore_planet_access(encoded));
    const auto ids = eawr::checkpoint::game_identities(config);
    const auto bytes = take(eawr::checkpoint::save_game_checkpoint(game, ids));
    const auto loaded = take(eawr::checkpoint::load_game_checkpoint(bytes, ids, {}, executor));
    expect(*loaded.session.galaxy().snapshot()->planet_access() == *game.galaxy().snapshot()->planet_access()
        && loaded.session.state_sha256() == game.state_sha256(), "section 19 disk reconstruction preserves exact access state/hash");
    auto owner = std::make_shared<cp::GalacticAiUsabilityProvider>(content(perception(false)));
    done(game.set_galactic_ai_services({owner, {}}));
    expect(!game.restore_planet_access(encoded), "GameSession refuses access restore after native installation");
    auto standalone = take(world_session(config));
    encoded.push_back(0); expect(!standalone.restore_planet_access(encoded), "malformed access restore refuses atomically");
}
}
int case_galactic_ai_usability_tests(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--connectivity-players") {
            connectivity_player_order();
            return galactic_test::failures ? 1 : 0;
        }
        connecting_heap_order();
        for (const bool fog : {false, true}) {
            std::string reference;
            for (const unsigned workers : {1U, 2U, 4U, 8U}) { indigenous(workers); connecting_capacity(workers); const auto result = multiplayer_view_independence(workers, fog) + route_publications(workers, fog) + equal_connecting_routes(workers) + deadline_projection(workers, fog) + stealth_publication(workers, fog) + discovery_counter_reuse(workers, fog) + pending_access_restore(workers) + pending_site_restore(workers) + negative_connecting_hops(workers) + refreshed_force_clock(workers) + expired_connectivity(workers, fog) + reconstruction(workers, fog) + contracts(workers, fog) + expiry_only_replay(workers, fog) + incremental_equals_clean(workers, fog) + partitioned_supplier(workers, fog); if (reference.empty()) reference = result; else expect(result == reference, "provider output and native/world hashes equal on 1/2/4/8"); }
        }
        for (const bool fog : {false, true}) expect(partitioned_supplier(16, fog) == partitioned_supplier(1, fog), "usability supplier contract 1/16 equality");
        eawr::sim::InlineExecutor executor; access_checkpoint(executor);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

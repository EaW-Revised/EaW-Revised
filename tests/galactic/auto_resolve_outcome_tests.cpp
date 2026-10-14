#include "galactic_support.hpp"
#include "eawr/campaign/auto_resolve_outcome.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace {
using namespace eawr::campaign;
using galactic_test::expect;
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
template <typename T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void check(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Fixture {
    gc::GalacticSetup setup;
    ConflictRecord conflict;
    AutoResolveRoster roster;
    AutoResolveRoundOutcome outcome;
    AutoResolveOutcomeInputs inputs;
};
Fixture fixture() {
    Fixture f; gc::SyntheticGalaxyOptions options; options.planets = 2; options.fleets_per_player = 1; options.members_per_fleet = 2;
    f.setup = gc::synthetic_galaxy(options);
    const auto planet = f.setup.planets.front().id;
    for (auto& fleet : f.setup.fleets) {
        fleet.orbit = planet; f.roster.fleets.push_back({fleet, {}, true, false}); f.roster.orbit_fleets.push_back(fleet.id);
        for (const auto& member : fleet.members) {
            AutoResolveObject object; object.id = member.id; object.owner = fleet.owner;
            object.original_type = "Original" + std::to_string(member.type); object.current_type = "Upgrade";
            object.space_capable = true; f.roster.objects.push_back(object);
            f.conflict.forces.push_back({member.id, member.type, fleet.owner, fleet.id, ForceRole::deployed, false, false});
        }
    }
    f.conflict.id = 900; f.conflict.planet = planet; f.conflict.invader = 2; f.conflict.defender = 1;
    f.roster.planet.id = planet; f.roster.planet.owner = 1; f.inputs.planet_type = "PlanetOriginal";
    f.outcome.winner = 1;
    AutoResolveAllocation loss; loss.id = f.setup.fleets.back().members.front().id; loss.owner = 2;
    loss.killer = f.setup.fleets.front().members.front().id; loss.final_blow_player = 1; loss.killed = true;
    f.outcome.allocations.push_back(loss);
    f.inputs.lethal_closure = [](const auto& request) {
        return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({request.id});
    };
    f.inputs.post_delivery_record = [](const auto&) {
        return eawr::core::Result<std::vector<AutoResolveRecordEntry>>::success({});
    };
    return f;
}
void planning_and_encoding() {
    auto f = fixture(); auto a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.destroyed == std::vector<eawr::sim::EntityId>{f.outcome.allocations.front().id}
        && a.record.size() == 1 && a.record.front().original_type == f.roster.objects[2].original_type,
        "WAR-54 records original victim type and owner rather than current upgrade");
    const auto bytes = take(encode_auto_resolve_outcome(a));
    expect(take(decode_auto_resolve_outcome(bytes)) == a, "outcome payload round trip retains all application facts");
    for (std::size_t i = 0; i < bytes.size(); ++i)
        expect(!decode_auto_resolve_outcome(std::span(bytes).first(i)), "every truncated outcome is refused");
    auto trailing = bytes; trailing.push_back(0); expect(!decode_auto_resolve_outcome(trailing), "trailing outcome bytes refused");
    auto immune = f;
    immune.inputs.lethal_closure = [](const auto&) { return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({}); };
    const auto survived = take(plan_auto_resolve_outcome(immune.conflict, immune.roster, immune.outcome, immune.inputs));
    expect(survived.destroyed.empty() && survived.deaths.size() == 1 && survived.record == a.record
        && std::all_of(survived.result.forces.begin(), survived.result.forces.end(), [](const auto& row) {
            return row.disposition == ForceDisposition::survived;
        }), "WAR-85..88 preserves immunity survivors and pre-damage history without inferring casualty success");
    auto immune_game = take(GameSession::create(f.setup));
    check(immune_game.stage_auto_resolve_outcome(survived));
    eawr::platform::ThreadWorkerAdapter immune_workers(1);
    const auto immune_tick = take(immune_game.step(immune_workers));
    expect(immune_tick.galaxy->fleets().back().members.size() == 2 && immune_game.auto_resolve_history().size() == 1,
        "ordinary damage survivor remains in the galaxy while its pre-damage battle record is retained");
    immune.inputs.lethal_closure = {};
    expect(!plan_auto_resolve_outcome(immune.conflict, immune.roster, immune.outcome, immune.inputs),
        "ordinary unit selections also require an explicit post-delivery casualty view");
    f.outcome.allocations.front().owner = 1;
    expect(!plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs), "wrong casualty owner refused");
    f = fixture(); f.outcome.retreating_player = 2;
    a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.result.retreat_player == 2U && a.retreat_fleets == std::vector<gc::FleetId>{f.setup.fleets.back().id}
        && a.result.forces.back().disposition == ForceDisposition::retreated, "WAR-49 retains survivor retreat and source fleet order");
    auto game = take(GameSession::create(f.setup)); check(game.stage_auto_resolve_outcome(a));
    eawr::platform::ThreadWorkerAdapter workers(1);
    expect(!game.step(workers), "retreat result cannot silently teleport or disappear without the WGM adapter");
    expect(!game.checkpoint_recording(), "failed outcome service seals the session and refuses saving");
    game = take(GameSession::create(f.setup));
    std::vector<gc::FleetId> retreated;
    AutoResolveOutcomeServices services;
    services.retreat = [&](auto&, const auto& request) { retreated = request.retreat_fleets; return eawr::core::Result<void>::success(); };
    game.set_auto_resolve_outcome_services(services); check(game.stage_auto_resolve_outcome(a));
    const auto returned = take(game.step(workers));
    expect(retreated == a.retreat_fleets && returned.result->retreat_player == 2U,
        "WAR-49 survivor retreat is delivered once to the ordinary-movement return service");
    f = fixture(); f.conflict.station_level = 3; f.roster.planet.base_type = "Base3";
    f.outcome.winner = 2; f.outcome.allocations = {};
    AutoResolveAllocation base; base.id = f.conflict.planet; base.base_changed = true; base.base_destroyed = true;
    f.outcome.allocations.push_back(base);
    f.inputs.base_level = [](const auto type) { return eawr::core::Result<std::uint32_t>::success(type.empty() ? 0U : 1U); };
    a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.base_changed && a.result.station_level == 0 && a.record.front().original_type == "Base3",
        "WAR-43 losing base retains an explicit level-zero clear and original-base death record");
    game = take(GameSession::create(f.setup)); check(game.stage_auto_resolve_outcome(a));
    expect(!game.step(workers), "base clear cannot silently bypass the control return adapter at level zero");
}
void container_record_and_land() {
    auto f = fixture(); auto& transport = f.roster.objects[2]; transport.kind = AutoResolveObjectKind::transport;
    transport.contents = {f.roster.objects[3].id};
    f.inputs.lethal_closure = {};
    expect(!plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs), "WAR-U04 container mortality needs explicit lethal closure");
    const auto child = f.roster.objects[3].id;
    f.inputs.lethal_closure = [child](const auto& request) { return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({request.id, child}); };
    auto a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.destroyed.size() == 2 && a.record.size() == 1 && a.record[0].original_type == f.roster.objects[3].original_type,
        "WAR-54 transport death records its contents while lethal closure supplies removed identities");
    bool delivered{};
    f.conflict.layer = BattleLayer::land;
    f.inputs.lethal_closure = [&](const auto& request) {
        delivered = true;
        return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({request.id});
    };
    f.inputs.post_delivery_record = [&](const auto&) {
        expect(delivered, "WAR-88 attrition record reads contents only after damage delivery");
        return eawr::core::Result<std::vector<AutoResolveRecordEntry>>::success({{"PostDamageOriginal", 2}});
    };
    a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.record.size() == 1 && a.record.front().original_type == "PostDamageOriginal",
        "WAR-88 attrition container history uses the explicit live post-delivery contents");
    f = fixture(); f.conflict.layer = BattleLayer::land; f.outcome = {}; f.outcome.winner = 2;
    f.roster.objects[0].kind = AutoResolveObjectKind::transport; f.roster.planet.landed = {f.roster.objects[0].id};
    f.roster.objects[2].kind = AutoResolveObjectKind::transport; f.roster.objects[3].kind = AutoResolveObjectKind::transport;
    f.roster.invader_entries = {f.roster.objects[2].id, f.roster.objects[3].id};
    a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.land_invaders == std::vector<eawr::sim::EntityId>{f.roster.objects[2].id}
        && a.destroyed == f.roster.planet.landed && a.fix_land_structures && a.result.station_level == 0,
        "WAR-56 land victory clears hostile surface presence and admits the first ordinary invader");
    f.inputs.raid_destination_entries = f.roster.invader_entries;
    a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    expect(a.land_invaders == f.roster.invader_entries, "WAR-56 raid destination keeps invasion-list landing order");
}
void session_replay_checkpoint(const std::string& corpus_path) {
    const auto f = fixture(); const auto application = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    std::ifstream input(corpus_path, std::ios::binary);
    const std::vector<std::uint8_t> corpus_bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto recorded_corpus = take(gc::parse_replay(corpus_bytes));
    expect(recorded_corpus.setup.outcomes.front().payload == take(encode_auto_resolve_outcome(application)),
        "tracked outcome corpus carries the reviewed application payload");
    std::vector<std::string> hashes; std::optional<gc::GalacticReplay> corpus;
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        auto game = take(GameSession::create(f.setup));
        if (count != 1) game.scramble_storage_for_testing();
        std::vector<AutoResolveDeath> delivered;
        AutoResolveOutcomeServices services;
        services.war_u04_lethal_effects = [&](auto&, auto deaths) { delivered.assign(deaths.begin(), deaths.end()); return eawr::core::Result<void>::success(); };
        game.set_auto_resolve_outcome_services(services);
        const auto untouched = game.galaxy().state_sha256();
        check(game.stage_auto_resolve_outcome(application));
        expect(untouched == game.galaxy().state_sha256(), "future outcome input does not change the current galaxy hash");
        expect(!game.stage_auto_resolve_outcome(application), "same conflict result cannot be scheduled twice");
        auto tick = take(game.step(workers));
        expect(tick.state_hash.get() == game.state_sha256() && tick.state_sha256 == tick.state_hash.get(),
            "outcome return publishes the committed state hash rather than the pre-application step hash");
        expect(game.galaxy().cached_state_hash().get() == game.galaxy().state_sha256(),
            "outcome return publishes the committed galaxy cache after application");
        expect(tick.result == application.result && delivered == application.deaths
            && tick.galaxy->fleets().back().members.size() == 1 && tick.galaxy->planets()[0].owner == f.setup.planets[0].owner,
            "WAR-45/62 return removes actual fleet losses without assigning political ownership");
        expect(tick.galaxy->events().size() == 3 && tick.galaxy->events()[0].kind == gc::GalacticEventKind::auto_resolve_won,
            "WAR-59 return publishes winner/loser/completion events");
        const auto loser = application.result.winner == application.invader ? application.defender : application.invader;
        expect(tick.story_notices.size() == 2 && tick.story_notices[0].player == application.result.winner
            && tick.story_notices[1].player == loser && loser != application.result.winner,
            "WAR-59 typed story winner and loser producers retain recipient order");
        const auto& won = std::get<gc::story::BattleNotice>(tick.story_notices[0].payload);
        const auto& lost = std::get<gc::story::BattleNotice>(tick.story_notices[1].payload);
        expect(won.kind == gc::story::Battle::won && lost.kind == gc::story::Battle::lost
            && won.object == application.planet && won.base == gc::story::BaseKind::space,
            "battle story retains the actual planet identity and layer");
        expect(tick.observer_story_notices(application.result.winner).size() == 1
            && tick.observer_story_notices(0).empty(), "battle notice publication filters to its player");
        expect(game.auto_resolve_history().size() == 1, "WAR-54 runtime ring receives the successful result once");
        const auto replay = take(gc::parse_replay(take(gc::write_replay(game.record()))));
        auto replayed = take(GameSession::from_replay(recorded_corpus)); replayed.set_auto_resolve_outcome_services(services);
        const auto replay_tick = take(replayed.step(workers));
        expect(replay_tick.state_sha256 == tick.state_sha256 && replay_tick.galaxy->canonical_bytes() == tick.galaxy->canonical_bytes(),
            "tag 11 outcome corpus replays full hash and snapshot facts");
        cp::Identities identities; identities.content = f.setup.content_identity; identities.campaign = f.setup.campaign_identity;
        const auto checkpoint = take(cp::save_game_checkpoint(game, identities));
        cp::GameLoadOptions options; options.auto_resolve_outcomes = services;
        auto restored = take(cp::load_game_checkpoint(checkpoint, identities, options, workers));
        expect(restored.session.state_sha256() == game.state_sha256() && restored.session.auto_resolve_history().empty(),
            "checkpoint restores applied section 12 while WAR-63 drops the runtime record ring");
        for (std::size_t i = 0; i < 3; ++i) {
            const auto continued = take(game.step(workers)); const auto resumed = take(restored.session.step(workers));
            expect(continued.state_sha256 == resumed.state_sha256, "checkpoint continuation retains ordered results");
            if (count == 1) hashes.push_back(continued.state_sha256); else expect(hashes[i] == continued.state_sha256, "outcome fixture equals on 1/2/4/8 workers");
        }
        if (count == 1) corpus = replay; else expect(*corpus == replay, "outcome corpus input is independent of worker count");
    }
}
void surface_retreat_before_cleanup() {
    const auto f = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    setup.seats = {{{1}, 2, gc::SeatKind::human, 0, true, 2}, {{2}, 1, gc::SeatKind::ai, 1, true, 0}};
    setup.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}};
    setup.routes = {{4, 1, 2, f(2)}}; setup.rules.maximum_fleet_movement_distance = f(12);
    gc::Fleet invader; invader.id = 10; invader.owner = 2; invader.orbit = 1; invader.members = {{20, 2}};
    setup.fleets = {invader};
    setup.fleet_control.emplace(); setup.fleet_control->types = {{2, true, true, false}};
    setup.movement.emplace(); auto& movement = *setup.movement; movement.mode_modifier = f(1);
    movement.types = {{2, f(2), f(50), 0, true, false, false, false}};
    movement.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    movement.planets = {{1, f(1), false}, {2, f(1), false}};
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    setup.control.emplace(); auto& control = *setup.control; control.change_seconds = f(7); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : control.players) for (const auto& b : control.players)
        control.relations.push_back({a.player, b.player, a.player == b.player});
    control.types = {{2, 0, false, true}};
    for (const auto& planet : setup.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id; row.surface_accessible = true;
        control.planets.push_back(row);
    }
    control.planets[0].landed = {{30, 2, 1}, {31, 2, 1}};
    control.planets[1].ground_base = {1, 1}; control.routes = {{4, f(1)}};
    ConflictRecord conflict; conflict.id = 90; conflict.planet = 1; conflict.layer = BattleLayer::land;
    conflict.invader = 2; conflict.defender = 1; conflict.forces = {{20, 2, 2, 10}, {30, 2, 1, 1}, {31, 2, 1, 1}};
    AutoResolveRoster roster; roster.planet.id = 1; roster.planet.owner = 1; roster.planet.landed = {30, 31};
    roster.invader_entries = {20}; roster.orbit_fleets = {10}; roster.fleets.push_back({setup.fleets[0], {}, true, false});
    for (const auto& force : conflict.forces) {
        AutoResolveObject object; object.id = force.id; object.owner = force.owner;
        object.original_type = "ModTransport"; object.current_type = "ModTransport";
        object.kind = AutoResolveObjectKind::transport; object.land_capable = true; roster.objects.push_back(object);
    }
    for (const unsigned killed : {0U, 1U, 2U}) {
        AutoResolveRoundOutcome outcome; outcome.winner = 2; outcome.retreating_player = 1;
        for (unsigned i = 0; i < killed; ++i) {
            AutoResolveAllocation loss; loss.id = 31 - i; loss.owner = 1; loss.killer = 20; loss.final_blow_player = 2; loss.killed = true;
            outcome.allocations.push_back(loss);
        }
        AutoResolveOutcomeInputs inputs; inputs.planet_type = "ModWorld";
        std::size_t lethal_requests = 0;
        inputs.lethal_closure = [&](const auto& loss) { ++lethal_requests; return eawr::core::Result<std::vector<eawr::sim::EntityId>>::success({loss.id}); };
        inputs.post_delivery_record = [](const auto&) { return eawr::core::Result<std::vector<AutoResolveRecordEntry>>::success({}); };
        const auto application = take(plan_auto_resolve_outcome(conflict, roster, outcome, inputs));
        expect(lethal_requests == killed && application.deaths.size() == killed && application.destroyed.size() == killed
            && application.land_invaders == std::vector<eawr::sim::EntityId>{20},
            "WGM-186 planner retains solver casualties and requests no additional residual lethal damage for retreat survivors");
        expect(application.result.forces[1].disposition == (killed == 2 ? ForceDisposition::destroyed : ForceDisposition::retreated),
            "only a solver-surviving surface force receives retreat disposition");
        std::vector<std::string> reference;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter workers(count);
            auto game = take(GameSession::create(setup)); std::vector<std::string> order;
            AutoResolveOutcomeServices services;
            services.war_u04_lethal_effects = [&](auto&, auto) { order.push_back("damage"); return eawr::core::Result<void>::success(); };
            services.retreat = [&](auto& galaxy, const auto& a) {
                order.push_back("retreat"); const auto next_id = galaxy.next_entity_id();
                auto departed = galaxy.apply_retreat_return(*a.result.retreat_player, a.planet, 2, true, a.retreat_fleets, a.destroyed);
                if (!departed) return departed;
                if (killed == 2) expect(galaxy.next_entity_id() == next_id, "dead-only surface group does not allocate a retreat fleet");
                std::size_t living = 0;
                for (const auto& fleet : galaxy.snapshot()->fleets()) if (fleet.owner == 1) for (const auto& member : fleet.members) {
                    ++living;
                    expect(!std::binary_search(a.destroyed.begin(), a.destroyed.end(), member.id), "no solver-confirmed dead actor enters the retreat fleet");
                }
                expect(living == 2 - killed, "surface retreat packs only confirmed survivors");
                return eawr::core::Result<void>::success();
            };
            services.control = [&](auto& galaxy, const auto& a) {
                order.push_back("control");
                for (const auto& force : galaxy.snapshot()->control()->planets[0].landed)
                    expect(std::binary_search(a.destroyed.begin(), a.destroyed.end(), force.id), "only confirmed casualties remain before residual cleanup");
                gc::ControlBattleReturn returned; returned.planet = a.planet; returned.invader = a.invader; returned.defender = a.defender;
                returned.winner = a.result.winner; returned.land = true; returned.destroyed = a.destroyed;
                returned.surviving_invaders = {{20, 2, 2}}; return galaxy.apply_control_battle(returned);
            };
            services.effects = [&](auto&, const auto&) { order.push_back("effects"); return eawr::core::Result<void>::success(); };
            game.set_auto_resolve_outcome_services(services); check(game.stage_auto_resolve_outcome(application));
            const auto tick = take(game.step(workers));
            expect(order == std::vector<std::string>{"damage", "retreat", "control", "effects"}, "land callback order follows WGM-186");
            expect(tick.galaxy->control()->planets[0].landed == std::vector<gc::SurfaceForce>{{20, 2, 2}},
                "only winning transports land after survivor retreat and casualty cleanup");
            const auto identities = cp::game_identities(setup); cp::GameLoadOptions options; options.auto_resolve_outcomes = services;
            auto restored = take(cp::load_game_checkpoint(take(cp::save_game_checkpoint(game, identities)), identities, options, workers));
            const auto next = take(game.step(workers)); const auto resumed = take(restored.session.step(workers));
            expect(next.state_sha256 == resumed.state_sha256, "fresh restore retains retreat identity and return ordering");
            const std::vector<std::string> hashes{tick.state_sha256, next.state_sha256};
            if (reference.empty()) reference = hashes; else expect(reference == hashes, "surface retreat is equal on 1/2/4/8 workers");
        }
    }
}
void ring_and_failed_callback() {
    const auto f = fixture(); auto a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    a.destroyed.clear(); a.deaths.clear(); a.record.clear(); for (auto& force : a.result.forces) force.disposition = ForceDisposition::survived;
    auto game = take(GameSession::create(f.setup)); eawr::platform::ThreadWorkerAdapter workers(1);
    for (std::uint64_t i = 0; i < 10; ++i) { a.result.conflict = 900 + i; check(game.stage_auto_resolve_outcome(a)); (void)take(game.step(workers)); }
    const auto records = game.auto_resolve_history(); expect(records.size() == 8 && records.front().result.conflict == 902
        && records.back().result.conflict == 909, "WAR-54 eight-entry ring advances and retains chronological death records");
    auto failed = take(GameSession::create(f.setup)); AutoResolveOutcomeServices services;
    services.war_u04_lethal_effects = [](auto&, auto) { eawr::core::Diagnostic error; error.code = "test-lethal"; error.message = "injected lethal failure"; return eawr::core::Result<void>::failure(std::move(error)); };
    failed.set_auto_resolve_outcome_services(services); a = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
    check(failed.stage_auto_resolve_outcome(a)); expect(!failed.step(workers) && failed.auto_resolve_history().empty(), "failed lethal delivery never records a successful battle");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--generate") {
            const auto f = fixture();
            const auto application = take(plan_auto_resolve_outcome(f.conflict, f.roster, f.outcome, f.inputs));
            auto game = take(GameSession::create(f.setup));
            check(game.stage_auto_resolve_outcome(application));
            const eawr::sim::InlineExecutor executor;
            for (unsigned tick = 0; tick < 4; ++tick) (void)take(game.step(executor));
            const auto bytes = take(gc::write_replay(game.record()));
            std::ofstream file(argv[2], std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file) throw std::runtime_error("could not write outcome corpus");
            return 0;
        }
        if (argc != 2) throw std::runtime_error("expected outcome corpus path");
        planning_and_encoding(); container_record_and_land(); session_replay_checkpoint(argv[1]); ring_and_failed_callback(); surface_retreat_before_cleanup();
    }
    catch (const std::exception& error) { expect(false, error.what()); }
    return galactic_test::failures ? 1 : 0;
}

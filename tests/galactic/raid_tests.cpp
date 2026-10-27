#include "galactic_support.hpp"
#include "raid_fixture.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/fleets.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/campaign/auto_resolve_outcome.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/sim/galactic/raid.hpp"
#include "../../apps/viewer/src/galactic/raid_input.hpp"
#include <iostream>
#include <array>
#include <cstdint>
#include <utility>
#include <stdexcept>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
namespace ui = eawr::viewer::galactic;
using galactic_test::expect;
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void check(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void authored_policy() {
    namespace campaign = eawr::campaign;
    using Fixed = eawr::sim::math::Fixed;
    const auto f = [](int n) { return Fixed::from_integer(n).value(); };
    eawr::data::galactic::GalacticData data;
    data.constants = {{"Raid_Force_Required_Faction", "ModSide", {}},
        {"Raid_Force_Free_Object_Category_Mask", "ModHero", {}},
        {"Raid_Force_Limited_Object_Category_Mask", "ModGround | ModAir", {}},
        {"Raid_Force_Max_Heros", "4", f(4)}, {"Raid_Force_Max_Limited_Objects", "7", f(7)}};
    eawr::units::GalacticType troop; troop.name = "ModTroop"; troop.category_mask = {"modground"};
    eawr::units::GalacticType hero; hero.name = "ModLeader"; hero.category_mask = {"ModHero"}; hero.named_hero = true;
    eawr::units::GalacticTypeTable types; types.types = {troop, hero};
    const std::vector<campaign::FleetTypeBinding> bindings{{81, "ModTroop"}, {82, "ModLeader"}};
    const std::vector<campaign::RaidFactionBinding> factions{{41, "Other"}, {44, "modside"}};
    {
        eawr::data::galactic::GalacticData absent;
        // The loader publishes empty mask constants even when all five tags are absent.
        absent.constants = {{"Raid_Force_Free_Object_Category_Mask", "", {}},
            {"Raid_Force_Limited_Object_Category_Mask", "", {}}};
        expect(!take(campaign::build_raid_inputs(types, absent, bindings, factions)),
            "all five raid tags absent produce an empty RaidSetup without failure");
    }
    for (const auto& [text, expected] : std::array<std::pair<std::string_view, std::int32_t>, 4>{{
            {"-1.9", -1}, {"2.9", 2}, {"text", 0}, {"9999999999999999999", INT32_MAX}}}) {
        auto numeric = data; numeric.constants[3].text = text; numeric.constants[4].text = text;
        const auto parsed = take(campaign::build_raid_inputs(types, numeric, bindings, factions));
        expect(parsed && parsed->rules.max_heroes == static_cast<std::uint32_t>(expected)
            && parsed->rules.max_limited == static_cast<std::uint32_t>(expected), "LSA-36 caps retain signed atoi bits");
    }
    const auto built = take(campaign::build_raid_inputs(types, data, bindings, factions));
    expect(built && built->rules.required_faction == 44 && built->rules.max_heroes == 4 && built->rules.max_limited == 7,
        "raid faction and caps follow modified effective constants");
    expect(built && built->types[0].categories == built->rules.limited_categories
        && built->types[1].categories == built->rules.free_categories && built->types[1].hero,
        "arbitrary authored categories feed the shared predicate");
    for (const auto value : {"", " \t ", " | , "}) {
        data.constants[1].text = value;
        const auto empty_free = take(campaign::build_raid_inputs(types, data, bindings, factions));
        expect(empty_free && empty_free->types[0].categories == empty_free->rules.limited_categories
            && empty_free->types[1].categories == 0 && empty_free->types[1].hero
            && empty_free->rules.required_faction == 44 && empty_free->rules.max_heroes == 4,
            "CGS-15 empty free mask exempts nothing and retains independent faction and hero gates");
        data.constants[2].text = value;
        const auto empty_both = take(campaign::build_raid_inputs(types, data, bindings, factions));
        expect(empty_both && empty_both->types[0].categories == 0 && empty_both->types[1].categories == 0,
            "CGS-15 empty limited mask selects no objects");
        data.constants[2].text = "ModGround | ModAir";
    }
    data.constants.back().number = f(-1);
    const auto from_text = take(campaign::build_raid_inputs(types, data, bindings, factions));
    expect(from_text && from_text->rules.max_limited == 7,
        "LSA-36 raid caps parse authored integer text rather than the cached numeric projection");
}

void qualification_and_input() {
    const auto setup = raid_test::setup();
    auto seeded = setup; seeded.fleets.front().raid_destination = 2;
    expect(!gc::GalacticSession::create(seeded), "fresh campaign cannot seed a raid without an accepted command");
    const auto game = take(gc::GalacticSession::create(setup));
    const auto snapshot = game.snapshot();
    const auto& fleet = snapshot->fleets().front();
    const auto qualifies = [&](auto movement, auto control, std::span<const gc::CensusObject> actors = {}, const gc::PlanetAccessState* access = nullptr) {
        return gc::qualifies_raid(fleet, 2, movement, control, actors, access);
    };
    expect(qualifies(*snapshot->movement(), *snapshot->control()), "authored faction/category and current payload qualify a raid");
    auto multiple = *snapshot->movement(); multiple.raid->rules.max_limited = 1;
    multiple.contents.front().children.push_back({23, 2});
    expect(qualifies(multiple, *snapshot->control()), "one transport counts once even with multiple living soldiers");
    auto movement = *snapshot->movement(); movement.raid->rules.max_limited = 0;
    expect(!qualifies(movement, *snapshot->control()), "limited cap refusal");
    movement = *snapshot->movement(); movement.raid->rules.required_faction = 2;
    expect(!qualifies(movement, *snapshot->control()), "required faction refusal");
    movement = *snapshot->movement(); movement.raid->types[1].stays_in_transport = true;
    expect(!qualifies(movement, *snapshot->control()), "transport-stay policy refusal");
    movement = *snapshot->movement(); movement.contents.front().children.clear();
    expect(!qualifies(movement, *snapshot->control()), "empty current transport refuses");
    auto control = *snapshot->control(); control.planets[1].surface_accessible = false;
    expect(!qualifies(*snapshot->movement(), control), "surface access refusal");
    control = *snapshot->control(); control.relations[1].allied = true;
    expect(!qualifies(*snapshot->movement(), control), "allied target refusal");
    gc::PlanetAccessState access{1, {{1, false, {}}, {2, true, {}}}};
    expect(!qualifies(*snapshot->movement(), *snapshot->control(), {}, &access), "story restriction refusal");
    std::vector<gc::CensusObject> actors{{20, 1, 1, 1, 10, false, false, 1}, {21, 2, 1, 1, 20, false, false, 2}};
    expect(qualifies(*snapshot->movement(), *snapshot->control(), actors), "live current transport payload qualifies");
    actors.back().current_type = 3;
    expect(!qualifies(*snapshot->movement(), *snapshot->control(), actors), "current payload overrides stale movement contents");
    const std::vector<ui::InputPlanet> planets{{1, {}, 1, true}, {2, {}, 2, true}};
    auto intent = ui::raid_destination_intent(*snapshot, 1, 10, 2, planets);
    ui::MapInput input;
    expect(intent && intent->kind == ui::DropKind::ability, "raid uses the typed ability drop adapter");
    const auto command = intent ? input.command(*intent, planets) : std::nullopt;
    expect(command && std::get<gc::MoveFleet>(*command).raid, "adapter emits a raid MoveFleet");
    if (intent) {
        std::get<gc::MoveFleet>(*intent->ability).raid = false;
        expect(!input.command(*intent, planets), "ordinary movement cannot masquerade as an ability");
    }
    expect(!ui::raid_destination_intent(*snapshot, 2, 10, 2, planets), "foreign source refuses preview");
}
void retained_raid_return_excludes_unrelated_forces() {
    namespace campaign = eawr::campaign;
    auto setup = raid_test::setup();
    setup.fleets.push_back({13, 1, 2, {{23, 3}}});
    setup.control->planets[1].landed.push_back({30, 1, 1});
    setup.movement->contents.push_back({30, {{31, 2}}});
    auto world = take(gc::GalacticSession::create(setup));
    const eawr::sim::InlineExecutor executor;
    check(world.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}, true}}));
    for (unsigned tick = 0; tick < 24; ++tick) static_cast<void>(take(world.step(executor)));
    expect(world.snapshot()->fleets().front().raid_destination == 2, "return fixture has an arrived raid");
    campaign::ConflictRecord conflict; conflict.id = 90; conflict.layer = campaign::BattleLayer::land;
    conflict.planet = 2; conflict.invader = 1; conflict.defender = 2; conflict.raid = true;
    conflict.forces = {{20, 1, 1, 10}};
    campaign::BattleResult result; result.conflict = 90; result.winner = 2;
    result.forces = {{20, campaign::ForceDisposition::retreated}};
    const auto plan = take(campaign::plan_tactical_return(conflict, result, *world.snapshot()));
    expect(plan.retreat_fleets == std::vector<gc::FleetId>{10}, "raid return excludes an unrelated friendly orbit fleet");
    campaign::AutoResolveRoster roster; roster.planet.id = 2;
    for (const auto& fleet : world.snapshot()->fleets()) if (fleet.owner == 1) {
        roster.fleets.push_back({fleet, {}, true, false}); roster.orbit_fleets.push_back(fleet.id);
    }
    roster.raid_entries = {20}; roster.invader_entries = {20};
    campaign::AutoResolveObject transport; transport.id = 20; transport.owner = 1;
    transport.original_type = transport.current_type = "ModTransport"; transport.kind = campaign::AutoResolveObjectKind::transport;
    roster.objects.push_back(transport);
    campaign::AutoResolveRoundOutcome outcome; outcome.winner = 2;
    campaign::AutoResolveOutcomeInputs inputs; inputs.planet_type = "ModPlanet"; inputs.raid_destination_entries = {20};
    const auto automatic = take(campaign::plan_auto_resolve_outcome(conflict, roster, outcome, inputs));
    expect(automatic.retreat_fleets == std::vector<gc::FleetId>{10}, "autoresolve raid return excludes unrelated orbit fleets");
    campaign::TacticalReturnState state;
    check(campaign::apply_tactical_return(world, conflict, plan, state, executor));
    const auto snapshot = world.snapshot();
    expect(snapshot->fleets().front().motion.transit && !snapshot->fleets().front().raid_destination,
        "raid survivors return through ordinary movement and clear the raid target");
    const auto unrelated = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& f) { return f.id == 13; });
    expect(unrelated != snapshot->fleets().end() && unrelated->orbit == 2 && !unrelated->motion.transit,
        "unrelated orbit fleet remains parked");
    expect(snapshot->control()->planets[1].landed == std::vector<gc::SurfaceForce>{{30, 1, 1}},
        "raid return does not gather unrelated friendly surface transports");
}

void invalidated_arrival_uses_ordinary_conflict() {
    const eawr::sim::InlineExecutor executor;
    auto setup = raid_test::setup();
    setup.seat_control.emplace();
    setup.seat_control->teams = {{1, {1}, false}, {2, {2}, false}};
    setup.seat_control->controllers = {{1, gc::FactionControl::human, gc::FactionControl::ai, {}, {}},
        {2, gc::FactionControl::human, gc::FactionControl::ai, {}, {}},
        {3, gc::FactionControl::ai, gc::FactionControl::ai, {}, {}}};
    auto world = take(gc::GalacticSession::create(setup));
    check(world.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}, true}}));
    static_cast<void>(take(world.step(executor)));
    expect(world.snapshot()->fleets().front().raid_destination == 2,
        "arrival refusal fixture accepted the raid before transit");
    // A departed owner's seat can leave before the landing admission. The
    // moving fleet remains live, but that admission now refuses inactive_seat.
    check(world.submit({{1, {1}, 2}, gc::SeatLeave{}}));
    bool conflict{};
    for (unsigned tick = 1; tick < 24; ++tick) {
        static_cast<void>(take(world.step(executor)));
        for (const auto& event : world.snapshot()->events())
            if (event.kind == gc::GalacticEventKind::movement_conflict && event.subject == 10) conflict = true;
    }
    const auto& fleet = world.snapshot()->fleets().front();
    expect(!world.seats().front().active, "arrival refusal fixture committed the seat departure");
    expect(fleet.orbit == 2 && !fleet.motion.transit && !fleet.raid_destination,
        "refused arrival clears the retained raid and completes ordinary arrival");
    expect(conflict && !world.pending_native_invasion(),
        "refused raid arrival admits the ordinary hostile orbital conflict");
}

void retained_state_and_reconstruction() {
    const auto setup = raid_test::setup();
    const auto ids = cp::game_identities(setup);
    const eawr::sim::InlineExecutor executor;
    auto game = take(eawr::campaign::GameSession::create(setup));
    check(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}, true}}));
    std::vector<std::string> hashes;
    for (int tick = 0; tick < 24; ++tick) {
        hashes.push_back(take(game.step(executor)).state_hash.get());
        const auto snap = game.galaxy().snapshot();
        expect(snap->fleets().front().raid_destination == 2, "raid target retained during transit and after arrival");
        expect(std::none_of(snap->events().begin(), snap->events().end(), [](const auto& e) {
            return e.kind == gc::GalacticEventKind::movement_conflict;
        }), "raid never admits the hostile orbital fleet");
        if (tick == 0 || tick == 8 || tick == 23) {
            const auto saved = take(cp::save_game_checkpoint(game, ids));
            auto restored = take(cp::load_game_checkpoint(saved, ids, {}, executor));
            expect(restored.session.galaxy().snapshot()->fleets().front().raid_destination == 2, "checkpoint retains optional raid destination");
            expect(restored.session.state_sha256() == game.state_sha256(), "raid save/load preserves whole-game digest");
            expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "raid save/load bytes are canonical");
        }
    }
    const auto* invasion = game.galaxy().pending_native_invasion();
    expect(invasion && invasion->fleet == 10 && invasion->planet == 2 && invasion->transports == std::vector<gc::FleetMember>{{20, 1}},
        "raid arrival retains the explicit current transport identity");
    const auto replay = game.galaxy().record();
    const auto bytes = take(gc::write_replay(replay));
    auto replayed = take(eawr::campaign::GameSession::create(take(gc::parse_replay(bytes)).setup));
    for (const auto& order : replay.commands) check(replayed.submit(order));
    for (std::size_t tick = 0; tick < hashes.size(); ++tick)
        expect(take(replayed.step(executor)).state_hash.get() == hashes[tick], "recorded raid reproduces per-tick state");
    check(game.submit({{game.completed_tick(), {1}, 2}, gc::MoveFleet{10, {2, 1}, {}}}));
    static_cast<void>(take(game.step(executor)));
    expect(!game.galaxy().snapshot()->fleets().front().raid_destination, "accepted ordinary move clears retained raid target");
    auto refused_setup = setup; refused_setup.movement->raid->rules.max_limited = 0;
    auto refused = take(gc::GalacticSession::create(refused_setup));
    check(refused.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}, true}}));
    static_cast<void>(take(refused.step(executor)));
    expect(!refused.snapshot()->fleets().front().raid_destination && !refused.snapshot()->fleets().front().motion.transit,
        "refused raid leaves location and raid state unchanged");
}
}
int main() {
    try { authored_policy(); qualification_and_input(); retained_state_and_reconstruction(); retained_raid_return_excludes_unrelated_forces(); invalidated_arrival_uses_ordinary_conflict(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::checker.finish();
}

#include "galactic_support.hpp"
#include "eawr/campaign/conflict.hpp"
#include "eawr/campaign/conflict_policy.hpp"

namespace {
using namespace eawr;
using galactic_test::expect;
using sim::math::Fixed;
Fixed f(std::int64_t n) { return Fixed::from_integer(n).value(); }
campaign::AdmissionForce force(sim::EntityId id, sim::galactic::PlayerId owner, sim::galactic::TypeId type = 1) {
    campaign::AdmissionForce row;
    row.force = {id, type, owner}; row.faction = owner;
    row.population = f(1); row.power = f(10); row.space_allowed = true; row.triggers_space = true;
    return row;
}
void admission() {
    campaign::CombatantFacts unit; unit.combatant = true;
    expect(campaign::triggers_space_combat(unit), "WGM104 zero-power combatant still triggers space");
    unit.stealth = true;
    expect(!campaign::triggers_space_combat(unit), "WGM104 stealth combatant does not trigger space");
    unit.combatant = false;
    expect(campaign::can_participate_space_combat(unit), "WGM105 generic object needs no combatant interface");
    campaign::CombatantFacts transport; transport.kind = campaign::CombatantKind::transport;
    unit.combatant = true; unit.ai_power = f(1); transport.contents.push_back(unit);
    expect(campaign::can_participate_land_combat(transport), "WGM106 positive combatant payload admits ground transport");
    transport.stays_in_transport = true;
    expect(!campaign::can_participate_land_combat(transport), "WGM106 transport type stay flag blocks ground participation");
    transport.stays_in_transport = false; transport.contents[0].stays_in_transport = true;
    expect(!campaign::can_participate_land_combat(transport), "WGM106 payload stay flag blocks ground participation");
    transport.contents[0].stays_in_transport = false; transport.contents[0].ai_power = {};
    expect(!campaign::can_participate_land_combat(transport), "WGM106 zero payload power does not admit ground");
    transport.contents[0].ai_power = f(1);
    campaign::CombatantFacts company; company.kind = campaign::CombatantKind::company; company.contents.push_back(transport);
    expect(campaign::can_participate_land_combat(company), "WGM107 company delegates to its contained combatant");
    company.kind = campaign::CombatantKind::squadron;
    expect(!campaign::can_participate_land_combat(company), "WGM107 squadron never supplies ground capability");
    company.kind = campaign::CombatantKind::space_base;
    expect(!campaign::can_participate_land_combat(company), "WGM107 space base is not a ground defense");
    company.kind = campaign::CombatantKind::ground_base;
    expect(campaign::can_participate_land_combat(company), "WGM107 ground base supplies ground capability");
    campaign::LayerPresence p; p.invader = 1; p.defender = 2;
    p.invading = {force(10,1)}; p.attached_fleets = {{2, {false, true, true, false}, {force(20,2)}}};
    expect(campaign::admits_space_conflict(p), "distinct live capable orbital sides admit space");
    p.attached_fleets[0].direct_members[0].triggers_space = false;
    expect(!campaign::admits_space_conflict(p), "stealth-only defense does not trigger space conflict");
    p.attached_fleets[0].direct_members[0].fleet_behavior = true;
    expect(campaign::admits_space_conflict(p), "WGM104 attached fleet behavior supplies defense despite failed combatant trigger");
    p.attached_fleets[0].direct_members[0].fleet_behavior = false;
    p.attached_fleets[0].direct_members[0].triggers_space = true;
    p.attached_fleets[0].direct_members[0].deleted = true;
    p.attached_fleets[0].direct_members[0].space_allowed = false;
    expect(campaign::admits_space_conflict(p), "WGM105 direct orbital member has no independent deletion or type gate");
    p.attached_fleets[0].facts.deleted = true;
    expect(!campaign::admits_space_conflict(p), "WGM105 deleting outer fleet cannot supply defense");
    p.attached_fleets[0].facts.deleted = false;
    p.attached_fleets[0].direct_members[0].triggers_space = false;
    p.starbase_level = 1; p.starbase_owner = 2;
    expect(campaign::admits_space_conflict(p), "defender-owned nonzero starbase supplies space defense");
    p.invading[0].deleted = true;
    expect(!campaign::admits_space_conflict(p), "deleted attacker does not supply space forces");
    p.invader = p.defender;
    expect(!campaign::admits_space_conflict(p), "equal owners do not admit ordinary space conflict");
    p.prepared_resolver = true;
    expect(campaign::admits_space_conflict(p) && campaign::admits_land_conflict(p), "resolver override precedes normal layer checks");
    p = {}; p.invader = 1; p.defender = 2; p.invading = {force(10,1)};
    p.invading[0].ground_capable = true;
    p.attached_fleets = {{2, {false, true, true, false}, {force(20,2)}}}; p.attached_fleets[0].direct_members[0].ground_capable = true;
    expect(!campaign::admits_land_conflict(p), "orbital defender is not a landed defense");
    p.ground_base_level = 1;
    expect(campaign::admits_land_conflict(p), "nonzero ground base supplies land defense");
    p.ground_base_level = 0; p.landed = {force(20,1), force(21,2)};
    p.landed[0].ground_capable = p.landed[1].ground_capable = true;
    expect(!campaign::admits_land_conflict(p), "first capable landed invader short-circuits later defender");
    std::reverse(p.landed.begin(), p.landed.end());
    expect(campaign::admits_land_conflict(p), "land admission retains inspected surface list order");
    expect(!campaign::skips_space_combat({}), "empty flattened fleet never skips combat");
    p.invading[0].skips_combat = true;
    expect(campaign::skips_space_combat(p.invading), "all flattened members may bypass combat");
    p.invading.push_back(force(11,1));
    expect(!campaign::skips_space_combat(p.invading), "one ordinary member defeats fleet-wide bypass");
    campaign::RaidRules raid{1, 1, 2, 1, 2};
    p.invading.resize(1); p.invading[0].categories = 2;
    expect(campaign::raid_capable(p.invading, raid), "eligible limited-category force can raid");
    p.invading[0].categories = 1; p.invading[0].force.hero = true;
    expect(!campaign::raid_capable(p.invading, raid), "free hero alone lacks a limited-category unit");
    auto other = force(11,1); other.categories = 2; other.force.hero = true;
    p.invading.push_back(other);
    expect(!campaign::raid_capable(p.invading, raid), "free category does not waive hero cap");
    p.invading[0].force.hero = false; p.invading[0].transport = true; p.invading[0].stays_in_transport = true;
    expect(!campaign::raid_capable(p.invading, raid), "transport payload staying inside ground battle cannot raid");
}
void arbitration() {
    campaign::ArbitrationInputs p; p.local_involved = true;
    const auto route = [&] { return campaign::arbitrate_conflict(p).value().route; };
    using Route = campaign::ConflictRoute;
    expect(route() == Route::tactical, "ask option without queue proceeds to tactical handoff");
    p.queue_tactical = true; expect(route() == Route::pending_tactical, "authored tactical queue retains pending battle");
    p.custom_option = 0; expect(route() == Route::dialog_auto, "involved single-player always option opens solver dialog");
    p.tutorial_disables_auto = true; expect(route() == Route::pending_tactical, "forcing blocks custom always choice");
    p.profile_auto = true; expect(route() == Route::instant_auto, "profile instant resolution survives tactical forcing gate");
    p.profile_auto = false; p.local_involved = false;
    expect(route() == Route::instant_auto, "uninvolved instant resolution survives forcing gate");
    p.multiplayer = true; p.invader_human = true; p.defender_human = false;
    expect(route() == Route::instant_auto, "multiplayer nonhuman participation resolves instantly despite forcing");
    p.defender_human = true; p.tutorial_disables_auto = false;
    expect(route() == Route::instant_auto, "two-human multiplayer always option is instant");
    p.custom_option = 3; expect(route() == Route::vote, "non-forced non-ask choice enters vote branch");
    p.multiplayer = false; p.local_involved = true; p.invader_power = f(80); p.defender_power = f(10);
    p.overrun_multiple = f(8);
    expect(!campaign::arbitrate_conflict(p).value().overrun, "overrun comparison is strict, equality does not overrun");
    p.invader_power = f(81);
    expect(campaign::arbitrate_conflict(p).value().overrun && route() == Route::instant_auto, "strict stronger power enables authored overrun");
    p.defender_named_hero = true; expect(!campaign::arbitrate_conflict(p).value().overrun, "weaker named hero blocks automatic overrun");
    p.defender_named_hero = false; p.overrun_multiple = {};
    expect(!campaign::arbitrate_conflict(p).value().overrun, "WGM-93 omitted zero scalar disables stock overrun");
    p.custom_option = 4; expect(!campaign::arbitrate_conflict(p), "unknown arbitration option refuses");
}
void selection() {
    std::vector<campaign::AdmissionForce> rows;
    for (const auto id : {1U,2U,3U}) rows.push_back(force(id,1,10));
    for (const auto id : {4U,5U,6U}) rows.push_back(force(id,1,20));
    auto chosen = campaign::select_conflict_forces(rows, f(4)).value();
    std::vector<sim::EntityId> starts; for (const auto& row : chosen.initial) starts.push_back(row.id);
    expect(starts == std::vector<sim::EntityId>{3,6,2,5}, "ascending type buckets and last-listed first across fixed cap pass 2");
    expect(chosen.reinforcement.size() == 2 && chosen.reinforcement[0].id == 1 && chosen.reinforcement[1].id == 4,
        "pass 4 preserves undeployed persistent identities");
    rows[0].pathfinder = true; rows[5].superweapon = true;
    chosen = campaign::select_conflict_forces(rows, f(4), {}, true).value();
    expect(chosen.initial.size() == 2 && chosen.initial[0].id == 1 && chosen.initial[1].id == 6,
        "pathfinder starts alone except mandatory superweapon");
    chosen = campaign::select_conflict_forces(rows, f(4), 20, true).value();
    expect(chosen.initial[0].id == 4, "preferred original type precedes pathfinder flag");
    rows = {force(1,1), force(2,1), force(3,1)};
    rows[0].transport = rows[1].transport = true;
    chosen = campaign::select_conflict_forces(rows, f(1)).value();
    expect(chosen.initial.size() == 2 && chosen.initial[0].id == 1 && chosen.initial[1].id == 3,
        "transports count by number without reducing ordinary population cap");
    rows[2].force.id = 1; expect(!campaign::select_conflict_forces(rows,f(1)), "duplicate persistent force refuses selection");
}
void defended_landing() {
    namespace gc = sim::galactic;
    gc::GalacticSetup s; s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 2}}; s.fleets = {{10, 1, 1, {{20, 1}}}};
    s.rules.maximum_fleet_movement_distance = f(1);
    s.fleet_control.emplace(); s.fleet_control->types = {{1, true, true, true}, {2, false, false, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1); m.types = {{1, f(1), f(1), 0, true, false, false, false}, {2, {}, {}, 0, false, false, false, false}};
    m.types[0].fleet_container = true; // declare the fixture's live container so nested contents are reachable
    m.contents = {{20, {{21, 2}}}}; m.planets = {{1, f(1), false}};
    for (const auto& p : s.players) { m.players.push_back({p.player_id, f(1), f(1), false}); m.access.push_back({p.player_id, 1, true, false}); }
    s.control.emplace(); auto& c = *s.control; c.change_seconds = f(1); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 0, false, true}, {2, 0, false, false}};
    gc::ControlPlanet planet; planet.planet = 1; planet.type = 30; planet.surface_accessible = true; planet.ground_base = {2, 1};
    c.planets = {planet};
    const sim::InlineExecutor executor;
    for (const auto installed : {false, true}) {
        auto created = gc::GalacticSession::create(s);
        expect(bool(created), "defended landing fixture validates live transport contents");
        if (!created) { std::cerr << created.error().message << '\n'; return; }
        auto galaxy = std::move(created).value(); galaxy.enable_conflict_requests(installed);
        expect(bool(galaxy.submit({{0, {1}, 1}, gc::LandFleet{10, 1}})), "landing uses existing opcode8");
        auto step = galaxy.step(executor).value();
        const auto found = std::find_if(step.snapshot->events().begin(), step.snapshot->events().end(), [](const auto& e) {
            return e.kind == gc::GalacticEventKind::land_conflict_requested;
        });
        expect((found != step.snapshot->events().end()) == installed, "only installed admission service admits defended request");
        expect(step.snapshot->fleets().front().members == s.fleets.front().members
            && step.snapshot->control()->planets.front().landed.empty(), "defended request never performs uncontested landing");
        if (!installed) continue;
        auto presence = campaign::collect_conflict_presence(*step.snapshot, *found,
            [](const gc::FleetMember& member, gc::PlayerId owner, sim::EntityId parent) {
                auto row = force(member.id, owner, member.type); row.force.parent = parent;
                row.transport = true; row.ground_capable = true;
                return core::Result<campaign::AdmissionForce>::success(row);
            }, [](const auto&) { return core::Result<campaign::OuterFleetFacts>::success({false, true, true, false}); });
        expect(presence && presence.value().invading.size() == 1 && presence.value().invading.front().force.id == 20
            && campaign::admits_land_conflict(presence.value()), "land admission retains transport identity and separate ground-base defense");
    }
}
void counterattack_space_defender() {
    namespace gc = sim::galactic;
    // #3958: player 1 owns the planet; player 2 holds its orbit; a player 1 fleet arrives. Retail never reads the planet
    // owner for the space defender: it is the first non-stealth orbiting fleet owner that differs from the invader.
    const auto presence_for = [](gc::PlayerId planet_owner, std::vector<gc::Fleet> fleets, gc::PlayerId starbase_owner, std::uint32_t starbase_level, bool hostile_stealth = false,
        std::vector<std::pair<gc::FleetId, std::uint32_t>> arrivals = {}) {
        gc::GalacticSetup s; s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
        s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
        s.planets = {{1, {}, planet_owner}}; s.fleets = std::move(fleets);
        s.rules.maximum_fleet_movement_distance = f(1);
        // Arrivals: those fleets start at planet 2 and are ordered into planet 1's orbit one tick apart, so the orbit-entry
        // order differs from the snapshot's id order. The session numbers orbit entries only with movement inputs.
        const bool ordered_arrivals = !arrivals.empty();
        s.debug_commands = ordered_arrivals;
        if (ordered_arrivals) {
            s.planets.push_back({2, {f(12), {}, {}}, planet_owner});
            s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::human, 1, true, 2}, {{3}, 3, gc::SeatKind::human, 2, true, 2}};
            s.fleet_control.emplace(); s.fleet_control->types = {{1, true, true, true}};
            s.movement.emplace(); auto& m = *s.movement;
            m.mode_modifier = f(1); m.types = {{1, f(1), f(1), 0, true, false, false, false}}; m.planets = {{1, f(1), false}, {2, f(1), false}};
            for (const auto& p : s.players) for (const auto& q : s.planets) {
                if (q.id == 1) m.players.push_back({p.player_id, f(1), f(1), false});
                m.access.push_back({p.player_id, q.id, true, false});
            }
        }
        s.control.emplace(); auto& c = *s.control;
        c.change_seconds = f(1); c.max_ground_forces = 10;
        c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
        for (const auto& a : c.players) for (const auto& b : c.players)
            c.relations.push_back({a.player, b.player, a.player == b.player});
        c.types = {{1, 0, false, true}};
        gc::ControlPlanet surface; surface.planet = 1; surface.type = 30; surface.surface_accessible = true;
        surface.starbase = {starbase_owner, starbase_level}; surface.ground_base = {planet_owner, 1}; c.planets = {surface};
        if (ordered_arrivals) { auto other = surface; other.planet = 2; other.type = 31; other.starbase = {}; c.planets.push_back(other); }
        gc::PlanetControl state; state.planet = 1; state.initialized = true; // the planet owner is carried by its political state
        state.current_player = state.target_player = planet_owner; state.current = state.target = f(1); c.state = {state};
        if (ordered_arrivals) { auto other = state; other.planet = 2; c.state.push_back(other); }
        auto created = gc::GalacticSession::create(s);
        expect(bool(created), "counterattack fixture validates");
        if (!created) { std::cerr << created.error().message << '\n'; return campaign::LayerPresence{}; }
        auto world = std::move(created).value();
        expect(world.snapshot()->planets().front().owner == planet_owner, "the fixture keeps the declared planet owner");
        const sim::InlineExecutor executor;
        std::uint64_t sequence = 0;
        for (const auto& [fleet_id, seat] : arrivals) {
            const auto ordered = world.submit(galactic_test::orbit(world.snapshot()->completed_tick(), gc::SeatId{seat}, ++sequence, fleet_id, 1));
            expect(bool(ordered), "the fixture submits an arrival into planet 1's orbit");
            if (!ordered) { std::cerr << ordered.error().message << '\n'; continue; }
            const auto stepped = world.step(executor);
            expect(bool(stepped), "the fixture steps an arrival into planet 1's orbit");
            if (!stepped) std::cerr << stepped.error().message << '\n';
        }
        gc::GalacticEvent request; request.kind = gc::GalacticEventKind::movement_conflict;
        request.subject = 10; request.value = 1;
        return campaign::collect_conflict_presence(*world.snapshot(), request,
            [hostile_stealth](const gc::FleetMember& member, gc::PlayerId owner, sim::EntityId parent) {
                auto row = force(member.id, owner, member.type); row.force.parent = parent;
                row.skips_combat = hostile_stealth && owner != 1 && owner != 2;
                return core::Result<campaign::AdmissionForce>::success(row);
            }, [](const auto&) { return core::Result<campaign::OuterFleetFacts>::success({false, true, true, false}); }).value();
    };
    const auto shown = [](const campaign::LayerPresence& p) {
        return " [invader " + std::to_string(p.invader) + " defender " + std::to_string(p.defender) + " admits " + std::to_string(campaign::admits_space_conflict(p)) + "]";
    };
    auto presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {11, 2, 1, {{21, 1}}}}, 0, 0);
    expect(presence.invader == 1 && presence.defender == 2, std::string("#3958 hostile orbit holder defends the planet owner's own orbit") + shown(presence));
    expect(campaign::admits_space_conflict(presence), std::string("#3958 planet owner's counterattack into a hostile orbit admits a space conflict") + shown(presence));
    presence = presence_for(2, {{10, 1, 1, {{20, 1}}}, {11, 2, 1, {{21, 1}}}}, 0, 0);
    expect(presence.defender == 2 && campaign::admits_space_conflict(presence), std::string("ordinary attack on an enemy planet is unchanged") + shown(presence));
    // The AI submits the same arrival, so its counterattack into its own blockaded planet resolves identically.
    presence = presence_for(2, {{10, 2, 1, {{20, 1}}}, {11, 1, 1, {{21, 1}}}}, 0, 0);
    expect(presence.invader == 2 && presence.defender == 1 && campaign::admits_space_conflict(presence),
        std::string("#3958 an AI planet owner's counterattack into a human-held orbit admits a space conflict") + shown(presence));
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {11, 1, 1, {{22, 1}}}, {12, 3, 1, {{21, 1}}}, {13, 2, 1, {{23, 1}}}}, 0, 0);
    expect(presence.defender == 3, std::string("defender is the first hostile orbit fleet in orbit order, not the planet owner") + shown(presence));
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}}, 2, 1);
    expect(presence.defender == 2 && campaign::admits_space_conflict(presence), std::string("a hostile starbase defends when no hostile fleet is in orbit") + shown(presence));
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {11, 3, 1, {{21, 1}}}}, 0, 0, true);
    expect(presence.defender == 0 && !campaign::admits_space_conflict(presence), std::string("a stealth-only hostile orbit supplies no defender") + shown(presence));
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {11, 3, 1, {{21, 1}}}, {13, 2, 1, {{23, 1}}}}, 0, 0, true);
    expect(presence.defender == 2 && campaign::admits_space_conflict(presence), std::string("a stealth fleet is skipped for the next hostile orbit fleet") + shown(presence));
    presence = presence_for(2, {{10, 1, 1, {{20, 1}}}}, 1, 1);
    expect(presence.defender == 0 && !campaign::admits_space_conflict(presence), std::string("planet owner alone, with the invader's own starbase, has no defender") + shown(presence));
    presence = presence_for(2, {{10, 1, 1, {{20, 1}}}}, 2, 1);
    expect(presence.defender == 2 && campaign::admits_space_conflict(presence), std::string("an enemy planet owner defends with only its starbase and no orbit fleet") + shown(presence));
    // The orbit list is ordered by orbit entry, not by the snapshot's id order: fleet 13 (owner 3) enters the orbit before
    // fleet 12 (owner 2), so without the sort the id-ordered scan would pick owner 2.
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {12, 2, 2, {{22, 1}}}, {13, 3, 2, {{23, 1}}}}, 0, 0, false, {{13, 3}, {12, 2}});
    expect(presence.defender == 3, std::string("the defender is the orbit-entry winner, not the first fleet by id") + shown(presence));
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {12, 2, 2, {{22, 1}}}, {13, 3, 2, {{23, 1}}}}, 0, 0, false, {{12, 2}, {13, 3}});
    expect(presence.defender == 2, std::string("the orbit-entry order moves the defender with it") + shown(presence));
    // The tactical station follows the starbase owner when that owner is a battle side, otherwise it joins the defender.
    const auto station_owner_for = [&](const campaign::LayerPresence& p) {
        campaign::ConflictRecord record; record.invader = p.invader; record.defender = p.defender;
        record.station_owner = p.starbase_level ? p.starbase_owner : 0;
        return std::pair(record.station_owner, campaign::station_side(record));
    };
    presence = presence_for(1, {{10, 1, 1, {{20, 1}}}, {11, 2, 1, {{21, 1}}}}, 1, 1);
    expect(presence.defender == 2 && campaign::admits_space_conflict(presence) && station_owner_for(presence) == std::pair<gc::PlayerId, gc::PlayerId>(1, 1),
        std::string("counterattack: the planet owner's own starbase keeps fighting for it, not for the orbit holder") + shown(presence));
    presence = presence_for(2, {{10, 1, 1, {{20, 1}}}, {11, 2, 1, {{21, 1}}}}, 2, 1);
    expect(presence.defender == 2 && station_owner_for(presence) == std::pair<gc::PlayerId, gc::PlayerId>(2, 2),
        std::string("ordinary attack: the defender's own starbase stays with the defender") + shown(presence));
    presence = presence_for(2, {{10, 1, 1, {{20, 1}}}, {12, 3, 1, {{23, 1}}}}, 2, 1);
    expect(presence.defender == 3 && station_owner_for(presence) == std::pair<gc::PlayerId, gc::PlayerId>(2, 3),
        std::string("3-player attack: a third party's starbase reduces its own owner's base but fights on the defender's side") + shown(presence));
}
void party_resolver() {
    namespace gc = sim::galactic;
    gc::GalacticSetup setup; setup.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    setup.planets = {{1, {}, 2}};
    setup.rules.maximum_fleet_movement_distance = f(1);
    setup.control.emplace(); auto& control = *setup.control;
    control.change_seconds = f(1); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : control.players) for (const auto& b : control.players)
        control.relations.push_back({a.player, b.player, a.player == b.player});
    gc::ControlPlanet surface; surface.planet = 1; surface.type = 30; surface.surface_accessible = true;
    surface.starbase = {2, 1}; surface.ground_base = {3, 1}; control.planets = {surface};
    gc::PlanetControl state; state.planet = 1; state.initialized = true;
    state.current_player = state.target_player = 2; state.current = state.target = f(1); control.state = {state};
    auto created = gc::GalacticSession::create(setup);
    expect(bool(created), "party resolver fixture validates");
    if (!created) return;
    const auto snapshot = created.value().snapshot();
    gc::GalacticEvent event; event.kind = gc::GalacticEventKind::movement_conflict; event.value = 1;
    campaign::LayerPresence presence; presence.invader = 1; presence.starbase_owner = 2; presence.starbase_level = 1;
    presence.attached_fleets = {{3, {}, {force(30, 3)}}, {2, {}, {force(20, 2)}}};
    const auto resolve = [&] { return campaign::resolve_conflict_parties(snapshot.get(), {.arrival = &event, .presence = &presence}); };
    auto parties = resolve();
    expect(parties.defender == 3 && parties.station_owner == 2 && parties.station_side == 3,
        "resolver keeps orbit order and third-party station ownership separate");
    expect(parties.contains(1) && parties.contains(3) && !parties.contains(2), "WAR-12 refuses a third combatant owner");
    std::swap(presence.attached_fleets[0], presence.attached_fleets[1]);
    expect(resolve().defender == 2, "resolver consumes qualified orbit-entry order");
    presence.attached_fleets[0].direct_members[0].skips_combat = true;
    expect(resolve().defender == 3, "resolver skips stealth-only fleet");
    presence.attached_fleets.clear();
    expect(resolve().defender == 2, "resolver falls back to hostile starbase");
    presence.starbase_level = 0;
    expect(resolve().defender == 0, "resolver supplies no battle without a defender");
    event.kind = gc::GalacticEventKind::land_conflict_requested;
    expect(resolve().defender == 3, "land resolver chooses ground-base owner before target player");
    gc::NativeInvasion invasion; invasion.invader = 1; invasion.defender = 2;
    parties = campaign::resolve_conflict_parties(snapshot.get(), {.arrival = &event, .presence = &presence, .invasion = &invasion});
    expect(parties.defender == 2, "prepared invasion retains its admitted defender");
    campaign::ConflictRecord record; record.planet = 1; record.invader = 1; record.defender = 3; record.station_owner = 1;
    parties = campaign::resolve_conflict_parties(snapshot.get(), {.admitted = &record});
    expect(parties.invader == 1 && parties.defender == 3 && parties.station_side == 1 && parties.planet_owner == 2,
        "admitted identities and station side survive a different political planet owner");
    record.station_owner = 0;
    expect(campaign::resolve_conflict_parties(nullptr, {.admitted = &record}).station_side == 3,
        "unspecified station owner defaults to defender without a live snapshot");
    parties = campaign::resolve_conflict_parties(snapshot.get(), {.admitted = &record, .current_station_owner = true});
    expect(parties.station_owner == 2 && parties.station_side == 3, "live return retains current third-party starbase owner");
}
void pending_alert() {
    campaign::ConflictRecord conflict; conflict.invader = 1; conflict.defender = 2;
    campaign::PendingBattleAlertInputs inputs;
    inputs.local = 1; inputs.active_mode = inputs.queue_tactical = inputs.authorized_issuer = true;
    auto view = campaign::pending_battle_alert_view(conflict, inputs, true);
    expect(view.visible && view.graph_visible && view.begin_admitted && view.resolve_admitted && !view.single_button,
        "WGM-174 ordinary alert has Begin and Resolve");
    for (auto member : {&campaign::PendingBattleAlertInputs::always_bypass,
        &campaign::PendingBattleAlertInputs::linked_tactical, &campaign::PendingBattleAlertInputs::tutorial_player,
        &campaign::PendingBattleAlertInputs::tutorial_forced, &campaign::PendingBattleAlertInputs::story_prevented}) {
        inputs.*member = true;
        view = campaign::pending_battle_alert_view(conflict, inputs, true);
        expect(view.single_button && view.begin_admitted && !view.resolve_admitted, "each sourced exclusion uses the Begin-only alert");
        inputs.*member = false;
    }
    inputs.campaign_conflict_queued = true;
    expect(!campaign::pending_battle_alert_view(conflict, inputs, true).resolve_admitted,
        "WGM-177 queued campaign tactical conflict refuses Resolve");
    inputs.campaign_conflict_queued = false; inputs.active_speech_events = 1;
    expect(!campaign::pending_battle_alert_view(conflict, inputs, true).visible, "solo alert waits for speech");
    inputs.multiplayer = true; inputs.timeout_ms = 15000;
    for (const auto elapsed : {14999ULL, 15000ULL, 15001ULL}) {
        inputs.elapsed_wall_ms = elapsed;
        view = campaign::pending_battle_alert_view(conflict, inputs, true);
        expect(view.visible && view.timeout_due == (elapsed > 15000), "WGM-176 multiplayer timeout is strictly greater than authored duration");
    }
    view = campaign::pending_battle_alert_view(conflict, inputs, true, false);
    expect(view.waiting_for_opponent && !view.timeout_due && !view.begin_admitted,
        "recorded zero preference is still a recorded vote, with no duplicate timeout");
    inputs.local = 3; inputs.authorized_issuer = false;
    view = campaign::pending_battle_alert_view(conflict, inputs, true);
    expect(view.visible && !view.graph_visible && !view.begin_admitted && !view.resolve_admitted,
        "local nonparticipation blanks graphs; owner admission controls actions independently of alert visibility");
}

void mixed_defender_selection() {
    namespace gc = sim::galactic;
    gc::GalacticSetup s; s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
    s.planets = {{1, {}, 2}}; s.fleets = {{10, 1, 1, {{20, 1}}}};
    s.rules.maximum_fleet_movement_distance = f(1);
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(1); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players)
        c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 0, false, true}};
    gc::ControlPlanet surface; surface.planet = 1; surface.type = 30; surface.surface_accessible = true;
    surface.landed = {{30, 1, 1}, {31, 1, 2}, {32, 1, 3}}; c.planets = {surface};
    gc::PlanetControl state; state.planet = 1; state.initialized = true;
    state.current_player = 2; state.current = state.target = f(1); c.state = {state};
    for (const auto target : {0U, 1U, 2U, 3U, 4U}) {
        c.state[0].target_player = target == 4 ? 2 : target;
        c.planets[0].ground_base = target == 4 ? gc::ControlBase{3, 1} : gc::ControlBase{};
        auto world = gc::GalacticSession::create(s).value();
        gc::GalacticEvent request; request.kind = gc::GalacticEventKind::land_conflict_requested;
        request.subject = 10; request.value = 1;
        auto presence = campaign::collect_conflict_presence(*world.snapshot(), request,
            [](const gc::FleetMember& member, gc::PlayerId owner, sim::EntityId parent) {
                auto row = force(member.id, owner, member.type); row.force.parent = parent;
                row.transport = row.ground_capable = true;
                return core::Result<campaign::AdmissionForce>::success(row);
            }, [](const auto&) { return core::Result<campaign::OuterFleetFacts>::success({false, true, true, false}); }).value();
        expect(presence.defender == (target == 4 ? 3U : target == 0 || target == 3 ? 1U : target),
            "WGS-186 collected defender uses base, then nonneutral target, then first linked owner");
        expect(presence.landed.size() == 3 && presence.landed[0].force.owner == 1
            && presence.landed[1].force.owner == 2 && presence.landed[2].force.owner == 3,
            "mixed conflict roster preserves all three original owners");
    }
    c.state[0].target_player = 99;
    expect(!gc::GalacticSession::create(s), "unknown target identities refuse before collected defender selection");
}

}
int main() { party_resolver(); counterattack_space_defender(); mixed_defender_selection(); pending_alert(); admission(); arbitration(); selection(); defended_landing(); return galactic_test::failures ? 1 : 0; }

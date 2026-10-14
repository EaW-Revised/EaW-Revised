#include "galactic_support.hpp"
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

}
int main() { pending_alert(); admission(); arbitration(); selection(); defended_landing(); return galactic_test::failures ? 1 : 0; }

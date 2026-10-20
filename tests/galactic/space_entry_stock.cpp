#include "../support/env.hpp"
#include "../support/result.hpp"
#include "space_entry_private.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
#include <tuple>
#include <utility>
#include <vector>

using eawr::testing::take;

namespace space_entry_test {
using galactic_test::expect;

void gateway_route(const vfs::Vfs& files, const data::Catalog& catalog,
    const data::galactic::GalacticData& authored, const units::GalacticTypeTable& types) {
    const auto* source = authored.campaign("Sandbox_Gateways_Rebel");
    if (!source) throw std::runtime_error("stock Gateway campaign is absent");
    campaign::CampaignSetupOptions options; options.seed = 17;
    for (const auto& faction : authored.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool local = faction.name == source->starting_active_player;
        if (local) options.local_observer = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
    }
    for (const auto& type : types.types) options.types.push_back({assets::object_type_crc(type.name), type.name});
    std::sort(options.types.begin(), options.types.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto plan = take(campaign::plan_live_campaign_setup(files, authored, types, source->name, options, "Normal_Default"));
    auto root = take(campaign::CampaignRoot::create(plan,
        take(campaign::prepare_live_campaign_services(files, catalog, authored, types, plan, "Normal_Default"))));
    const auto provider = take(campaign::prepare_stock_space_entry(files, catalog, authored, types, plan));
    check(campaign::configure_live_campaign_conflicts(root, files, catalog, authored, types, provider));
    auto& game = *root.game();
    const auto planet = [&](std::string_view name) {
        for (const auto& row : plan.locations()) if (row.original_type == name && row.planet) return row.planet->id;
        throw std::runtime_error("Gateway planet binding is absent");
    };
    const auto player = [&](std::string_view faction) {
        for (const auto& row : plan.player_bindings()) if (row.faction == faction) return row.player.player_id;
        throw std::runtime_error("Gateway player binding is absent");
    };
    const auto type = [&](std::string_view name) {
        for (const auto& row : plan.type_bindings()) if (row.original_type == name) return row.id;
        throw std::runtime_error("Gateway unit binding is absent");
    };
    const auto bespin = planet("Bespin"), byss = planet("Byss");
    const auto rebel = player("Rebel"), empire = player("Empire");
    const auto before = game.galaxy().snapshot();
    expect(before->production() && std::any_of(before->production()->planets.begin(), before->production()->planets.end(), [&](const auto& p) {
        return p.planet == byss && std::any_of(p.objects.begin(), p.objects.end(), [&](const auto& object) {
            return object.kind == gc::ProductionKind::structure && object.type == type("E_Ground_Light_Vehicle_Factory");
        });
    }), "Gateway includes a ground factory without requiring its ground combat prototype in space");
    sim::EntityId attacking{};
    for (const auto& fleet : before->fleets()) if (fleet.owner == rebel && fleet.orbit == bespin
        && std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.type == type("Rebel_X-Wing_Squadron"); })) attacking = fleet.id;
    expect(attacking != 0, "qualified Gateway route starts with a Rebel X-wing fleet at Bespin");
    expect(std::any_of(before->fleets().begin(), before->fleets().end(), [&](const auto& fleet) {
        return fleet.owner == empire && fleet.orbit == byss && std::any_of(fleet.members.begin(), fleet.members.end(),
            [&](const auto& member) { return member.type == type("Tartan_Patrol_Cruiser"); });
    }), "qualified Gateway route has the Empire Tartan defender at Byss");
    if (!attacking) return;
    const platform::ThreadWorkerAdapter executor(1);
    check(game.submit({{game.completed_tick(), {rebel}, 1}, gc::MoveFleet{attacking, {bespin, byss}, {}}}));
    while (!game.pending_conflict() && game.completed_tick() < 6000) (void)take(game.step(executor));
    auto pending = game.pending_conflict_view();
    expect(pending && pending->conflict.planet == byss && pending->conflict.invader == rebel
        && pending->conflict.defender == empire && pending->arbitration.route == campaign::ConflictRoute::pending_tactical,
        "Bespin to Byss reaches tactical admission instead of immediate autoresolve");
    if (!pending) return;
    while (!pending->choice_ready && game.completed_tick() < 6000) {
        (void)take(game.step(executor)); pending = game.pending_conflict_view();
        if (!pending) throw std::runtime_error("Gateway announcement disappeared before Fight");
    }
    expect(pending->choice_ready && provider->available(pending->conflict), "Gateway Fight is enabled with supported reachable content");
    check(game.submit({{game.completed_tick(), {rebel}, 2}, gc::BattleChoice{pending->conflict.id, true}}));
    const auto selected = take(game.step(executor));
    if (!selected.diagnostics.empty()) {
        const auto& diagnostic = selected.diagnostics.front();
        throw std::runtime_error("Gateway Fight: " + diagnostic.code + ": " + diagnostic.message);
    }
    expect(game.battle() && game.conflict(), "Gateway Fight stages the real stock tactical child");
    if (game.battle()) (void)take(game.step(executor));
}

void installed_reserve_order(const std::shared_ptr<const campaign::StockSpaceEntry>& provider,
    const campaign::CampaignSetupPlan& plan, const data::galactic::GalacticData& authored) {
    const auto type = [&](std::string_view name) {
        for (const auto& binding : plan.type_bindings()) if (binding.original_type == name) return binding.id;
        throw std::runtime_error("stock reserve-order type missing");
    };
    gc::GalacticSetup setup; setup.seed = plan.galaxy().seed; setup.content_identity = plan.galaxy().content_identity;
    setup.players = plan.galaxy().players; setup.seats = plan.galaxy().seats; setup.planets = plan.galaxy().planets;
    setup.planets.front().owner = 2;
    setup.fleets = {{10,1,setup.planets.front().id,{}}, {11,2,setup.planets.front().id,{{200,type("Tartan_Patrol_Cruiser")}}},
        {12,1,setup.planets.front().id,{}}};
    setup.rules.maximum_fleet_movement_distance = whole(10000);
    setup.fleet_control.emplace(); setup.movement.emplace(); setup.movement->mode_modifier = whole(1);
    for (const auto name : {"Rebel_X-Wing_Squadron", "Tartan_Patrol_Cruiser"}) {
        const auto id = type(name);
        setup.fleet_control->types.push_back({id, false, true, false});
        setup.movement->types.push_back({id, whole(1), {}, 0, true, false, false, false, false});
    }
    std::sort(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::sort(setup.movement->types.begin(), setup.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& player : setup.players) {
        setup.movement->players.push_back({player.player_id, whole(1), {}, false});
        for (const auto& planet : setup.planets) setup.movement->access.push_back({player.player_id, planet.id, true, false});
    }
    for (const auto& planet : setup.planets) setup.movement->planets.push_back({planet.id, {}, false});
    for (sim::EntityId id = 100; id < 140; ++id) setup.fleets.front().members.push_back({id,type("Rebel_X-Wing_Squadron")});
    for (sim::EntityId id = 50; id < 90; ++id) setup.fleets.back().members.push_back({id,type("Rebel_X-Wing_Squadron")});
    setup.player_economy = gc::PlayerEconomySetup{{{1,whole(10000),whole(100000),0,5},{2,whole(10000),whole(100000),0,5},
        {3,{},whole(100000),0,5},{4,{},whole(100000),0,5}}};
    auto galaxy = take(gc::GalacticSession::create(setup));
    expect(galaxy.snapshot()->fleets().front().motion.orbit_entry > 0
        && galaxy.snapshot()->fleets().front().motion.orbit_entry < galaxy.snapshot()->fleets().back().motion.orbit_entry,
        "stock fleet order is assigned by the live campaign session");
    campaign::ConflictRecord record; record.id = 900; record.planet = setup.planets.front().id;
    record.invader = 1; record.defender = 2; record.seats = {{{1},1},{{2},2}};
    for (const auto& location : plan.locations()) if (location.planet && location.planet->id == record.planet)
        record.map_identity = authored.planet(location.original_type)->space_tactical_map;
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
        record.forces.push_back({member.id,member.type,fleet.owner,fleet.id});
    std::sort(record.forces.begin(),record.forces.end(),[](const auto& a,const auto& b){return a.id < b.id;});
    const auto resolved = take(provider->resolve(record,*galaxy.snapshot(),901));
    std::vector<sim::EntityId> expected;
    for (sim::EntityId id = 100; id < 140; ++id) expected.push_back(id);
    for (sim::EntityId id = 50; id < 65; ++id) expected.push_back(id);
    expect(resolved.conflict.tactical.campaign->reserve_order == expected,
        "stock multiple-fleet entry keeps orbit-source reserve order after deploying the last 25 squadrons");
    expect(resolved.conflict.tactical.campaign->reserves.front().id == 50,
        "stock reserve identity manifest stays sorted independently of source order");
    std::vector<std::string> reference;
    for (const auto workers : {1U,2U,4U,8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto battle = take(tt::TacticalSession::create(resolved.conflict.tactical,resolved.content));
        expect(battle.ledgers().front().pool_tokens == expected,"stock battle ledger starts with the oldest fleet's first reserve");
        const auto identity = checkpoint::tactical_identities(resolved.conflict.tactical.content_identity);
        const auto saved = take(checkpoint::save_tactical_checkpoint(battle,battle.pending_commands(),checkpoint::default_seats(resolved.conflict.tactical),identity));
        checkpoint::TacticalSessionFactory factory = [&](const tt::TacticalReplay& replay) {
            auto recorded = resolved.conflict; recorded.tactical = replay.setup;
            return tt::TacticalSession::from_replay(replay,take(provider->content(recorded)));
        };
        auto restored = take(checkpoint::load_tactical_checkpoint(saved,identity,factory,executor));
        expect(restored.session.ledgers().front().pool_tokens == expected,"stock checkpoint rebind retains the recorded reserve order");
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 2; ++tick) {
            const auto step = take(battle.step(executor)); hashes.push_back(step.state_sha256);
            expect(take(restored.session.step(executor)).state_sha256 == step.state_sha256,"stock reserve-order restore continues exactly");
        }
        if (workers == 1) reference = hashes;
        else expect(reference == hashes,"stock multiple-fleet reserve order is deterministic on 1/2/4/8 workers");
    }
}

void installed_garrison(const std::shared_ptr<const campaign::StockSpaceEntry>& provider,
    const campaign::CampaignSetupPlan& plan, const data::galactic::GalacticData& authored,
    const vfs::Vfs& files, const data::Catalog& catalog) {
    const auto binding = [&](std::string_view name) {
        const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
            [&](const auto& row) { return row.original_type == name; });
        if (at == plan.type_bindings().end()) throw std::runtime_error("garrison fixture type missing");
        return at->id;
    };
    gc::GalacticSetup setup; setup.seed = plan.galaxy().seed; setup.content_identity = plan.galaxy().content_identity;
    setup.players = plan.galaxy().players; setup.seats = plan.galaxy().seats; setup.planets = plan.galaxy().planets;
    setup.planets.front().owner = 2;
    setup.fleets = {{10, 1, setup.planets.front().id, {{100, binding("Nebulon_B_Frigate")}}},
        {11, 2, setup.planets.front().id, {{200, binding("Tartan_Patrol_Cruiser")}}}};
    setup.rules.maximum_fleet_movement_distance = whole(10000);
    setup.fleet_control.emplace(); setup.movement.emplace(); setup.movement->mode_modifier = whole(1);
    setup.fleet_control->types = {take(plan.resolve_fleet_type(binding("Nebulon_B_Frigate"))),
        take(plan.resolve_fleet_type(binding("Tartan_Patrol_Cruiser")))};
    std::sort(setup.fleet_control->types.begin(), setup.fleet_control->types.end(),
        [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto name : {"Nebulon_B_Frigate", "Tartan_Patrol_Cruiser"})
        setup.movement->types.push_back({binding(name), whole(1), {}, 0, true, false, false, false, false});
    std::sort(setup.movement->types.begin(), setup.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& player : setup.players) {
        setup.movement->players.push_back({player.player_id, whole(1), {}, false});
        for (const auto& p : setup.planets) setup.movement->access.push_back({player.player_id, p.id, true, false});
    }
    for (const auto& p : setup.planets) setup.movement->planets.push_back({p.id, {}, false});
    setup.player_economy = gc::PlayerEconomySetup{{{1, whole(10000), whole(100000), 0, 5}, {2, whole(10000), whole(100000), 0, 5},
        {3, whole(75000), whole(100000), 0, 5}, {4, {}, whole(100000), 0, 5}}};
    auto galaxy = take(gc::GalacticSession::create(setup));
    campaign::ConflictRecord record; record.id = 900; record.planet = setup.planets.front().id;
    record.invader = 1; record.defender = 2; record.station_level = 2;
    const auto planet = std::find_if(plan.locations().begin(), plan.locations().end(),
        [&](const auto& p) { return p.planet && p.planet->id == record.planet; });
    record.map_identity = authored.planet(planet->original_type)->space_tactical_map;
    record.seats = {{{1}, 1}, {{2}, 2}};
    record.forces = {{100, binding("Nebulon_B_Frigate"), 1, 10}, {200, binding("Tartan_Patrol_Cruiser"), 2, 11}};
    auto entry = take(provider->resolve(record, *galaxy.snapshot(), 901));
    const auto station = std::find_if(entry.conflict.tactical.units.begin(), entry.conflict.tactical.units.end(),
        [&](const auto& unit) { return unit.entity_id == entry.conflict.station; });
    expect(station != entry.conflict.tactical.units.end(), "GC engagement fixture has an authored station");
    if (station == entry.conflict.tactical.units.end()) return;
    const auto station_position = station->position;
    // AI-24, WGT-33: exercise the authored frigate/capital station-defence trigger.
    // Coordinates and frame bound are test inputs, not runtime policy.
    for (auto& unit : entry.conflict.tactical.units) {
        if (unit.entity_id != 100 && unit.entity_id != 200) continue;
        unit.position = station_position;
        unit.position.x = take(sim::math::add(unit.position.x, whole(unit.owner == 1 ? -4100 : 1000)));
    }
    auto destination = station_position;
    destination.x = take(sim::math::add(destination.x, whole(-1500)));
    // Use the same battle-command envelope as the campaign session contracts.
    tt::TacticalReplay command_replay; command_replay.setup.players = entry.conflict.tactical.players;
    command_replay.final_tick_count = 1;
    command_replay.commands.push_back({{0, 1, 0}, {100}, tt::MovePayload{destination}});
    const auto command_bytes = take(tt::write_replay(command_replay));
    const auto command_header = static_cast<std::size_t>(command_bytes[10]) | (static_cast<std::size_t>(command_bytes[11]) << 8U);
    const auto command_start = command_header + command_replay.setup.players.size() * 24 + 4;
    const gc::TacticalLayerCommand approach{entry.conflict.id, command_bytes[command_start + 20],
        std::vector<std::uint8_t>(command_bytes.begin() + static_cast<std::ptrdiff_t>(command_start), command_bytes.end())};
    const auto view = take(provider->presentation(entry.conflict, entry.content));
    if (!view->start) throw std::runtime_error("garrison fixture has no prepared start");
    auto control_start = *view->start; control_start.setup = entry.conflict.tactical;
    skirmish::Fixture control_fixture; control_fixture.map = entry.conflict.tactical.skirmish->map;
    control_fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}};
    const auto inputs = take(skirmish::read_start_inputs(control_fixture, files, catalog, view->tables));
    auto control = skirmish::ai_setup(control_start, inputs, view->tables);
    check(skirmish::enable_goal_system(files, control));
    const auto modules = take(skirmish::ai_modules(files, control, true));
    for (const auto& type : control.content.types) std::cout << "GARRISON_TYPE " << type.name
        << " targeting=" << type.targeting << " unit_ai=" << type.unit_ai << '\n';
    std::vector<std::string> reference;
    std::vector<std::string> defence_reference;
    // AI-21/22: compare AI context on the same resolved actors, map and ledger.
    // This control changes the AI inputs, not the campaign world's economy.
    for (const bool campaign_context : {true, false}) {
        reference.clear();
        for (const auto worker_count : {1U, 2U, 4U, 8U, 0U}) {
            const auto workers = worker_count == 0 ? 1U : worker_count;
            auto journal = worker_count == 0 ? std::shared_ptr<script::foc::AiJournal>{}
                : std::make_shared<script::foc::AiJournal>();
            auto staging = take(campaign::GameSession::create(setup));
            if (campaign_context) {
                auto ai = campaign::space_entry_script_services(provider, {}, journal);
                staging.set_child_configurator(std::move(ai.bind));
                check(staging.stage_battle(entry.conflict, entry.content, take(ai.scripts(entry.conflict))));
            } else {
                auto control_setup = control; control_setup.journal = journal;
                auto prepared = take(script::foc::prepare_session(control_setup, modules));
                staging.set_child_configurator(std::move(prepared.bind));
                check(staging.stage_battle(entry.conflict, entry.content, std::move(prepared.scripts)));
            }
            const platform::ThreadWorkerAdapter executor(workers);
            std::set<sim::EntityId> garrison_ids;
            const auto remember_garrison = [&](const tt::TacticalSession& battle) {
                for (const auto& squadron : battle.squadrons()) {
                    const auto origin = battle.squadron_state(squadron.container);
                    // WU-37, FL-07: runtime hangar launches retain their actual spawner.
                    // Produce_Force may select existing units; a high id is not provenance.
                    if (!origin || !origin->garrison_unit || origin->spawner != entry.conflict.station) continue;
                    garrison_ids.insert(squadron.container);
                    garrison_ids.insert(squadron.members.begin(), squadron.members.end());
                }
            };
            const auto entering = take(staging.step(executor));
            expect(entering.diagnostics.empty() && staging.battle(), "stock sequence stages the campaign child without diagnostics");
            if (staging.battle()) remember_garrison(*staging.battle());
            std::vector<std::string> hashes;
            bool fleet_attacked = false, spawned_attacked = false, defence_started = false, scout_took_fleet = false;
            sim::EntityId engaged_garrison{};
            std::uint64_t fleet_attack_tick{}, garrison_attack_tick{};
            for (unsigned frame = 0; frame < 1800; ++frame) {
                if (frame == 180) check(staging.submit({{staging.completed_tick(), {1}, 1}, approach}));
                const auto tick = take(staging.step(executor));
                expect(tick.diagnostics.empty(), "GC defender and admitted approach have no diagnostics");
                hashes.push_back(tick.state_hash.get());
                const auto* battle = staging.battle();
                if (!battle) break;
                remember_garrison(*battle);
                for (const auto& unit : battle->units()) {
                    const bool attacking = unit.order.kind == tt::OrderKind::attack || unit.order.kind == tt::OrderKind::attack_move;
                    if (unit.owner != 2 || !attacking || unit.order.target != 100) continue;
                    if (!fleet_attacked && unit.entity_id == 200) {
                        fleet_attacked = true; fleet_attack_tick = battle->completed_tick();
                    }
                    if (!spawned_attacked && garrison_ids.contains(unit.entity_id)) {
                        spawned_attacked = true; engaged_garrison = unit.entity_id;
                        garrison_attack_tick = battle->completed_tick();
                    }
                }
            }
            if (!journal) {
                expect(reference == hashes, "station-defence diagnostics do not change any campaign/script state hash");
                continue;
            }
            std::vector<std::string> defence_trace;
            std::size_t defence_rejections{};
            for (const auto& event : journal->plans) {
                if (event.player == 2 && event.goal == "DEFEND_SPACE_STATION" && event.event == "started") defence_started = true;
                if (event.player == 2 && event.goal == "SPACE_SCOUT" && event.event == "order"
                    && event.detail.find("[200]") != std::string::npos) scout_took_fleet = true;
                if (event.player == 2 && event.goal == "DEFEND_SPACE_STATION") {
                    defence_trace.push_back(std::to_string(event.tick) + " " + event.target + " " + event.event + " " + event.detail);
                    if (event.event == "rejected" && event.detail.starts_with("nonpositive authored desire")) {
                        ++defence_rejections;
                        expect(event.detail.find("DISTANCETONEARESTENEMY") != std::string::npos
                            && event.detail.find("FUNCTION_SHOULD_BURN_UNITS_SPACE.EVALUATE") != std::string::npos
                            && event.detail.find(" raw=") != std::string::npos && event.detail.find(" adjusted=") != std::string::npos,
                            "station-defence rejection records actual distance, burn and proposal values");
                    }
                }
                if (workers == 1) std::cout << "GARRISON_TRACE campaign=" << campaign_context << ' '
                    << event.tick << ' ' << event.player << ' '
                    << event.goal << ' ' << event.target << ' ' << event.plan << ' ' << event.event << ' ' << event.detail << '\n';
            }
            std::cout << "GARRISON_RESULT campaign=" << campaign_context << " workers=" << workers << " defence=" << defence_started
                << " fleet_attack=" << fleet_attacked << " spawned_attack=" << spawned_attacked
                << " fleet_tick=" << fleet_attack_tick << " spawned_id=" << engaged_garrison
                << " spawned_tick=" << garrison_attack_tick << " garrison_ids=" << garrison_ids.size() << '\n';
            // WAS-10, GS-04: absent resource types must not poison the burn
            // equation and block the authored station-defence proposal.
            // PE-25: the skirmish context fogs the AI's unseen cells, so the stock scouting goal takes the lone
            // free cruiser at once and it is away scouting, not behind its station. The GC run journals no
            // scouting goal, so the fleet stays and defends; the station-spawned garrison defends in both.
            if (campaign_context) expect(fleet_attacked, "fleet behind its station receives an attack within 1800 frames");
            else expect(scout_took_fleet, "skirmish context: the scouting goal journals an order for the fleet");
            if (campaign_context) expect(spawned_attacked, "station-spawned GC garrison receives an attack within 1800 frames");
            expect(defence_started, "stock GC and skirmish contexts start their authored station-defence plan");
            expect(defence_rejections > 0, "stock station-defence fixture journals its nonpositive authored desire");
            if (workers == 1) { reference = std::move(hashes); defence_reference = std::move(defence_trace); }
            else {
                expect(reference == hashes, "GC garrison engagement is deterministic on 1/2/4/8 workers");
                expect(defence_reference == defence_trace, "station-defence proposal diagnostics are deterministic on 1/2/4/8 workers");
            }
        }
    }
}

void stock_scene_relation_graphs(const data::galactic::GalacticData& data, const vfs::Vfs& files, const data::Catalog& catalog) {
    auto tables = tables_for_entry();
    auto modifier = tables.units.front(); modifier.id = "SyntheticSceneOwnerModifier";
    modifier.production.upgrade_object = true;
    std::vector<campaign::CampaignPlayerBinding> bindings;
    auto input = entry_inputs(tables); input.structures.clear(); input.riders.clear(); input.types[0].escorts.clear(); input.sides.clear();
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(bindings.size() + 1);
        bindings.push_back({{id, id, 0}, faction.name, true});
        input.sides.push_back({id, faction.name, whole(1), false});
        if (faction.benefits_from_corruption.value_or(false)) {
            if (!modifier.affiliation.empty()) modifier.affiliation += ',';
            modifier.affiliation += faction.name;
            input.upgrade_players.push_back({id, faction.name, true, {id}, {}, {modifier.id}, {}});
        }
    }
    tables.units.push_back(modifier);
    const auto relationships = take(campaign::prepare_initial_campaign_relations(data, bindings));
    for (std::size_t i = 0; i < relationships.control.size(); ++i) {
        const auto& r = relationships.control[i]; input.relations.push_back({r.player, r.other, r.allied, relationships.placement[i].enemy});
    }
    std::map<std::string, skirmish::StartInputs> maps;
    std::set<std::tuple<std::string, std::uint32_t, gc::PlayerId, gc::PlayerId>> covered;
    std::size_t references{}, refusals{}, generic_locations{}, asymmetric_cases{}, placements{};
    for (const auto& campaign_data : data.campaigns) for (const auto& location : campaign_data.planets) {
        const auto* planet = data.planet(location);
        if (!planet || planet->space_tactical_map.empty()) { ++generic_locations; continue; } // WGT-55/56 producer seam
        auto name = planet->space_tactical_map;
        if (!std::string_view(name).starts_with("Data/Art/Maps/") && !std::string_view(name).starts_with("data/art/maps/")) name = "Data/Art/Maps/" + name;
        const auto path = take(vfs::canonicalize(name));
        if (!maps.contains(path)) {
            skirmish::Fixture fixture; fixture.map = path;
            auto map = take(skirmish::read_start_inputs(fixture, files, catalog, tables));
            placements += map.placements.size(); maps.emplace(path, std::move(map));
        }
        input.map = maps.at(path);
        const auto maximum = planet->max_space_base.value_or(0);
        if (maximum < 0 || maximum > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("invalid authored space base bound in census");
        for (std::int64_t level = 0; level <= maximum; ++level) for (const auto& invader : bindings) for (const auto& defender : bindings) {
            if (invader.player.player_id == defender.player.player_id || data.faction(invader.faction)->is_neutral.value_or(false)
                || data.faction(defender.faction)->is_neutral.value_or(false)) continue;
            ++references;
            const auto state = std::tuple{path, static_cast<std::uint32_t>(level), invader.player.player_id, defender.player.player_id};
            if (!covered.insert(state).second) continue;
            campaign::ConflictRecord record; record.id = 90; record.map_identity = input.map.map;
            record.invader = invader.player.player_id; record.defender = defender.player.player_id; record.station_level = static_cast<std::uint32_t>(level);
            const auto enemy = [&](gc::PlayerId a, gc::PlayerId b) {
                const auto at = std::find_if(input.relations.begin(), input.relations.end(), [&](const auto& r) { return r.player == a && r.other == b; });
                return at != input.relations.end() && at->enemy;
            };
            asymmetric_cases += enemy(record.invader, record.defender) != enemy(record.defender, record.invader);
            const auto projected = campaign::prepare_space_entry(record, input, 100, 17);
            if (!projected) ++refusals;
            expect(static_cast<bool>(projected), "stock map relationship admission: " + campaign_data.name + " " + path
                + " " + invader.faction + " versus " + defender.faction + " level=" + std::to_string(level)
                + (projected ? "" : " " + projected.error().message));
        }
    }
    expect(!covered.empty() && placements > 0 && asymmetric_cases > 0 && refusals == 0,
        "stock admission census includes real placements, NPC/asymmetric pairs and zero refusals");
    std::cout << "WGT-123 stock admission cases=" << covered.size() << " references=" << references << " campaigns=" << data.campaigns.size()
        << " maps=" << maps.size() << " placements=" << placements << " asymmetric=" << asymmetric_cases
        << " generic_locations=" << generic_locations << " refusals=" << refusals << '\n';
}

void installed_commanders(const campaign::CampaignSetupPlan& plan, const units::GalacticTypeTable& types,
    const data::galactic::GalacticData& authored, const std::shared_ptr<const campaign::StockSpaceEntry>& provider) {
    const auto id = [&](std::string_view name) {
        for (const auto& binding : plan.type_bindings()) if (binding.original_type == name) return binding.id;
        throw std::runtime_error("missing commander fixture binding");
    };
    campaign::StartingForceInputs creation;
    creation.types = types; creation.bindings.assign(plan.type_bindings().begin(), plan.type_bindings().end());
    creation.relations = take(campaign::prepare_initial_campaign_relations(authored,plan.player_bindings())).placement;
    for (const auto& binding : creation.bindings) {
        const auto source = take(units::resolve_initial_galactic_type(types, binding.original_type));
        creation.runtime_types.push_back({binding.id, false, source.always_spawn_in_orbit.value_or(false), {}});
        if (std::find(source.behavior.begin(), source.behavior.end(), "FLEET") != source.behavior.end()) creation.fleet_type = binding.id;
        if (std::find(source.behavior.begin(), source.behavior.end(), "SQUADRON") != source.behavior.end()) creation.squadron_type = binding.id;
    }
    const auto nested = take(campaign::prepare_nested_creation(creation));
    std::vector<std::string> reference;
    for (const auto workers : {1U,2U,4U,8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        gc::GalacticSetup setup; setup.seed = 17; setup.content_identity = plan.galaxy().content_identity;
        setup.players = plan.galaxy().players; setup.seats = plan.galaxy().seats; setup.planets = plan.galaxy().planets;
        setup.planets.front().owner = 2;
        const auto planet_id = setup.planets.front().id;
        setup.fleets = {{10,1,planet_id,{{100,id("Corellian_Corvette")},{101,id("Nebulon_B_Frigate")}}}};
        setup.movement.emplace(); setup.movement->mode_modifier = whole(1); setup.fleet_control.emplace();
        gc::PoliticalControlSetup control; control.max_ground_forces = 10;
        gc::ControlPlanet presence; presence.planet = planet_id; presence.surface_accessible = true; control.planets.push_back(presence);
        std::vector<sim::EntityId> commanders;
        sim::EntityId next = 300;
        for (unsigned count = 0; count < 2; ++count) {
            // This is the exact nested-company allocator called by GC production completion.
            const auto made = take(gc::place_nested_force(nested,id("Fleet_Com_Rebel_Team"),1,planet_id,next,
                setup.fleets,*setup.movement,control));
            expect(made.fleets.size()==1 && made.landed.empty() && made.contents.size()==1,
                "stock commander production creates an orbital company transport");
            commanders.push_back(made.contents.front().children.front().id);
            setup.fleets.insert(setup.fleets.end(),made.fleets.begin(),made.fleets.end());
            setup.movement->contents.insert(setup.movement->contents.end(),made.contents.begin(),made.contents.end()); next=made.next_id;
        }
        // Production creates the companies in their own orbit before the opposing
        // fleet is introduced for the tactical admission fixture.
        setup.fleets.push_back({11,2,planet_id,{{200,id("Tartan_Patrol_Cruiser")}}});
        std::sort(setup.fleets.begin(),setup.fleets.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        for (const auto& type : nested.types) {
            const bool transport=type.role==gc::NestedCreationRole::transport;
            setup.fleet_control->types.push_back({type.type,transport,true,false});
            setup.movement->types.push_back({type.type,whole(1),{},0,true,false,false,false,transport});
        }
        for(const auto& player:setup.players) {
            setup.movement->players.push_back({player.player_id,whole(1),{},false});
            for(const auto& planet:setup.planets) setup.movement->access.push_back({player.player_id,planet.id,true,false});
        }
        for(const auto& planet:setup.planets) setup.movement->planets.push_back({planet.id,{},false});
        setup.rules.maximum_fleet_movement_distance=whole(10000);
        setup.player_economy=gc::PlayerEconomySetup{{{1,whole(10000),whole(100000),0,5},{2,whole(10000),whole(100000),0,5},
            {3,whole(10000),whole(100000),0,5},{4,{},whole(100000),0,5}}};
        auto galaxy=take(gc::GalacticSession::create(setup));
        campaign::ConflictRecord record; record.id=900; record.planet=planet_id; record.invader=1; record.defender=2;
        record.seats={{{1},1},{{2},2}};
        for(const auto& location:plan.locations()) if(location.planet && location.planet->id==planet_id)
            record.map_identity=authored.planet(location.original_type)->space_tactical_map;
        for(const auto& fleet:setup.fleets) for(const auto& member:fleet.members) record.forces.push_back({member.id,member.type,fleet.owner,fleet.id});
        std::sort(record.forces.begin(),record.forces.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        auto resolved=take(provider->resolve(record,*galaxy.snapshot(),901));
        for(const auto commander:commanders) {
            const auto force=std::find_if(resolved.conflict.forces.begin(),resolved.conflict.forces.end(),[&](const auto& row){return row.id==commander;});
            expect(force!=resolved.conflict.forces.end() && force->role==campaign::ForceRole::rider && force->carrier==101,
                "company-created stock commanders both attach to the largest initial source ship");
        }
        resolved.content.victory={}; resolved.content.motion.squadrons={};
        auto live=take(tt::TacticalSession::create(resolved.conflict.tactical,resolved.content));
        const auto live_units = live.units();
        for(const auto ship:{100U,101U}) {
            const auto unit=std::find_if(live_units.begin(),live_units.end(),[&](const auto& row){return row.entity_id==ship;});
            const auto sensor=std::find_if(resolved.content.sensors.begin(),resolved.content.sensors.end(),[&](const auto& row){return row.type_id==unit->type_id;});
            const auto instance=std::find_if(live.snapshot()->instances().begin(),live.snapshot()->instances().end(),[&](const auto& row){return row.entity_id==ship;});
            expect(instance->reveal_range==take(sim::math::multiply(sensor->reveal_range,Fixed::from_ratio(3,2).value())),
                "two stock commanders activate one authored reveal multiplier across their fleet");
            if(ship==101) expect(instance->contained.size()==2,"snapshot retains both hidden commander identities on the visible flagship");
        }
        const auto commander_type = skirmish::type_id("Generic_Fleet_Commander_Rebel");
        expect(std::any_of(resolved.content.economy.command_bonuses.begin(),resolved.content.economy.command_bonuses.end(),
            [&](const auto& profile){return profile.type==commander_type && profile.battlefield && !profile.all_allies
                && profile.bonus.percentages[4]==Fixed::from_ratio(1,4).value() && profile.bonus.stacking_category==0;}),
            "stock commander XML binds the owner-only automatic defence source in category zero");
        const auto saved=take(live.save_checkpoint_state()); auto restored=take(tt::TacticalSession::create(resolved.conflict.tactical,resolved.content));
        check(restored.load_checkpoint_state(saved));
        const auto hashes=std::vector<std::string>{take(live.step(executor)).state_sha256,take(restored.step(executor)).state_sha256};
        expect(hashes[0]==hashes[1],"stock commander sources and presentation identities restore without reattachment");
        if(reference.empty()) reference=hashes; else expect(reference==hashes,"stock company-to-battle commander path agrees on 1/2/4/8 workers");
    }
}

void installed_layer_services(const vfs::Vfs& files, const data::Catalog& catalog,
    data::galactic::GalacticData authored, const units::GalacticTypeTable& types,
    campaign::CampaignSetupOptions options) {
    auto source = *authored.campaign("Sandbox_Equal_Footing_Empire");
    source.name = "LayerServicesFixture";
    source.locations = source.planets = {"Shola", "Kamino"};
    source.trade_routes.clear();
    source.home_locations = {{"Rebel", "Shola"}, {"Empire", "Kamino"}};
    source.starting_active_player = "Rebel";
    source.starting_forces = {{"Rebel", "Shola", "R_Ground_Barracks"}, {"Empire", "Kamino", "E_Ground_Barracks"},
        {"Rebel", "Shola", "Corellian_Corvette"}, {"Empire", "Shola", "Tartan_Patrol_Cruiser"}};
    source.special_case_production.clear(); source.corruption_level_overrides.clear();
    authored.campaigns.push_back(std::move(source));
    options.seats.push_back({gc::SeatId{3}, 3, gc::SeatKind::ai, 2, true, 0});
    const auto plan = take(campaign::plan_live_campaign_setup(files, authored, types, "LayerServicesFixture", options, "Normal_Default"));
    const auto services = take(campaign::prepare_live_campaign_services(files, catalog, authored, types, plan, "Normal_Default"));
    const auto provider = take(campaign::prepare_stock_space_entry(files, catalog, authored, types, plan));
    const auto location = std::find_if(plan.locations().begin(), plan.locations().end(), [](const auto& row) {
        return row.original_type == "Shola" && row.planet;
    });
    if (location == plan.locations().end()) throw std::runtime_error("stock layer fixture has no battle location");
    for (const auto& [layer, ai_parent_queries] : std::array{
        std::pair{campaign::BattleLayer::space, false},
        std::pair{campaign::BattleLayer::land, false},
        std::pair{campaign::BattleLayer::space, true}}) {
        auto root = take(campaign::CampaignRoot::create(plan, services));
        check(campaign::configure_live_campaign_conflicts(root, files, catalog, authored, types, provider));
        auto& game = *root.game();
        const auto snapshot = game.galaxy().snapshot();
        campaign::ConflictRecord record;
        record.id = game.galaxy().next_entity_id(); record.planet = location->planet->id;
        record.invader = 1; record.defender = 2; record.seats = {{{1}, 1}, {{2}, 2}};
        record.map_identity = authored.planet("Shola")->space_tactical_map;
        for (const auto& fleet : snapshot->fleets()) if (fleet.orbit == record.planet && (fleet.owner == 1 || fleet.owner == 2))
            for (const auto& member : fleet.members) record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        expect(record.forces.size() == 2, "stock layer fixture retains both authored forces");
        auto entry = take(provider->resolve(record, *snapshot, record.id + 1));
        // Both layers formerly used the same callbacks for an already retained entry.
        entry.conflict.layer = layer;
        std::size_t parent_preparations{};
        if (ai_parent_queries) {
            check(game.add_conflict_parent_queries(campaign::BattleLayer::space,
                [&parent_preparations, id = entry.conflict.id](const campaign::ConflictRecord& context) {
                    ++parent_preparations;
                    expect(context.id == id && context.layer == campaign::BattleLayer::space,
                        "AI-on parent queries receive the stock space conflict identity");
                    return campaign::GalacticParentQuery([](std::string_view, tt::PlayerId)
                        -> std::optional<script::numeric::LuaNumber> { return {}; });
                }));
            expect(game.has_conflict_parent_queries(campaign::BattleLayer::space)
                && !game.has_conflict_parent_queries(campaign::BattleLayer::land),
                "AI-on stock fixture binds the space parent-query slot");
        }
        check(game.stage_battle(entry.conflict, entry.content));
        if (ai_parent_queries) expect(parent_preparations == 1,
            "AI-on stock space staging reaches the bound parent-query factory exactly once");
        expect(!game.battle(), "both installed layers stage their retained script host before activation");
    }
}

void installed(bool garrison_only, bool population_only, bool commander_only, bool superweapon_only) {
    std::optional<std::string> root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root || root->empty()) { std::cout << "SKIPPED: stock entry needs EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto files = take(vfs::Vfs::mount(mounts));
    const auto catalog = take(data::load_catalog(files, data::Profile::foc));
    auto authored = take(data::galactic::load_galactic_data(files, catalog.catalog));
    if (!garrison_only && !population_only && !commander_only) stock_scene_relation_graphs(authored, files, catalog.catalog);
    const auto types = units::galactic_type_table(catalog.catalog);
    data::galactic::Campaign fixture; fixture.name = "SpaceEntryFixture";
    fixture.locations = {superweapon_only ? "NalHutta" : "Shola", "Kamino"}; fixture.planets = fixture.locations; fixture.starting_active_player = "Rebel";
    fixture.starting_tech_levels = {{"Rebel", 0}, {"Empire", 0}}; fixture.max_tech_levels = {{"Rebel", 5}, {"Empire", 5}};
    authored.campaigns.push_back(fixture);
    campaign::CampaignSetupOptions options; options.seed = 17; options.local_observer = 1;
    options.players = {{{1, 1, 0}, "Rebel", true}, {{2, 2, 0}, "Empire", true}, {{3, 3, 0}, "Underworld", true},
        {{4, 4, 0}, "Neutral", true}};
    options.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    for (const auto& type : types.types) options.types.push_back({assets::object_type_crc(type.name), type.name});
    std::sort(options.types.begin(), options.types.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto plan = take(campaign::plan_campaign_setup(authored, types, fixture.name, options));
    const auto provider = take(campaign::prepare_stock_space_entry(files, catalog.catalog, authored, types, plan));
    if (commander_only) { installed_commanders(plan,types,authored,provider); return; }
    if (garrison_only) { installed_garrison(provider, plan, authored, files, catalog.catalog); return; }
    installed_reserve_order(provider, plan, authored);
    const auto type_id = [&](std::string_view name) {
        const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& row) { return row.original_type == name; });
        if (at == plan.type_bindings().end()) throw std::runtime_error("stock fixture missing binding");
        return at->id;
    };
    if (superweapon_only) {
        // #3935: a Death Star in the attacking fleet is deployed at the map's Death_Star_Position marker and keeps
        // its superweapon identity; the same battle without one deploys none.
        const auto super_name = std::string_view{"Death_Star_II"};
        for (const bool with_super : {true, false}) {
            gc::GalacticSetup initial; initial.seed = options.seed; initial.content_identity = plan.galaxy().content_identity;
            initial.players = plan.galaxy().players; initial.seats = options.seats; initial.planets = plan.galaxy().planets;
            initial.planets.front().owner = 1;
            initial.fleets = {{10, 1, initial.planets.front().id, {{100, type_id("Corellian_Corvette")}}},
                {11, 2, initial.planets.front().id, {{200, type_id("Star_Destroyer")}}}};
            if (with_super) initial.fleets.back().members.push_back({201, type_id(super_name)});
            initial.player_economy = gc::PlayerEconomySetup{{{1, whole(10000), whole(100000), 0, 5},
                {2, whole(10000), whole(100000), 0, 5}, {3, whole(10000), whole(100000), 0, 5}, {4, {}, whole(100000), 0, 5}}};
            auto galaxy = take(gc::GalacticSession::create(initial));
            campaign::ConflictRecord conflict; conflict.id = 900; conflict.planet = initial.planets.front().id;
            conflict.invader = 2; conflict.defender = 1; conflict.seats = {{{1}, 1}, {{2}, 2}};
            const auto location = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& row) {
                return row.planet && row.planet->id == conflict.planet;
            });
            conflict.map_identity = authored.planet(location->original_type)->space_tactical_map;
            for (const auto& fleet : initial.fleets) for (const auto& member : fleet.members)
                conflict.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
            std::sort(conflict.forces.begin(), conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            const auto resolved = take(provider->resolve(conflict, *galaxy.snapshot(), 901));
            const auto& units = resolved.conflict.tactical.units;
            for (const auto& unit : units) std::cout << "SUPERWEAPON_UNIT with=" << with_super << " id=" << unit.entity_id
                << " type=" << unit.type_id << " owner=" << unit.owner
                << " x=" << unit.position.x.raw() / sim::math::Fixed::scale << " y=" << unit.position.y.raw() / sim::math::Fixed::scale
                << " z=" << unit.position.z.raw() / sim::math::Fixed::scale << "\n";
            const auto super_units = std::count_if(units.begin(), units.end(), [&](const auto& unit) {
                return unit.type_id == skirmish::type_id(super_name);
            });
            expect(super_units == (with_super ? 1 : 0), with_super ? "the attacking Death Star is a battle unit" : "no Death Star in the fleet, none in the battle");
            if (with_super) {
                // TSW-01: form_space_entry repositions it from the map's planet backdrop: below the ship plane (the
                // backdrop's authored height plus the type's 1200; the installed run puts it at z -2800 on this map) and
                // facing the battlefield centre, wherever the formation put it. The rule's arithmetic is pinned by
                // skirmish_start_contracts; this proves the campaign path applies it.
                const auto unit = std::find_if(units.begin(), units.end(), [&](const auto& row) { return row.type_id == skirmish::type_id(super_name); });
                const auto scale = static_cast<double>(sim::math::Fixed::scale);
                const auto x = static_cast<double>(unit->position.x.raw()) / scale, y = static_cast<double>(unit->position.y.raw()) / scale;
                expect(unit->position.z == whole(-2800), "TSW-01: the Death Star stands at the backdrop's height plus 1200");
                const auto yaw = 2.0 * std::atan2(static_cast<double>(unit->rotation.z.raw()), static_cast<double>(unit->rotation.w.raw()));
                const auto toward_centre = std::atan2(-y, -x);
                auto turn = std::fabs(yaw - toward_centre); if (turn > 3.14159265358979) turn = 6.28318530717959 - turn;
                expect((x != 0.0 || y != 0.0) && turn < 0.01, "TSW-01: the Death Star faces the battlefield centre");
            }

            expect(resolved.conflict.superweapon == (with_super ? 201U : 0U), "the conflict names the superweapon force");
        }
        return;
    }
    if (population_only) {
        for (const auto name : {std::string_view{"Executor_Super_Star_Destroyer"}, std::string_view{"Slave_I"}}) {
            const bool executor = name == "Executor_Super_Star_Destroyer";
            const auto count = executor ? 7U : 1U;
            const auto type = type_id(name);
            gc::GalacticSetup initial; initial.seed = options.seed; initial.content_identity = plan.galaxy().content_identity;
            initial.players = plan.galaxy().players; initial.seats = options.seats; initial.planets = plan.galaxy().planets;
            initial.planets.front().owner = 1;
            initial.fleets = {{10, 1, initial.planets.front().id, {{100, type_id("Corellian_Corvette")}}},
                {11, 2, initial.planets.front().id, {}}};
            for (sim::EntityId id = 200; id < 200 + count; ++id) initial.fleets.back().members.push_back({id, type});
            if (!executor) {
                // WGT-21: the hero remains in its authored ship transport;
                // an empty galactic transport is faithfully removed at entry.
                initial.rules.maximum_fleet_movement_distance = whole(10000);
                initial.fleet_control.emplace(); initial.movement.emplace(); initial.movement->mode_modifier = whole(1);
                for (const auto member_name : {std::string_view{"Corellian_Corvette"}, std::string_view{"Slave_I"}, std::string_view{"Boba_Fett"}}) {
                    const auto member_type = type_id(member_name);
                    const bool transport = member_name == "Slave_I";
                    initial.fleet_control->types.push_back({member_type, transport, true, false});
                    initial.movement->types.push_back({member_type, whole(1), {}, 0, true, false, false, false, transport});
                }
                std::sort(initial.fleet_control->types.begin(), initial.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
                std::sort(initial.movement->types.begin(), initial.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
                for (const auto& player : initial.players) {
                    initial.movement->players.push_back({player.player_id, whole(1), {}, false});
                    for (const auto& planet : initial.planets) initial.movement->access.push_back({player.player_id, planet.id, true, false});
                }
                for (const auto& planet : initial.planets) initial.movement->planets.push_back({planet.id, {}, false});
                initial.movement->contents.push_back({200, {{250, type_id("Boba_Fett")}}});
            }
            initial.player_economy = gc::PlayerEconomySetup{{{1, whole(10000), whole(100000), 0, 5},
                {2, whole(10000), whole(100000), 0, 5}, {3, whole(10000), whole(100000), 0, 5}, {4, {}, whole(100000), 0, 5}}};
            auto galaxy = take(gc::GalacticSession::create(initial));
            campaign::ConflictRecord conflict; conflict.id = 900; conflict.planet = initial.planets.front().id;
            conflict.invader = 2; conflict.defender = 1; conflict.seats = {{{1}, 1}, {{2}, 2}};
            const auto location = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& row) {
                return row.planet && row.planet->id == conflict.planet;
            });
            conflict.map_identity = authored.planet(location->original_type)->space_tactical_map;
            for (const auto& fleet : initial.fleets) for (const auto& member : fleet.members)
                conflict.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
            std::sort(conflict.forces.begin(), conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            const auto resolved = take(provider->resolve(conflict, *galaxy.snapshot(), 901));
            const auto deployed = std::count_if(resolved.conflict.tactical.units.begin(), resolved.conflict.tactical.units.end(), [&](const auto& unit) {
                return unit.type_id == skirmish::type_id(name) && unit.owner == 2;
            });
            expect(deployed == (executor ? 6 : 1), "installed space population admits six cost-3 Executors at cap 20 and the cost-1 hero");
            if (!executor) expect(std::any_of(resolved.conflict.tactical.units.begin(), resolved.conflict.tactical.units.end(), [&](const auto& unit) {
                return unit.entity_id == 200 && unit.type_id == skirmish::type_id(name)
                    && std::any_of(unit.contained.begin(), unit.contained.end(), [](const auto& rider) { return rider.entity_id == 250; });
            }), "Boba Fett enters battle in the occupied Slave I transport used by the population contract");
            const auto& reserves = resolved.conflict.tactical.campaign->reserves;
            expect(reserves.size() == (executor ? 1U : 0U), "only the Executor surplus becomes a persistent reserve");
            if (executor) expect(reserves.size() == 1 && reserves.front().id == 200,
                "same-type GC split deploys from the back and retains the first roster identity");
            std::vector<std::string> reference;
            for (const unsigned workers : {1U, 2U, 4U, 8U}) {
                auto content = resolved.content;
                // The admission contract isolates population from combat and fog.
                content.combat = {}; content.fog.reset(); content.victory = {};
                auto battle = take(tt::TacticalSession::create(resolved.conflict.tactical, content));
                const platform::ThreadWorkerAdapter pool(workers);
                const auto view = [&]() -> const tt::EconomyView& {
                    const auto rows = battle.snapshot()->economy();
                    return *std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.player == 2; });
                };
                expect(view().population == (executor ? 18U : 1U) && view().population_cap == 20,
                    "campaign initial accounting uses space cost and excludes spawned garrisons");
                if (executor && reserves.size() == 1) {
                    const auto first = std::find_if(resolved.conflict.tactical.units.begin(), resolved.conflict.tactical.units.end(),
                        [&](const auto& unit) { return unit.type_id == skirmish::type_id(name) && unit.owner == 2; });
                    expect(first != resolved.conflict.tactical.units.end(), "installed Executor is deployed");
                    if (first == resolved.conflict.tactical.units.end()) return;
                    const auto type_crc = skirmish::type_id(name);
                    check(battle.submit({{0, 2, 0}, {}, tt::ReinforcePayload{type_crc, first->position, reserves.front().id}}));
                    static_cast<void>(take(battle.step(pool)));
                    expect(battle.ledgers()[1].pool.size() == 1 && view().population == 18,
                        "cost-3 reserve cannot consume the two remaining cap points");
                    check(battle.submit({{1, 2, 0}, {first->entity_id}, tt::DamagePayload{whole(1000000000)}}));
                    static_cast<void>(take(battle.step(pool)));
                    expect(view().population == 15, "destroyed initial Executor releases its cost-3 share");
                    std::optional<sim::math::Vec3> drop;
                    for (std::uint32_t attempt = 0; attempt < 101 && !drop; ++attempt) {
                        const auto candidate = tt::reinforcement_search_candidate(first->position, attempt, {}, content.economy.bounds);
                        if (candidate && take(battle.reinforcement_point(2, type_crc, *candidate))) drop = candidate;
                    }
                    expect(drop.has_value(), "installed reserve has a valid drop point after loss");
                    if (!drop) return;
                    check(battle.submit({{2, 2, 0}, {}, tt::ReinforcePayload{type_crc, *drop, reserves.front().id}}));
                    static_cast<void>(take(battle.step(pool)));
                    expect(battle.ledgers()[1].pool.empty() && view().population == 18,
                        "initial reserve is callable after population frees and charges the same space cost");
                } else static_cast<void>(take(battle.step(pool)));
                const auto saved = take(battle.save_checkpoint_state());
                auto restored = take(tt::TacticalSession::create(resolved.conflict.tactical, content));
                // Use the sealed executed command stream for checkpoint cursor validation.
                for (const auto& command : battle.record().commands) check(restored.submit(command));
                check(restored.load_checkpoint_state(saved));
                expect(restored.state_sha256() == battle.state_sha256(), "space population and reserves survive checkpoint restore");
                const std::vector<std::string> hashes{battle.state_sha256(), battle.snapshot()->sha256()};
                if (reference.empty()) reference = hashes;
                else expect(reference == hashes, "space population admission and recall agree on 1/2/4/8 workers");
            }
            if (executor) {
                // Source-baseline witness: BASE's loader omitted this field entirely.
                auto baseline_types = types;
                const_cast<units::GalacticType*>(baseline_types.find(name))->space_override_population_value.reset();
                const auto baseline = take(campaign::prepare_stock_space_entry(files, catalog.catalog, authored, baseline_types, plan));
                const auto before = take(baseline->resolve(conflict, *galaxy.snapshot(), 901));
                expect(std::count_if(before.conflict.forces.begin(), before.conflict.forces.end(), [&](const auto& force) {
                    return force.type == type && force.role == campaign::ForceRole::deployed;
                }) == 7, "BASE population binding deploys all seven Executors by treating the type as cost zero");
                std::cout << "SPACE_POPULATION_BASELINE Executor deployed=7 actual_population=21 cap=20; fixed deployed=6 reserve=1 population=18\n";
            }
        }
        return;
    }
    gc::GalacticSetup setup; setup.seed = options.seed; setup.content_identity = plan.galaxy().content_identity;
    setup.players = plan.galaxy().players; setup.seats = options.seats; setup.planets = plan.galaxy().planets;
    setup.planets.front().owner = 2;
    setup.fleets = {{10, 1, setup.planets.front().id, {}}, {11, 2, setup.planets.front().id, {{200, type_id("Tartan_Patrol_Cruiser")}}}};
    for (sim::EntityId id = 100; id < 140; ++id) setup.fleets.front().members.push_back({id, type_id("Rebel_X-Wing_Squadron")});
    setup.rules.maximum_fleet_movement_distance = whole(10000);
    setup.fleet_control.emplace(); setup.movement.emplace(); setup.movement->mode_modifier = whole(1);
    for (const auto name : {"Rebel_X-Wing_Squadron", "Tartan_Patrol_Cruiser", "Gallofree_Transport", "Generic_Fleet_Commander_Rebel", "Imperial_Landing_Craft", "Darth_Vader"}) {
        const auto id = type_id(name); const bool transport = std::string_view(name) == "Gallofree_Transport" || std::string_view(name) == "Imperial_Landing_Craft";
        setup.fleet_control->types.push_back({id, transport, true, false});
        setup.movement->types.push_back({id, whole(1), {}, 0, true, false, false, false, transport});
    }
    std::sort(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::sort(setup.movement->types.begin(), setup.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& player : setup.players) {
        setup.movement->players.push_back({player.player_id, whole(1), {}, false});
        for (const auto& p : setup.planets) setup.movement->access.push_back({player.player_id, p.id, true, false});
    }
    for (const auto& p : setup.planets) setup.movement->planets.push_back({p.id, {}, false});
    setup.fleets.back().members.push_back({250, type_id("Imperial_Landing_Craft")});
    setup.movement->contents.push_back({250, {{450, type_id("Darth_Vader")}}});
    for (sim::EntityId id = 300; id < 330; ++id) {
        setup.fleets.front().members.push_back({id, type_id("Gallofree_Transport")});
        setup.movement->contents.push_back({id, {{id + 100, type_id("Generic_Fleet_Commander_Rebel")}}});
    }
    // WGT-122: two authored scout companies supply eight walkers. The
    // company's XML chooses its transport and constituents, on either side.
    const auto* ground_company = types.find("Imperial_Heavy_Scout_Squad");
    expect(ground_company && !ground_company->company_units.empty() && !ground_company->company_transport.empty(),
        "installed ground-company fixture has authored transport and constituents");
    if (!ground_company) throw std::runtime_error("installed ground company is absent");
    sim::EntityId ground_id = 500;
    for (const auto owner : {1U, 2U}) for (unsigned company = 0; company < 2; ++company) {
        const sim::EntityId transport = (owner == 1 ? 330U : 251U) + company;
        setup.fleets[owner == 1 ? 0 : 1].members.push_back({transport, type_id(ground_company->company_transport)});
        gc::MovementContents contents; contents.container = transport;
        for (const auto& name : ground_company->company_units) {
            const auto id = type_id(name); contents.children.push_back({ground_id++, id});
            if (std::none_of(setup.movement->types.begin(), setup.movement->types.end(), [id](const auto& row) { return row.type == id; })) {
                setup.fleet_control->types.push_back({id, false, true, false});
                setup.movement->types.push_back({id, whole(1), {}, 0, true});
            }
        }
        setup.movement->contents.push_back(std::move(contents));
    }
    std::sort(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::sort(setup.movement->types.begin(), setup.movement->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::sort(setup.movement->contents.begin(), setup.movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    setup.player_economy = gc::PlayerEconomySetup{{{1, whole(10000), whole(100000), 0, 5}, {2, whole(10000), whole(100000), 0, 5},
        {3, whole(75000), whole(100000), 0, 5}, {4, {}, whole(100000), 0, 5}}};
    auto galaxy = take(gc::GalacticSession::create(setup));
    campaign::ConflictRecord record; record.id = 900; record.planet = setup.planets.front().id;
    record.invader = 1; record.defender = 2; record.station_level = 2;
    const auto planet = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& p) { return p.planet && p.planet->id == record.planet; });
    record.map_identity = authored.planet(planet->original_type)->space_tactical_map;
    record.seats = {{{1}, 1}, {{2}, 2}};
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members) record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
    std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    expect(provider->available(record), "an unreachable story rocket prototype does not disable supported battles");
    auto unsupported = record;
    unsupported.forces.push_back({800, type_id("UM06_EclipseDefensePad"), 2, 11});
    const auto availability = provider->availability(unsupported);
    expect(!availability && availability.error().code == tt::diagnostic_codes::invalid_setup
        && availability.error().message.find("RFL-03/05, #1760") != std::string::npos && !provider->available(unsupported),
        "availability returns the cached reachable rocket refusal with its native diagnostic code");
    const auto refused = provider->resolve(unsupported, *galaxy.snapshot(), 901);
    expect(!refused && refused.error().message.find("RFL-03/05, #1760") != std::string::npos,
        "a battle that can spawn the unsupported rocket refuses through the named flight seam");
    auto resolved = take(provider->resolve(record, *galaxy.snapshot(), 901));
    expect(std::count_if(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [](const auto& force) {
        return force.id >= 500 && force.id < 516 && force.role == campaign::ForceRole::rider;
    }) == 8, "stock defender's eight ground constituents enter inside two XML transports");
    expect(std::count_if(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [](const auto& force) {
        return force.id >= 500 && force.id < 516 && force.role == campaign::ForceRole::reinforcement && force.carrier;
    }) == 8, "stock over-cap ground companies preserve eight contained reserve identities");
    const auto ground_view = take(provider->presentation(resolved.conflict, resolved.content));
    for (const auto& name : ground_company->company_units)
        expect(!ground_view->tables.find(name), "ordinary ground members do not load spatial combat profiles");
    const auto commander = std::find_if(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [](const auto& force) { return force.id == 400; });
    const auto first_squadron = std::find_if(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [&](const auto& force) {
        return force.type == type_id("Rebel_X-Wing_Squadron") && force.role == campaign::ForceRole::deployed;
    });
    expect(first_squadron != resolved.conflict.forces.end() && commander != resolved.conflict.forces.end()
        && commander->role == campaign::ForceRole::rider && commander->carrier == first_squadron->id,
        "stock squadron container has its own positive source health and the first admitted tied squadron becomes flagship");
    expect(resolved.conflict.tactical.skirmish && resolved.conflict.station && resolved.conflict.station_level == 2,
        "stock authored planet map creates current faction station");
    expect(std::any_of(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [](const auto& f) { return f.role == campaign::ForceRole::reinforcement; }),
        "stock faction cap leaves over-cap persistent reserves");
    expect(resolved.conflict.tactical.campaign && std::any_of(resolved.conflict.tactical.campaign->reserves.begin(), resolved.conflict.tactical.campaign->reserves.end(), [](const auto& r) {
        return !r.contained.empty() && r.contained.front().entity_id == r.id + 100 && r.contained.front().parent == r.id;
    }), "stock over-cap transport preserves its exact passenger tree in the reserve binding");
    const auto unique_rider = std::find_if(resolved.conflict.forces.begin(), resolved.conflict.forces.end(), [](const auto& force) { return force.id == 450; });
    expect(unique_rider != resolved.conflict.forces.end() && unique_rider->role == campaign::ForceRole::rider
        && std::binary_search(resolved.conflict.tactical.campaign->population_registered.begin(),
            resolved.conflict.tactical.campaign->population_registered.end(), unique_rider->carrier),
        "stock unique passenger ship registers population with its temporary identity");
    const auto companion_view = take(provider->presentation(resolved.conflict, resolved.content));
    const auto companion = std::find_if(resolved.conflict.tactical.units.begin(), resolved.conflict.tactical.units.end(), [&](const auto& unit) {
        return unique_rider != resolved.conflict.forces.end() && unit.entity_id == unique_rider->carrier;
    });
    const auto carrier = std::find_if(companion_view->tables.units.begin(), companion_view->tables.units.end(), [&](const auto& type) {
        return companion != resolved.conflict.tactical.units.end() && skirmish::type_id(type.id) == companion->type_id;
    });
    const auto* vader_team = carrier == companion_view->tables.units.end() ? nullptr
        : companion_view->tables.find(carrier->replenish_team);
    expect(vader_team && vader_team->lua_script == "ObjectScript_VaderTIESquadron",
        "stock Rebel entry retains the scripted team Vader's carrier can create: "
            + (carrier == companion_view->tables.units.end() ? std::string("missing carrier") : carrier->id + "/" + carrier->replenish_team));
    if (vader_team && !vader_team->lua_script.empty()) {
        auto manifest_proof = take(provider->prepare_ai(resolved.conflict));
        check(manifest_proof.scripts.create_instance(42, "Data/Scripts/GameObject/" + vader_team->lua_script + ".lua"));
    }
    installed_layer_services(files, catalog.catalog, authored, types, options);
    auto staging = take(campaign::GameSession::create(setup));
    auto ai = campaign::space_entry_script_services(provider);
    staging.set_child_configurator(std::move(ai.bind));
    check(staging.stage_battle(resolved.conflict, resolved.content, take(ai.scripts(resolved.conflict))));
    const platform::ThreadWorkerAdapter executor(1);
    const auto first_tick = take(staging.step(executor));
    expect(first_tick.diagnostics.empty(), "stock Rebel entry against an Empire fleet carrying Vader stages and steps without script diagnostics");
    const auto reference_hash = first_tick.state_hash.get();
    expect(take(provider->content(resolved.conflict)).durability == resolved.content.durability,
        "stock recorded child rebinds exact content");
    const auto hash_before_presentation = staging.state_sha256();
    const auto view = take(provider->presentation(resolved.conflict, resolved.content));
    expect(view->setup == resolved.conflict.tactical, "presentation receives exact immutable resolved setup");
    expect(view->start && view->start->setup == resolved.conflict.tactical && !view->start->markers.empty(),
        "first presentation copies already prepared native start metadata");
    const auto fresh_provider = take(campaign::prepare_stock_space_entry(files, catalog.catalog, authored, types, plan));
    const auto fresh_view = take(fresh_provider->presentation(resolved.conflict, resolved.content));
    expect(fresh_view->start && fresh_view->start->setup == resolved.conflict.tactical
        && fresh_view->start->markers.empty() && fresh_view->start->units.size() == resolved.conflict.tactical.units.size(),
        "fresh provider projects exact retained actors without inventing unsaved fleet markers");
    expect(staging.state_sha256() == hash_before_presentation,
        "presentation metadata creation leaves the authoritative state hash unchanged");
    expect(!view->tables.find("UM06_EclipseDefensePad"), "unreachable story rocket is absent from this battle's table");
    // The campaign subset keeps a type's retreat self-destruct explosion with the type (star bases author one).
    for (const auto& type : view->tables.units) {
        if (type.retreat_self_destruct.empty()) continue;
        expect(type.retreat_self_destruct_index < view->tables.projectiles.size()
            && view->tables.projectiles[type.retreat_self_destruct_index].id == type.retreat_self_destruct,
            "campaign battle table resolves " + type.id + " retreat self-destruct explosion");
    }
    const auto* squadron = view->tables.find("Rebel_X-Wing_Squadron");
    expect(view->tables.find("Tartan_Patrol_Cruiser") && squadron && !squadron->members.empty()
        && std::all_of(squadron->members.begin(), squadron->members.end(), [&](const auto& member) { return view->tables.find(member.craft); }),
        "battle closure retains the participating cruiser and transitively spawned squadron craft");
    if (view->start) {
        skirmish::StartInputs ai_inputs; ai_inputs.tables = &view->tables;
        const auto ai_table = skirmish::ai_setup(*view->start, ai_inputs, view->tables);
        const auto ship = std::find_if(ai_table.content.types.begin(), ai_table.content.types.end(),
            [](const auto& type) { return type.name == "TARTAN_PATROL_CRUISER"; });
        const auto* tactical_ship = view->tables.find("Tartan_Patrol_Cruiser");
        expect(tactical_ship && tactical_ship->unit_ai && tactical_ship->targeting
            && ship != ai_table.content.types.end() && ship->unit_ai && ship->targeting,
            "stock GC tactical battle table retains UnitAI tracking and targeting for its cruiser");
    }
    for (const auto workers : {2U, 4U, 8U}) {
        const auto repeated = take(provider->resolve(record, *galaxy.snapshot(), 901));
        const auto repeated_view = take(provider->presentation(repeated.conflict, repeated.content));
        expect(repeated.conflict == resolved.conflict
            && units::canonical_encoding(repeated_view->tables) == units::canonical_encoding(view->tables),
            "the same record produces identical projected indices and canonical content");
        auto replay_stage = take(campaign::GameSession::create(setup));
        auto scripts = campaign::space_entry_script_services(provider);
        replay_stage.set_child_configurator(std::move(scripts.bind));
        check(replay_stage.stage_battle(repeated.conflict, repeated.content, take(scripts.scripts(repeated.conflict))));
        expect(take(replay_stage.step(platform::ThreadWorkerAdapter(workers))).state_hash.get() == reference_hash,
            "stock projected content stages and steps identically on 1/2/4/8 workers");
    }
    auto plain_record = record;
    plain_record.forces.erase(std::remove_if(plain_record.forces.begin(), plain_record.forces.end(),
        [](const auto& force) { return force.id == 250; }), plain_record.forces.end());
    auto plain_setup = setup;
    auto& plain_defenders = plain_setup.fleets.back().members;
    plain_defenders.erase(std::remove_if(plain_defenders.begin(), plain_defenders.end(),
        [](const auto& member) { return member.id == 250; }), plain_defenders.end());
    auto& plain_contents = plain_setup.movement->contents;
    plain_contents.erase(std::remove_if(plain_contents.begin(), plain_contents.end(),
        [](const auto& contents) { return contents.container == 250; }), plain_contents.end());
    const auto plain_galaxy = take(gc::GalacticSession::create(plain_setup));
    auto plain_entry = take(provider->resolve(plain_record, *plain_galaxy.snapshot(), 902));
    auto plain_stage = take(campaign::GameSession::create(plain_setup));
    auto plain_scripts = campaign::space_entry_script_services(provider);
    plain_stage.set_child_configurator(std::move(plain_scripts.bind));
    check(plain_stage.stage_battle(plain_entry.conflict, plain_entry.content, take(plain_scripts.scripts(plain_entry.conflict))));
    const auto plain_tick = take(plain_stage.step(platform::ThreadWorkerAdapter(1)));
    expect(plain_tick.diagnostics.empty(), "stock Rebel entry on a plain planet stages and steps without attachment diagnostics");
    // WGP-188: three separate planet entitlements, including a noncombatant owner.
    auto corrupted = record;
    corrupted.upgrades = {{type_id("Generic_Racketeering_Corruption_Upgrade"), record.planet, 1},
        {type_id("Generic_Racketeering_Corruption_Upgrade"), record.planet, 2},
        {type_id("Underworld_Racketeering_Corruption_Upgrade"), record.planet, 3}};
    auto corrupted_entry = take(provider->resolve(corrupted, *galaxy.snapshot(), 901));
    const auto third_owner = std::find_if(corrupted_entry.conflict.tactical.players.begin(), corrupted_entry.conflict.tactical.players.end(),
        [](const auto& p) { return p.player_id == 3; });
    expect(third_owner != corrupted_entry.conflict.tactical.players.end() && third_owner->team_id == 2 && !third_owner->commandable(),
        "WGT-123: stock nonparticipant corruption owner uses the third hostile relation class and no command seat");
    expect(corrupted_entry.conflict.upgrades == corrupted.upgrades && corrupted_entry.conflict.tactical.campaign
        && corrupted_entry.conflict.tactical.campaign->upgrades.size() == resolved.conflict.tactical.campaign->upgrades.size() + 3,
        "stock corrupted planet creates every owner's separate tactical corruption object");
    for (const auto& upgrade : corrupted_entry.conflict.tactical.campaign->upgrades) {
        if (upgrade.type != type_id("Generic_Racketeering_Corruption_Upgrade")
            && upgrade.type != type_id("Underworld_Racketeering_Corruption_Upgrade")) continue;
        const auto object = std::find_if(corrupted_entry.conflict.tactical.units.begin(), corrupted_entry.conflict.tactical.units.end(),
            [&](const auto& unit) { return unit.entity_id == upgrade.holder; });
        expect(object != corrupted_entry.conflict.tactical.units.end() && object->owner == upgrade.owner
            && object->type_id == upgrade.type && object->position == sim::math::Vec3{}
            && object->rotation == sim::math::identity_quat(),
            "corruption objects use their authored type, owning player and the origin");
        expect(std::none_of(corrupted_entry.conflict.forces.begin(), corrupted_entry.conflict.forces.end(),
            [&](const auto& force) { return force.id == upgrade.holder; }), "planet modifier objects never become fleet forces");
    }
    const auto* salvage = corrupted_entry.content.economy.upgrade(type_id("Underworld_Racketeering_Corruption_Upgrade"));
    expect(salvage && salvage->bounties.size() == 1 && salvage->bounties.front().percentage == whole(1),
        "stock salvage ability is loaded with its authored percentage");
    std::vector<tt::FactionId> salvage_targets;
    for (const auto name : {"Empire", "Rebel", "Hutts", "Pirates"}) salvage_targets.push_back(skirmish::faction_id(name));
    std::sort(salvage_targets.begin(), salvage_targets.end());
    expect(salvage && salvage->bounties.size() == 1 && salvage->bounties.front().factions == salvage_targets
        && !salvage->bounties.front().all_allies, "stock salvage parses its target factions and disabled allied propagation");
    const auto corruption_view = take(provider->presentation(corrupted_entry.conflict, corrupted_entry.content));
    auto authored_bounty = corruption_view->tables;
    authored_bounty.units.front().category_bits = 3;
    authored_bounty.units.front().bounty_sp = whole(0);
    authored_bounty.units.front().bounty_mp = whole(-1);
    authored_bounty.constants.bounty_sp = {{1, whole(7)}, {2, whole(11)}};
    authored_bounty.constants.bounty_mp = {{1, whole(23)}, {2, whole(19)}};
    auto bounty_inputs = synthetic_inputs(authored_bounty);
    const auto bounty_type = skirmish::type_id(authored_bounty.units.front().id);
    const auto bounty_amount = [&](const auto& rules) {
        const auto row = std::find_if(rules.bounty_values.begin(), rules.bounty_values.end(),
            [&](const auto& value) { return value.first == bounty_type; });
        if (row == rules.bounty_values.end()) throw std::runtime_error("bounty type missing");
        return row->second;
    };
    expect(bounty_amount(take(skirmish::economy_rules(*corruption_view->start, bounty_inputs, authored_bounty, true))) == whole(0),
        "zero single-player override wins over authored category defaults");
    authored_bounty.units.front().bounty_sp = whole(-1);
    expect(bounty_amount(take(skirmish::economy_rules(*corruption_view->start, bounty_inputs, authored_bounty, true))) == whole(11),
        "negative single-player override selects the maximum applicable authored single-player category");
    authored_bounty.campaign_multiplayer = true;
    expect(bounty_amount(take(skirmish::economy_rules(*corruption_view->start, bounty_inputs, authored_bounty, true))) == whole(23),
        "negative multiplayer override selects the maximum applicable authored multiplayer category");
    std::string corruption_hash;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        auto corrupted_stage = take(campaign::GameSession::create(setup));
        auto corrupted_ai = campaign::space_entry_script_services(provider);
        corrupted_stage.set_child_configurator(std::move(corrupted_ai.bind));
        check(corrupted_stage.stage_battle(corrupted_entry.conflict, corrupted_entry.content, take(corrupted_ai.scripts(corrupted_entry.conflict))));
        const auto corrupt_tick = take(corrupted_stage.step(platform::ThreadWorkerAdapter(workers)));
        expect(corrupt_tick.diagnostics.empty(), "corrupted stock space battle stages and steps without script diagnostics");
        if (corruption_hash.empty()) corruption_hash = corrupt_tick.state_hash.get();
        else expect(corruption_hash == corrupt_tick.state_hash.get(), "installed corruption objects are deterministic on 1/2/4/8 workers");
    }
    auto corruption_battle = take(tt::TacticalSession::create(corrupted_entry.conflict.tactical, corrupted_entry.content));
    campaign::BattleResult corruption_return;
    check(provider->decorate_result(corrupted_entry.conflict, corruption_battle, corruption_return));
    expect(corruption_return.upgrades == corrupted.upgrades, "child return preserves planet entitlements without recording temporary modifier holders");
    const auto background = std::find_if(corruption_return.credits.begin(), corruption_return.credits.end(), [](const auto& row) { return row.player == 3; });
    expect(background != corruption_return.credits.end() && background->credits == whole(75000), "nonparticipant modifier owner retains its real treasury above the combatant cap");
    // Retain the full owner-reported regression: all nine authored types, for all
    // three owners. Inapplicable entries survive once without a temporary holder.
    auto all_corruption = record;
    std::set<tt::TypeId> corruption_types;
    for (const auto name : {"Underworld_Racketeering_Corruption_Upgrade", "Generic_Racketeering_Corruption_Upgrade",
        "Generic_Intimidation_Corruption_Upgrade", "Generic_Piracy_Corruption_Upgrade", "Generic_Kidnapping_Corruption_Upgrade",
        "Generic_Corrupt_Militia_Corruption_Upgrade", "Generic_Bribery_Corruption_Upgrade", "Generic_Black_Market_Corruption_Upgrade",
        "Generic_Bonded_Citizens_Corruption_Upgrade"}) {
        const auto type = type_id(name); corruption_types.insert(type);
        for (const gc::PlayerId owner : {1U, 2U, 3U}) all_corruption.upgrades.push_back({type, record.planet, owner});
    }
    std::sort(all_corruption.upgrades.begin(), all_corruption.upgrades.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.owner, a.holder, a.type} < std::tuple{b.owner, b.holder, b.type};
    });
    const auto all_entry = take(provider->resolve(all_corruption, *galaxy.snapshot(), 901));
    const auto all_view = take(provider->presentation(all_entry.conflict, all_entry.content));
    using OriginCounts = std::map<std::pair<gc::PlayerId, tt::TypeId>, unsigned>;
    OriginCounts expected_origins;
    const std::array<std::string_view, 4> affiliations{"", "Rebel", "Empire", "Underworld"};
    for (const auto& upgrade : all_corruption.upgrades) {
        const auto type = std::find_if(all_view->tables.units.begin(), all_view->tables.units.end(), [&](const auto& row) {
            return skirmish::type_id(row.id) == upgrade.type;
        });
        if (type == all_view->tables.units.end() || !type->production.upgrade_object) continue;
        auto names = type->affiliation; std::replace(names.begin(), names.end(), ',', ' ');
        std::istringstream stream(names); std::string name;
        while (stream >> name) if (name == affiliations.at(upgrade.owner)) { ++expected_origins[{upgrade.owner, upgrade.type}]; break; }
    }
    const auto origin_counts = [&](const auto& entry) {
        OriginCounts counts;
        for (const auto& upgrade : entry.conflict.tactical.campaign->upgrades) if (corruption_types.contains(upgrade.type)) {
            const auto object = std::find_if(entry.conflict.tactical.units.begin(), entry.conflict.tactical.units.end(), [&](const auto& unit) {
                return unit.entity_id == upgrade.holder && unit.type_id == upgrade.type && unit.owner == upgrade.owner;
            });
            if (object != entry.conflict.tactical.units.end()) ++counts[{upgrade.owner, upgrade.type}];
        }
        return counts;
    };
    expect(!expected_origins.empty() && origin_counts(all_entry) == expected_origins,
        "nine-type corruption regression creates one origin object per affiliated upgrade entry");
    auto all_battle = take(tt::TacticalSession::create(all_entry.conflict.tactical, all_entry.content));
    campaign::BattleResult all_return; check(provider->decorate_result(all_entry.conflict, all_battle, all_return));
    expect(all_return.upgrades == all_corruption.upgrades, "all nine types survive return exactly once, including uninstantiated entries");
    auto repeated_record = record; repeated_record.upgrades = all_return.upgrades;
    const auto repeated_entry = take(provider->resolve(repeated_record, *galaxy.snapshot(), *all_entry.conflict.tactical.first_free_entity_id));
    auto repeated_battle = take(tt::TacticalSession::create(repeated_entry.conflict.tactical, repeated_entry.content));
    campaign::BattleResult repeated_return; check(provider->decorate_result(repeated_entry.conflict, repeated_battle, repeated_return));
    expect(repeated_return.upgrades == all_return.upgrades && origin_counts(repeated_entry) == expected_origins,
        "resolve/return/reentry/return never grows the planet list or duplicates origin objects");
    gateway_route(files, catalog.catalog, authored, types);
}
} // namespace space_entry_test

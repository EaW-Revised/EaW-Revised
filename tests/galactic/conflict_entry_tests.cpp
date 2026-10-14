#include "galactic_support.hpp"
#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/campaign/standalone_replay.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/scene/scene.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace {
using namespace eawr;
namespace tactical = sim::tactical;
using galactic_test::expect;
using sim::math::Fixed;
Fixed whole(const std::int64_t n) { return Fixed::from_integer(n).value(); }

units::UnitTables synthetic_tables() {
    units::UnitTables tables;
    for (const auto tag : {"Object_Max_Health_Multiplier_Space", "Hull_Vs_Hard_Points_Health_Constraint",
        "Engines_Disabled_Speed_Modifier", "Health_Low_Percent_Threshold", "ShieldRechargeIntervalInSecs",
        "Depleted_Shield_Disable_Time", "Depleted_Shield_Damage_Increment", "Depleted_Shield_Regen_Cap",
        "EnergyRechargeIntervalInSecs", "EnergyToShieldExchangeRate", "Object_Max_Speed_Multiplier_Space",
        "XYExpansionDistanceSpace", "TurnInPlaceSlowdownCorvette", "TurnInPlaceSlowdownFrigate", "TurnInPlaceSlowdownCapital"}) {
        units::NamedConstant constant; constant.tag = tag; constant.value = whole(1);
        tables.constants.scalars.push_back(constant);
    }
    units::NamedConstant rotations; rotations.tag = "MaxRotationsSpace"; rotations.value = whole(8);
    tables.constants.scalars.push_back(rotations);
    for (const auto& [name, kind] : {std::pair{"Ship", units::UnitKind::ship}, {"Station", units::UnitKind::station},
        {"Squadron", units::UnitKind::squadron}, {"Craft", units::UnitKind::craft}}) {
        units::UnitType type; type.id = name; type.kind = kind; type.victory_relevant = true;
        if (kind != units::UnitKind::squadron) type.hull = whole(100);
        if (kind == units::UnitKind::ship || kind == units::UnitKind::craft) {
            type.living_projectile_collision = true;
            type.movement.max_speed = whole(2); type.movement.min_speed = whole(1);
            type.movement.max_rate_of_turn = whole(1); type.movement.max_lift = whole(1);
            type.movement.space_layer = "Corvette";
        }
        if (kind == units::UnitKind::squadron) { type.members = {{"Craft", 0, {}}, {"Craft", 0, {}}}; }
        tables.units.push_back(type);
    }
    auto reserve = tables.units.front(); reserve.id = "ReserveShip"; tables.units.push_back(reserve);
    units::UnitType pad; pad.id = "Pad"; pad.kind = units::UnitKind::station;
    pad.hull = whole(100); pad.capture_point = true; pad.build_pad = true;
    pad.capture_radius = whole(100); pad.capture_seconds = whole(1);
    pad.affiliation = "Rebel, Empire";
    tables.units.push_back(pad);
    return tables;
}
skirmish::MapPlacement marker(const std::uint32_t ordinal, std::string name, const std::int64_t x) {
    skirmish::MapPlacement p; p.record = ordinal; p.type = std::move(name); p.marker = true;
    p.position = sim::math::Vec3{whole(x), {}, {}}; p.orientation_degrees = sim::math::Vec3{};
    return p;
}
skirmish::StartInputs synthetic_inputs(const units::UnitTables& tables) {
    skirmish::StartInputs inputs; inputs.tables = &tables;
    inputs.map = "data/art/maps/conflict-test.ted"; inputs.map_sha256 = std::string(64, 'a');
    inputs.factions = {{"Rebel", true, false, false, {}}, {"Empire", true, false, false, {}},
        {"Neutral", false, true, true, {}}};
    // Deliberately no lobby palette or Team_NN markers: selection owns the entry points.
    inputs.placements = {marker(1, "AttackerPoint", -1000),
        marker(2, "DefenderPoint", 1000), marker(3, "StationPoint", 1200)};
    skirmish::MapPlacement map; map.record = 4; map.type = "Pad"; map.element = "SpaceStructure";
    map.owner_index = 2; map.position = sim::math::Vec3{whole(1000), {}, {}}; map.orientation_degrees = sim::math::Vec3{};
    inputs.placements.push_back(map);
    return inputs;
}
skirmish::Fixture fixture_for(const skirmish::StartInputs& inputs) {
    skirmish::Fixture fixture; fixture.map = inputs.map; fixture.map_sha256 = inputs.map_sha256;
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}};
    fixture.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    return fixture;
}
void native_content_selection() {
    auto raw = synthetic_tables(); raw.campaign_scene = true; raw.campaign_multiplayer = true;
    raw.units[0].id = "Renamed_Carrier";
    raw.units[2].id = "Renamed_Squadron"; raw.units[2].members[0].craft = "Renamed_Craft";
    raw.units[2].members[1].craft = "Renamed_Craft";
    for (auto& member : raw.units[2].members) { member.craft_index = 3; }
    raw.units[3].id = "Renamed_Craft";
    raw.units[0].production.buildable_campaign = {{"Mod_Faction", {"Renamed_Squadron"}}};
    units::LoadInput original; original.types = {"Renamed_Carrier", "Unrelated_Prototype"};
    original.obstacles = {"Renamed_Prop"};
    auto scene = campaign::native_scene_selection(original, raw);
    scene.battle_types = {"Renamed_Carrier"};
    const auto narrowed = campaign::native_scene_tables(raw, scene, {});
    expect(narrowed && narrowed.value().units.size() == 3
        && narrowed.value().find("Renamed_Carrier") && narrowed.value().find("Renamed_Squadron")
        && narrowed.value().find("Renamed_Craft") && !narrowed.value().find("Station"),
        "native selection follows renamed campaign production and squadron references without FoC type lists");
    if (!narrowed) return;
    expect(narrowed.value().constants == raw.constants && narrowed.value().campaign_multiplayer,
        "native narrowing retains original global constants and multiplayer campaign mode");
    const auto exact = campaign::native_scene_selection(original, narrowed.value());
    expect(exact.source_types == original.types && exact.source_obstacles == original.obstacles,
        "native header keeps original loader inputs independently of the reachable battle subset");
    const auto again = campaign::native_scene_tables(raw, exact, {});
    expect(again && units::content_identity(again.value()) == units::content_identity(narrowed.value()),
        "recorded exact roots reconstruct the same canonical campaign content identity");
    raw.campaign_scene = false;
    expect(!campaign::native_scene_tables(raw, exact, {}), "native replay cannot bind skirmish-mode profiles");
    raw.campaign_scene = true; raw.campaign_multiplayer = false;
    expect(!campaign::native_scene_tables(raw, exact, {}), "native replay rejects the wrong campaign multiplayer mode");
}

void queued_pending_alerts() {
    namespace gc = sim::galactic;
    const auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    std::array<campaign::ConflictRecord, 2> conflicts;
    for (std::size_t i = 0; i < conflicts.size(); ++i) {
        auto& conflict = conflicts[i];
        conflict.id = 100 + i;
        conflict.planet = setup.planets[i].id;
        conflict.invader = i == 0 ? 1U : 2U;
        conflict.defender = i == 0 ? 2U : 1U;
        conflict.layer = i == 0 ? campaign::BattleLayer::space : campaign::BattleLayer::land;
        conflict.tactical.first_free_entity_id = conflict.id + 1;
        conflict.tactical.content_identity = setup.content_identity;
    }
    auto services = std::make_shared<platform::CampaignServiceConfiguration>();
    services->configure = [conflicts](campaign::GameSession& game) {
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return core::Result<std::optional<campaign::RecordedAdmission>>::success({});
        }, [](const auto&) {
            return core::Result<tactical::TacticalContent>::success({});
        });
        game.set_auto_resolver([](const auto& conflict, const auto&, const auto&) {
            campaign::AutoResolveOutcomeApplication outcome;
            outcome.planet = conflict.planet;
            outcome.planet_type = "FixturePlanet";
            outcome.invader = conflict.invader; outcome.defender = conflict.defender;
            outcome.land = conflict.layer == campaign::BattleLayer::land;
            outcome.result.conflict = conflict.id; outcome.result.winner = conflict.defender;
            return core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success(outcome);
        });
        campaign::AutoResolveOutcomeServices outcomes;
        outcomes.control = [](auto&, const auto&) { return core::Result<void>::success(); };
        game.set_auto_resolve_outcome_services(std::move(outcomes));
        campaign::Arbitration arbitration; arbitration.route = campaign::ConflictRoute::instant_auto;
        for (const auto& conflict : conflicts)
            if (auto queued = game.queue_conflict(conflict, arbitration); !queued) return queued;
        return core::Result<void>::success();
    };
    std::vector<std::string> reference;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto game = campaign::GameSession::create(setup).value();
        expect(bool(services->configure(game)), "two conflicts queue before the first conflict retires");
        std::size_t emitted = 0;
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 8; ++tick) {
            auto step = game.step(executor);
            expect(bool(step), "queued automatic conflicts step successfully");
            if (!step) { std::cerr << step.error().message << '\n'; return; }
            hashes.push_back(step.value().state_hash.get());
            for (const auto& event : step.value().conflict_events) {
                if (event.kind != gc::GalacticEventKind::conflict_pending) continue;
                const auto found = step.value().pending_alerts.find(event.subject);
                expect(found != step.value().pending_alerts.end(), "GameTick retains every emitted conflict alert");
                if (found != step.value().pending_alerts.end() && emitted < conflicts.size()) {
                    const auto& conflict = conflicts[emitted];
                    expect(event.subject == conflict.id && found->second.invader == conflict.invader
                        && found->second.defender == conflict.defender
                        && found->second.land == (conflict.layer == campaign::BattleLayer::land),
                        "alert context follows the emitted subject through automatic retirement");
                }
                ++emitted;
            }
        }
        expect(emitted == 2 && !game.pending_conflict(), "both queued conflicts emit once and retire");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "queued conflict hashes match on 1/2/4/8 workers");
        platform::LiveGameSession::Options options;
        options.workers = workers; options.pacing = platform::LiveGameSession::Pacing::driven;
        options.services = services;
        auto started = platform::LiveGameSession::start(setup, options);
        expect(bool(started), "queued conflict publication owner starts");
        if (!started) return;
        auto live = std::move(started).value();
        live->advance_to(8);
        expect(live->wait_for(8, std::chrono::seconds(10)), "both queued alerts reach publication");
        const auto publication = live->galactic_events_after(0);
        std::size_t published = 0;
        for (const auto& row : publication.events) {
            if (row.event.kind != gc::GalacticEventKind::conflict_pending) continue;
            expect(row.pending_alert.has_value(), "publisher attaches metadata for both queued subjects");
            if (row.pending_alert && published < conflicts.size()) {
                const auto& conflict = conflicts[published];
                expect(row.event.subject == conflict.id && row.pending_alert->invader == conflict.invader
                    && row.pending_alert->defender == conflict.defender
                    && row.pending_alert->land == (conflict.layer == campaign::BattleLayer::land),
                    "published queued alert retains its own players and layer");
            }
            ++published;
        }
        expect(published == 2 && live->galactic_events_after(publication.through).events.empty(),
            "both pending alerts publish exactly once with exclusive cursors");
        expect(live->tick_hashes() == reference, "live queued conflict hashes equal direct 1/2/4/8 results");
        live->stop();
    }
}
void contracts() {
    const auto tables = synthetic_tables();
    const auto inputs = synthetic_inputs(tables);
    const auto fixture = fixture_for(inputs);
    campaign::ConflictRecord record; record.id = 90; record.invader = 1; record.defender = 2;
    record.map_identity = inputs.map; record.station = 45; record.station_type = skirmish::type_id("Station");
    record.forces = {{7, skirmish::type_id("Squadron"), 1}, {21, skirmish::type_id("Ship"), 2},
        {45, record.station_type, 2, 0, campaign::ForceRole::escort},
        {79, skirmish::type_id("ReserveShip"), 1, 0, campaign::ForceRole::reinforcement}};
    const std::vector<campaign::ForcePlacement> placements{{7, 1}, {21, 2}, {45, 3}};
    auto built = campaign::build_conflict_entry(record, fixture, inputs, placements, 100, 11);
    expect(static_cast<bool>(built), "selected sparse conflict builds");
    if (!built) { std::cerr << built.error().message << '\n'; return; }
    const auto& entry = built.value();
    expect(entry.conflict.tactical.seed == campaign::child_seed(11, 90), "seed derives from conflict identity");
    expect(entry.conflict.forces == record.forces && record.tactical.units.empty(), "record is copied without mutating admission inputs");
    const auto& setup = entry.conflict.tactical;
    expect(setup.units.size() == 6 && setup.units[0].entity_id == 7 && setup.units[1].entity_id == 21
        && setup.units[2].entity_id == 45 && setup.units[3].entity_id == 100
        && setup.units[4].entity_id == 101 && setup.units[5].entity_id == 102,
        "persistent forces keep IDs, reserve stays absent, map object then craft use shared allocator");
    expect(setup.first_free_entity_id == 103 && setup.squadrons == std::vector<tactical::Squadron>{{7, {101, 102}}},
        "craft links and first-free boundary translated together");
    expect(setup.units[2].garrison_enabled && setup.units[2].position.x == whole(1200), "selected station uses shared transform and explicit garrison input");
    expect(setup.skirmish->slots.front().fleet == std::vector<std::string>{"Squadron", "ReserveShip"},
        "replay content closure includes reserve-only types without deploying them");
    auto reversed = placements; std::reverse(reversed.begin(), reversed.end());
    auto reordered = campaign::build_conflict_entry(record, fixture, inputs, reversed, 100, 11);
    expect(reordered && reordered.value().conflict.tactical == setup, "placement lookup order cannot change creation or allocation order");
    auto rebuilt_content = campaign::conflict_entry_content(setup, inputs);
    expect(static_cast<bool>(rebuilt_content), "export reconstructs content from its metadata");
    if (!rebuilt_content) return;
    expect(entry.content.economy.pads.neutral == 3 && entry.content.economy.pads.capture.size() == 1
        && rebuilt_content.value().economy.pads == entry.content.economy.pads,
        "campaign and exported child both retain neutral-pad capture content");
    std::vector<std::string> reference;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto live = tactical::TacticalSession::create(setup, entry.content);
        expect(static_cast<bool>(live), "built content creates tactical session");
        if (!live) { std::cerr << live.error().message << '\n'; return; }
        expect(static_cast<bool>(live.value().submit({{0, 1, 1}, {21}, tactical::DamagePayload{whole(10)}})), "entry accepts ordinary tactical input");
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 40; ++tick) hashes.push_back(live.value().step(executor).value().state_hash.get());
        const auto live_units = live.value().units();
        const auto pad = std::find_if(live_units.begin(), live_units.end(),
            [](const auto& unit) { return unit.entity_id == 100; });
        expect(pad != live_units.end() && pad->owner == 2,
            "AI defender captures neutral pad inside the campaign child");
        const auto replay = tactical::parse_replay(tactical::write_replay(live.value().record()).value()).value();
        auto alone = tactical::TacticalSession::from_replay(replay, rebuilt_content.value()).value();
        for (const auto& hash : hashes) expect(alone.step(executor).value().state_hash.get() == hash, "ordinary replay exactly reproduces child hashes");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "1/2/4/8 worker hashes agree");
    }
    const auto refuses = [&](const campaign::ConflictRecord& value, const std::vector<campaign::ForcePlacement>& selected,
        const sim::EntityId boundary = 100) { return !campaign::build_conflict_entry(value, fixture, inputs, selected, boundary, 11); };
    expect(refuses(record, placements, 79), "reserves cannot collide with first-free boundary");
    expect(refuses(record, placements, std::numeric_limits<sim::EntityId>::max()), "ephemeral allocation overflow refused");
    auto changed = record; changed.layer = campaign::BattleLayer::land; expect(refuses(changed, placements), "land stays gated");
    changed = record; changed.forces[1].id = 7; expect(refuses(changed, placements), "duplicate persistent IDs refused");
    changed = record; changed.forces[1].type = 999; expect(refuses(changed, placements), "unknown type refused");
    auto missing = placements; missing.pop_back(); expect(refuses(record, missing), "missing station selection refused");
    missing = placements; missing.push_back({79, 1}); expect(refuses(record, missing), "reserve placement refused");
    missing = placements; missing.front().marker = 999; expect(refuses(record, missing), "missing map marker refused");
    missing = placements; missing.push_back(placements.front()); expect(refuses(record, missing), "duplicate selection refused");
    auto wrong = inputs; wrong.map_sha256 = std::string(64, 'b');
    expect(!campaign::conflict_entry_content(setup, wrong), "export rejects wrong map identity");
}

void campaign_map_admission() {
    auto tables = synthetic_tables();
    for (const auto name : {"WorkHull", "MissionHull", "PermanentFrame"}) {
        auto type = tables.units.front(); type.id = name; type.victory_relevant = false;
        tables.units.push_back(std::move(type));
    }
    auto inputs = synthetic_inputs(tables);
    inputs.factions[0].name = "Side_A"; inputs.factions[0].can_control_planets = true;
    inputs.factions[1].name = "Side_B"; inputs.factions[1].can_control_planets = true;
    inputs.factions.push_back({"Side_C", true, false, false, {}});
    inputs.factions.back().can_control_planets = true;
    inputs.factions.push_back({"Ambient", true, false, false, {}}); // playable but cannot control planets
    auto fixture = fixture_for(inputs);
    fixture.slots = {{1, "Side_A", 0, true, {}}, {2, "Side_B", 1, false, {}},
        {4, "Side_C", 2, false, {}}, {5, "Ambient", 3, false, {}}};
    fixture.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    const auto append = [&](std::uint32_t record, std::string type, std::int32_t owner) -> skirmish::MapPlacement& {
        skirmish::MapPlacement placement;
        placement.record = record; placement.type = std::move(type); placement.element = "SpaceUnit";
        placement.owner_index = owner; placement.position = sim::math::Vec3{whole(record * 100), {}, {}};
        placement.orientation_degrees = sim::math::Vec3{}; placement.hull = whole(100);
        inputs.placements.push_back(std::move(placement));
        return inputs.placements.back();
    };
    append(10, "WorkHull", 1); // defender
    append(11, "WorkHull", 0); // attacker, without authored retention
    append(12, "WorkHull", 3).loads_when_faction_present = true; // flag cannot admit an unrelated side
    append(13, "MissionHull", 0).loads_when_faction_present = true;
    append(14, "PermanentFrame", 3).branched_discardable = false;
    append(15, "WorkHull", 2); // neutral
    append(16, "WorkHull", 4); // non-neutral, non-planet-controlling side
    append(17, "Pad", 3); // a build pad is not a MARKER exception
    append(18, "Marker", 3).marker = true;
    auto& gated = append(19, "PermanentFrame", 1);
    gated.branched_discardable = false; gated.enable_at_level = 2;
    append(20, "WorkHull", 1).enable_at_level = -1;
    append(21, "WorkHull", 3).marker_behavior = true; // MARKER behaviour without Is_Marker or marker class
    campaign::ConflictRecord record;
    record.id = 90; record.invader = 1; record.defender = 2; record.map_identity = inputs.map;
    record.station_level = 1;
    record.forces = {{7, skirmish::type_id("Ship"), 1}, {21, skirmish::type_id("Ship"), 2}};
    const std::array<campaign::ForcePlacement, 2> forces{{{7, 1}, {21, 2}}};
    const auto records = [](const skirmish::SkirmishStart& start) {
        std::vector<std::uint32_t> result;
        for (const auto& unit : start.units) if (unit.role == skirmish::UnitRole::map_object) result.push_back(unit.record);
        return result;
    };
    auto built = campaign::build_conflict_entry(record, fixture, inputs, forces, 100, 11);
    expect(bool(built), "WGT-121 renamed Fondor-like campaign map builds");
    if (!built) { std::cerr << built.error().message << '\n'; return; }
    const std::vector<std::uint32_t> kept{4, 10, 13, 14, 15, 16, 20, 21};
    expect(records(built.value().start) == kept,
        "WGT-121 keeps defender, qualified attacker, branch override, neutral and noncontrolling owners in record order");
    expect(built.value().start.setup.first_free_entity_id == 100 + kept.size(),
        "WGT-121 rejected objects consume no campaign IDs");
    expect(built.value().start.removed.size() == 4 && std::all_of(built.value().start.removed.begin(),
        built.value().start.removed.end(), [](const auto& row) { return row.reason == skirmish::Removal::campaign_branch; }),
        "WGT-121 reports ordinary attacker, foreign hull, foreign build pad and level-gated override removal");
    // The shared entry seam without a filter retains its previous map-first contract.
    const std::array<skirmish::SelectedStartUnit, 2> selected{{{1, "Ship", skirmish::UnitRole::fleet, 1},
        {2, "Ship", skirmish::UnitRole::fleet, 2}}};
    auto unfiltered = skirmish::build_selected_start(fixture, inputs, selected, true);
    expect(unfiltered && records(unfiltered.value()).size() == kept.size() + 4,
        "WGT-121 optional filter leaves the shared builder default unchanged");
    record.corruption_mission = true;
    auto corrupted = campaign::build_conflict_entry(record, fixture, inputs, forces, 100, 11);
    const std::vector<std::uint32_t> corruption_kept{4, 10, 11, 13, 14, 15, 16, 20, 21};
    expect(corrupted && records(corrupted.value().start) == corruption_kept,
        "WGT-121 active corruption retains attacker objects without admitting unrelated owners");
    record.corruption_mission = false; record.station_level = 2;
    auto higher = campaign::build_conflict_entry(record, fixture, inputs, forces, 100, 11);
    auto higher_kept = kept; higher_kept.insert(std::lower_bound(higher_kept.begin(), higher_kept.end(), 19U), 19);
    expect(higher && records(higher.value().start) == higher_kept, "WGT-121 authored enable level admits at equality");
    auto other_defender = record;
    other_defender.station_level = 1;
    other_defender.defender = 4; other_defender.forces.back().owner = 4;
    auto owned = campaign::build_conflict_entry(other_defender, fixture, inputs, forces, 100, 11);
    const std::vector<std::uint32_t> other_kept{4, 12, 13, 14, 15, 16, 17, 21};
    expect(owned && records(owned.value().start) == other_kept,
        "WGT-121 the same hulls and build pad survive when their authored faction becomes defender");
    std::vector<std::string> reference;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto child = tactical::TacticalSession::create(built.value().conflict.tactical, built.value().content);
        expect(bool(child), "WGT-121 filtered scene creates child");
        if (!child) return;
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 12; ++tick) hashes.push_back(child.value().step(executor).value().state_hash.get());
        const auto replay = tactical::parse_replay(tactical::write_replay(child.value().record()).value()).value();
        auto restored = tactical::TacticalSession::from_replay(replay, built.value().content).value();
        for (const auto& hash : hashes) expect(restored.step(executor).value().state_hash.get() == hash,
            "WGT-121 resolved child replay never readmits discarded placements");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "WGT-121 filtered child hashes equal on 1/2/4/8 workers");
    }
}

void handoff_corpus(bool fiscal_enabled = false) {
    namespace gc = sim::galactic;
    const auto tables = synthetic_tables(); const auto inputs = synthetic_inputs(tables);
    const auto fixture = fixture_for(inputs); const auto ship = skirmish::type_id("Ship");
    gc::GalacticSetup galaxy; galaxy.seed = 17; galaxy.content_identity = units::content_identity(tables);
    galaxy.players = {{1, 1, 0}, {2, 2, 0}};
    galaxy.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    galaxy.planets = {{1, {}, 1}, {2, {whole(12), {}, {}}, 2}};
    galaxy.fleets = {{10, 1, 1, {{20, ship}}}, {12, 2, 2, {{21, ship}}}};
    galaxy.rules.maximum_fleet_movement_distance = whole(12); galaxy.rules.fiscal_cycle_frames = 5;
    if (fiscal_enabled) {
        galaxy.player_economy = gc::PlayerEconomySetup{{{1, {}, whole(1000), 0, 0}, {2, {}, whole(1000), 0, 0}}};
        galaxy.fiscal = gc::FiscalSetup{whole(1000), {{1, true, whole(1)}, {2, true, whole(1)}},
            {{1, whole(11)}, {2, whole(13)}}};
    }
    galaxy.fleet_control.emplace(); galaxy.fleet_control->types = {{ship, false, true, false}};
    galaxy.movement.emplace(); auto& movement = *galaxy.movement;
    movement.mode_modifier = whole(1); movement.types = {{ship, whole(2), whole(100), 0, true, false, false, false}};
    movement.players = {{1, whole(1), whole(1), false}, {2, whole(1), whole(1), false}};
    movement.planets = {{1, whole(1), false}, {2, whole(1), false}};
    for (const auto& p : galaxy.players) for (const auto& planet : galaxy.planets)
        movement.access.push_back({p.player_id, planet.id, true, p.player_id == 1 && planet.id == 2});
    const campaign::GameSession::ContentResolver content = [&](const auto& record) {
        return campaign::conflict_entry_content(record.tactical, inputs);
    };
    const campaign::ForceQualifier qualify = [&](const gc::FleetMember& member, gc::PlayerId owner, sim::EntityId parent) {
        campaign::AdmissionForce row; row.force = {member.id, member.type, owner, parent};
        row.space_allowed = row.triggers_space = true; row.population = whole(1); row.power = whole(10);
        return core::Result<campaign::AdmissionForce>::success(row);
    };
    const campaign::GameSession::ConflictService service = [&](const auto& snapshot, const auto& event, sim::EntityId next) {
        using Result = core::Result<std::optional<campaign::RecordedAdmission>>;
        auto presence = campaign::collect_conflict_presence(snapshot, event, qualify, [](const auto&) { return core::Result<campaign::OuterFleetFacts>::success({false, true, true, false}); });
        if (!presence) return Result::failure(presence.error());
        if (!campaign::admits_space_conflict(presence.value())) return Result::success({});
        campaign::ConflictRecord record; record.id = next; record.planet = event.value;
        record.map_identity = fixture.map;
        record.invader = presence.value().invader; record.defender = presence.value().defender;
        record.seats = {{{1}, 1}, {{2}, 2}};
        std::vector<campaign::ForcePlacement> placements;
        for (const auto* rows : {&presence.value().invading_forces, &presence.value().orbital_forces}) for (const auto& row : *rows) {
            record.forces.push_back(row.force); placements.push_back({row.force.id, row.force.owner == 1 ? 1U : 2U});
        }
        std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        auto entry = campaign::build_conflict_entry(record, fixture, inputs, placements, next + 1, galaxy.seed);
        if (!entry) return Result::failure(entry.error());
        campaign::ArbitrationInputs arbitration; arbitration.local_involved = true; arbitration.queue_tactical = true;
        return Result::success(campaign::RecordedAdmission{0, entry.value().conflict, campaign::arbitrate_conflict(arbitration).value()});
    };
    std::vector<std::string> reference;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        auto services = std::make_shared<platform::CampaignServiceConfiguration>();
        services->configure = [&](campaign::GameSession& owner) {
            owner.set_conflict_service(service, content);
            return core::Result<void>::success();
        };
        platform::LiveGameSession::Options live_options;
        live_options.workers = workers; live_options.pacing = platform::LiveGameSession::Pacing::driven;
        live_options.services = services;
        auto live = platform::LiveGameSession::start(galaxy, live_options).value();
        live->submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}});
        live->advance_to(6);
        expect(live->wait_for(6, std::chrono::seconds(10)), "pending alert reaches its publication boundary");
        const auto alerts = live->galactic_events_after(0);
        std::size_t alert_count = 0;
        for (const auto& published : alerts.events) if (published.event.kind == gc::GalacticEventKind::conflict_pending) {
            ++alert_count;
            expect(published.pending_alert && published.pending_alert->invader == 1
                && published.pending_alert->defender == 2 && !published.pending_alert->land,
                "retained pending alert carries the conflict owner's exact players and layer");
        }
        expect(alert_count == 1 && live->galactic_events_after(alerts.through).events.empty(),
            "pending alert publishes once with an exclusive consumer cursor on 1/2/4/8 workers");
        live->stop();
        expect(!live->galactic_events_after(0).events.empty(), "pending publication survives owner stop");
        const platform::ThreadWorkerAdapter executor(workers);
        auto game = campaign::GameSession::create(galaxy).value(); game.set_conflict_service(service, content);
        const auto fiscal_paid_once = [&] {
            if (!fiscal_enabled) return true;
            const auto& accounts = game.galaxy().snapshot()->player_economy()->players;
            return accounts[0].credits == whole(11) && accounts[1].credits == whole(13);
        };
        const auto identities = checkpoint::game_identities(galaxy);
        checkpoint::GameLoadOptions initial_options; initial_options.content = content; initial_options.conflict_service = service;
        auto initial_saved = checkpoint::save_game_checkpoint(game, identities).value();
        auto initial_loaded = checkpoint::load_game_checkpoint(initial_saved, identities, initial_options, executor);
        expect(bool(initial_loaded) && initial_loaded.value().session.recording().conflict_services
            && initial_loaded.value().session.state_sha256() == game.state_sha256(),
            "conflict service context survives an empty admission checkpoint before arrival");
        if (!initial_loaded) return;
        game = std::move(initial_loaded.value().session);
        std::vector<std::string> hashes; int applied = 0;
        game.set_result_applier([&](auto&, const auto&) { ++applied; return core::Result<void>::success(); });
        expect(bool(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}})), "handoff corpus movement accepted");
        while (!game.pending_conflict() && game.completed_tick() < 20) {
            auto step = game.step(executor);
            expect(bool(step), "arrival/admission step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; return; }
            hashes.push_back(step.value().state_hash.get());
        }
        expect(game.pending_conflict() && !game.battle(), "arrival admits a pending conflict before loading child");
        if (!game.pending_conflict()) return;
        const auto record = *game.pending_conflict();
        expect(game.galaxy().clock().pause_depth == 0 && game.recording().admissions.size() == 1,
            "GFD18 arrival t announces without arbitrating or pausing");
        const auto announced_frame = game.galaxy().clock().galactic_frame;
        const auto announced_cycle = game.galaxy().clock().fiscal_cycle;
        if (fiscal_enabled) expect(game.galaxy().snapshot()->player_economy()->players[0].credits == Fixed{},
            "configured fiscal does not pay before the retained deadline frame");
        auto arbitration_tick = game.step(executor).value(); hashes.push_back(arbitration_tick.state_hash.get());
        expect(!game.battle() && game.pending_conflict() && game.galaxy().clock().galactic_frame == announced_frame
            && !arbitration_tick.conflict_events.empty(), "GFD18 next eligible tail t+1 arbitrates and retains pending choice");
        expect(announced_frame == 5 && game.galaxy().clock().fiscal_cycle == announced_cycle + 1
            && arbitration_tick.galaxy_work.fiscal_cycles == 1,
            "GFD26 paused arbitration pays a due fiscal window once at the retained frame");
        expect(fiscal_paid_once(), "configured fiscal pays both owners once when arbitration pauses the deadline frame");
        const auto paused_frame = game.galaxy().clock().galactic_frame;
        expect(game.galaxy().clock().pause_depth == 1, "pending conflict owns one nested pause");
        expect(bool(game.submit({{game.completed_tick(), {2}, 1}, gc::BattleChoice{record.id, true}})), "AI choice shape is valid");
        auto ai_choice = game.step(executor).value(); hashes.push_back(ai_choice.state_hash.get());
        expect(!ai_choice.diagnostics.empty() && game.pending_conflict() && !game.battle(),
            "AI-side input cannot select the human's pending battle");
        expect(bool(game.submit({{game.completed_tick(), {1}, 2}, gc::PauseGalaxy{}})), "player adds its own pause");
        hashes.push_back(game.step(executor).value().state_hash.get());
        expect(game.galaxy().clock().galactic_frame == paused_frame && game.galaxy().clock().pause_depth == 2,
            "pending conflict freezes galactic time and preserves independent pause");
        expect(game.galaxy().clock().fiscal_cycle == announced_cycle + 1,
            "GFD26 pending pause does not repeat the retained frame fiscal payment");
        expect(fiscal_paid_once(), "pending pause does not repeat configured fiscal credits");
        const auto saved = checkpoint::save_game_checkpoint(game, identities).value();
        checkpoint::GameLoadOptions options; options.content = content;
        options.apply_result = [&](auto&, const auto&) { ++applied; return core::Result<void>::success(); };
        auto loaded = checkpoint::load_game_checkpoint(saved, identities, options, executor);
        expect(bool(loaded), "pending conflict checkpoint restores");
        if (!loaded) { std::cerr << loaded.error().message << '\n'; return; }
        expect(loaded.value().session.state_sha256() == game.state_sha256()
            && loaded.value().session.pending_conflict() && *loaded.value().session.pending_conflict() == record,
            "pending restore retains policy, roster, content and nested pause");
        game = std::move(loaded.value().session);
        expect(fiscal_paid_once(), "pending checkpoint retains the already-paid fiscal ledger");
        expect(bool(game.submit({{game.completed_tick(), {1}, 3}, gc::BattleChoice{record.id + 1, true}})), "stale choice shape is valid");
        auto rejected = game.step(executor).value(); hashes.push_back(rejected.state_hash.get());
        expect(!rejected.diagnostics.empty() && game.pending_conflict(), "stale conflict choice cannot load child");
        expect(bool(game.submit({{game.completed_tick(), {1}, 4}, gc::BattleChoice{record.id, true}})), "tactical choice accepted");
        auto chosen = game.step(executor).value(); hashes.push_back(chosen.state_hash.get());
        expect(game.battle() && !game.pending_conflict() && game.galaxy().clock().galactic_frame == paused_frame,
            "choice builds FT09 child while retained galaxy remains frozen");
        expect(content(record).value().economy.pads.capture.size() == 1, "child resolver retains pad capture content");
        tactical::TacticalReplay envelope; envelope.setup.players = record.tactical.players;
        envelope.final_tick_count = 1; envelope.commands = {{{0, 1, 1}, {}, tactical::QuitPayload{}}};
        const auto bytes = tactical::write_replay(envelope).value();
        const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
        const auto start = header + envelope.setup.players.size() * 24 + 4;
        gc::TacticalLayerCommand quit{record.id, bytes[start + 20],
            std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end())};
        expect(bool(game.submit({{game.completed_tick(), {1}, 5}, quit})), "child quit routes through session envelope");
        while (game.battle() && game.completed_tick() < 600) {
            auto step = game.step(executor);
            expect(bool(step), "child/result step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; return; }
            hashes.push_back(step.value().state_hash.get());
            expect(game.galaxy().clock().galactic_frame == paused_frame, "child never services galactic parent");
            expect(fiscal_paid_once(), "child never repeats parent fiscal credits");
        }
        expect(!game.battle() && applied == 1, "result applies exactly once then tears down child");
        expect(game.galaxy().clock().pause_depth == 1, "return removes only conflict-owned nested pause");
        expect(bool(game.submit({{game.completed_tick(), {1}, 6}, gc::ResumeGalaxy{}})), "player resumes remaining pause");
        hashes.push_back(game.step(executor).value().state_hash.get());
        expect(game.galaxy().clock().galactic_frame == paused_frame + 1, "galaxy resumes after child/result return");
        expect(fiscal_paid_once(), "result return and resume preserve the one paid fiscal window");
        const auto recording = game.recording();
        auto replayed = campaign::GameSession::from_recording(recording, content).value();
        std::vector<std::string> replay_hashes;
        while (replayed.completed_tick() < recording.commands.final_tick_count) {
            replayed.scramble_storage_for_testing(); auto step = replayed.step(executor);
            expect(bool(step), "handoff history replay succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; return; }
            replay_hashes.push_back(step.value().state_hash.get());
        }
        expect(replay_hashes == hashes, "arrival -> pending -> child -> result -> resume reproduces every hash");
        if (workers == 1) reference = hashes; else expect(reference == hashes, "handoff corpus equal on 1/2/4/8 workers");
    }
    // WAR-02/03: an automatic branch waiting on its resolver remains automatic,
    // including the non-interactive exceptions to a tactical forcing gate.
    std::string automatic_reference;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto game = campaign::GameSession::create(galaxy).value();
        game.set_conflict_service([&](const auto& snapshot, const auto& event, sim::EntityId next) {
            auto selected = service(snapshot, event, next);
            if (selected && selected.value()) selected.value()->arbitration = {campaign::ConflictRoute::instant_auto, true, false};
            return selected;
        }, content);
        expect(bool(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}})), "automatic movement accepted");
        while (!game.pending_conflict() && game.completed_tick() < 20) {
            auto tick = game.step(executor); expect(bool(tick), "automatic admission succeeds");
            if (!tick) return;
        }
        expect(game.pending_conflict() != nullptr, "automatic entry waits for its resolver");
        if (!game.pending_conflict()) return;
        const auto id = game.pending_conflict()->id;
        expect(bool(game.step(executor)), "automatic arbitration succeeds");
        expect(bool(game.submit({{game.completed_tick(), {1}, 2}, gc::BattleChoice{id, true}})), "automatic choice shape is valid");
        auto refused = game.step(executor).value();
        expect(!refused.diagnostics.empty() && game.pending_conflict() && !game.battle()
            && game.galaxy().clock().pause_depth == 1,
            "battle choice cannot redirect a sourced automatic branch while its resolver waits");
        if (workers == 1) automatic_reference = refused.state_hash.get();
        else expect(automatic_reference == refused.state_hash.get(), "automatic choice refusal equal on 1/2/4/8 workers");
    }
    // A synchronized human choice cannot let an external vote decision defeat
    // the sourced story/tutorial tactical forcing gate.
    auto versus = galaxy; versus.seats[1].kind = gc::SeatKind::human; versus.seats[1].human_side_id = 2;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto game = campaign::GameSession::create(versus).value();
        game.set_conflict_service([&](const auto& snapshot, const auto& event, sim::EntityId next) {
            auto selected = service(snapshot, event, next);
            if (selected && selected.value()) selected.value()->arbitration = {campaign::ConflictRoute::pending_tactical, true, false};
            return selected;
        }, content);
        game.set_vote_progress([](const auto&, const auto&) {
            return core::Result<std::optional<bool>>::success(false);
        });
        expect(bool(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}})), "forced versus movement accepted");
        while (!game.pending_conflict() && game.completed_tick() < 20) {
            auto tick = game.step(executor);
            expect(bool(tick), "forced versus admission succeeds");
            if (!tick) return;
        }
        expect(game.pending_conflict() != nullptr, "forced versus has pending entry");
        if (!game.pending_conflict()) return;
        const auto id = game.pending_conflict()->id;
        expect(bool(game.step(executor)), "forced versus arbitration succeeds");
        expect(bool(game.submit({{game.completed_tick(), {1}, 2}, gc::BattleChoice{id, true}})), "forced yes vote accepted");
        const auto refused = game.step(executor).value();
        expect(!refused.diagnostics.empty() && game.pending_conflict() && !game.battle(),
            "forced tactical entry refuses an automatic vote result without ending the session");
        game.set_vote_progress([](const auto&, const auto&) { return core::Result<std::optional<bool>>::success(true); });
        expect(bool(game.submit({{game.completed_tick(), {2}, 1}, gc::BattleChoice{id, true}})), "other human yes vote accepted");
        expect(bool(game.step(executor)) && game.battle(), "forced tactical vote result loads the child");
    }
}

int export_installed(const std::filesystem::path& root, const std::filesystem::path& out,
    const std::string& map = {}, const bool fondor = false, const bool standalone = false) {
    std::vector<vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, {"base", "GameData"}}) {
        auto mount = vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!mount) { std::cerr << mount.error().message << '\n'; return 1; }
        mounts.push_back(std::move(mount).value().mount);
    }
    auto filesystem = vfs::Vfs::mount(mounts).value();
    auto catalog = data::load_catalog(filesystem, data::Profile::foc).value();
    scene::VfsAssetCache cache(filesystem);
    units::LoadInput load; load.campaign_scene = standalone;
    if (standalone) for (const auto name : units::pinned_m2_types()) load.types.emplace_back(name);
    load.catalog = &catalog.catalog; load.filesystem = &filesystem;
    load.model = cache.access().model;
    auto fixture = skirmish::m2_fixture(); fixture.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    if (!map.empty()) fixture.map = map;
    load.space_map = fixture.map;
    auto tables = units::load_unit_tables(load).value();
    if (standalone) {
        // Native preparation supplies the campaign's production type roots. This
        // synthetic installed fixture starts from a smaller fleet, so follow its
        // authored upgrade links before selecting the actual battle closure.
        while (true) {
            std::vector<std::string> additional;
            for (const auto& type : tables.units) {
                const auto& next = type.production.next_upgrade;
                if (!next.empty() && std::find(load.types.begin(), load.types.end(), next) == load.types.end()
                    && std::find(additional.begin(), additional.end(), next) == additional.end()) additional.push_back(next);
            }
            if (additional.empty()) break;
            load.types.insert(load.types.end(), additional.begin(), additional.end());
            auto expanded = units::load_unit_tables(load);
            if (!expanded) { std::cerr << expanded.error().message << '\n'; return 1; }
            tables = std::move(expanded).value();
        }
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog.catalog, tables).value();
    fixture.map_sha256 = inputs.map_sha256;
    std::optional<tactical::ReplayNativeScene> native;
    if (standalone) {
        skirmish::FixtureOptions lobby; lobby.map = fixture.map;
        expect(!skirmish::fixture_from_options(lobby, filesystem, catalog.catalog),
            "legacy campaign map still refuses the ordinary two-slot skirmish lobby");
        load.space_map.clear();
        // Preserve every authored map input, including capture/build-pad types
        // promoted into unit profiles and therefore absent from tables.obstacles.
        for (const auto& placement : inputs.placements) if (!placement.marker
            && std::find(load.obstacles.begin(), load.obstacles.end(), placement.type) == load.obstacles.end())
            load.obstacles.push_back(placement.type);
        native = campaign::native_scene_selection(load, tables);
    }
    if (fondor) {
        fixture.slots.push_back({3, "Underworld", 2, false, {}});
    }
    std::vector<const skirmish::MapPlacement*> markers;
    for (const auto& placement : inputs.placements)
        if (placement.marker && placement.position && placement.orientation_degrees
            && placement.orientation_degrees->x == Fixed{} && placement.orientation_degrees->y == Fixed{}) markers.push_back(&placement);
    if (markers.size() < 2) { std::cerr << "fixture needs two authored yaw-only markers\n"; return 1; }
    campaign::SyntheticCampaign synthetic; synthetic.name = "conflict-entry-test";
    synthetic.galaxy.fleets_per_player = 1; synthetic.galaxy.members_per_fleet = 1;
    if (fondor) synthetic.galaxy.players = 3;
    auto galaxy = campaign::build_synthetic_campaign(synthetic).value();
    galaxy.content_identity = units::content_identity(tables);
    auto session = campaign::GameSession::create(galaxy).value();
    campaign::ConflictRecord record; record.invader = fondor ? 3U : 1U; record.defender = fondor ? 1U : 2U;
    record.planet = galaxy.planets.front().id;
    record.map_identity = fixture.map; record.id = session.galaxy().next_entity_id() + 1;
    if (!standalone) { record.station = record.id - 1; record.station_level = 1; }
    record.seats = {{{record.invader}, record.invader}, {{record.defender}, record.defender}};
    std::sort(record.seats.begin(), record.seats.end(), [](const auto& a, const auto& b) { return a.seat < b.seat; });
    std::vector<campaign::ForcePlacement> placements;
    for (auto& fleet : galaxy.fleets) {
        if (fleet.owner != record.invader && fleet.owner != record.defender) continue;
        const auto type = fleet.owner == 1 ? "Y-Wing_Squadron" : "Tartan_Patrol_Cruiser";
        fleet.members.front().type = skirmish::type_id(type);
        record.forces.push_back({fleet.members.front().id, fleet.members.front().type, fleet.owner, fleet.id});
        placements.push_back({fleet.members.front().id, markers[fleet.owner == record.invader ? 0 : 1]->record});
    }
    if (!standalone) {
        const auto station = std::find_if(tables.units.begin(), tables.units.end(), [&](const auto& type) {
            return type.kind == units::UnitKind::station && type.affiliation == (fondor ? "Rebel" : "Empire");
        });
        if (station == tables.units.end()) return 1;
        record.station_type = skirmish::type_id(station->id);
        record.forces.push_back({record.station, record.station_type, record.defender, 0, campaign::ForceRole::escort});
        placements.push_back({record.station, markers[1]->record});
    }
    std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (standalone) {
        // A fleet-only native conflict. Match the live provider's record-driven
        // reachability instead of making unrelated raw prototypes battle actors.
        native->battle_types.clear();
        for (const auto& type : tables.units) if (std::any_of(record.forces.begin(), record.forces.end(),
            [&](const auto& force) { return force.type == skirmish::type_id(type.id); }))
            native->battle_types.push_back(type.id);
        auto selected = campaign::native_scene_tables(tables, *native, inputs.placements);
        if (!selected) { std::cerr << selected.error().message << '\n'; return 1; }
        tables = std::move(selected).value(); inputs.tables = &tables;
        native = campaign::native_scene_selection(load, tables);
        galaxy.content_identity = units::content_identity(tables);
    }
    std::filesystem::create_directories(out);
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto built = campaign::build_conflict_entry(record, fixture, inputs, placements, record.id + 1, galaxy.seed);
        if (!built) { std::cerr << built.error().message << '\n'; return 1; }
        if (standalone) {
            built.value().conflict.tactical.native_scene = native;
            auto bound = campaign::space_entry_content(built.value().conflict.tactical, inputs);
            if (!bound) { std::cerr << bound.error().message << '\n'; return 1; }
            built.value().content = std::move(bound).value();
        }
        if (fondor) {
            const auto foreign = std::count_if(built.value().start.removed.begin(), built.value().start.removed.end(),
                [](const auto& row) { return row.faction == "Empire" && row.reason == skirmish::Removal::campaign_branch; });
            const auto survived = std::any_of(built.value().start.units.begin(), built.value().start.units.end(),
                [](const auto& unit) { return unit.role == skirmish::UnitRole::map_object && unit.state.owner == 2; });
            if (foreign == 0 || survived) { std::cerr << "Fondor foreign map placements were not discarded\n"; return 1; }
            std::ofstream report(out / "admission.json");
            report << "{\"map\":\"" << fixture.map << "\",\"foreign_removed\":" << foreign
                << ",\"foreign_survived\":false,\"units\":" << built.value().start.units.size() << "}\n";
        }
        auto game = campaign::GameSession::create(galaxy).value();
        auto staged = game.stage_battle(built.value().conflict, built.value().content);
        if (!staged) { std::cerr << staged.error().message << '\n'; return 1; }
        auto entered = game.step(executor); if (!entered) return 1;
        std::ostringstream hashes; hashes << "tick,sha256\n";
        for (int tick = 0; tick < 60; ++tick) {
            auto step = game.step(executor);
            if (!step || !step.value().battle) return 1;
            hashes << step.value().battle->completed_tick() << ',' << step.value().battle_state_hash.get() << '\n';
        }
        const auto recording = *game.battle_record();
        const auto replay = tactical::write_replay(recording).value();
        if (standalone) {
            const auto parsed = tactical::parse_replay(replay);
            if (!parsed) { std::cerr << parsed.error().message << '\n'; return 1; }
            auto restored = campaign::load_standalone_space_replay(parsed.value(), filesystem, catalog.catalog);
            if (!restored) { std::cerr << restored.error().message << '\n'; return 1; }
            const auto& scene = restored.value();
            expect(scene.setup == recording.setup && scene.start && scene.start->setup == recording.setup,
                "standalone native loader projects exact sparse actors without rebuilding placement");
            tactical::TacticalContent content;
            content.sensors = scene.content.sensors; content.durability = scene.content.durability;
            content.motion = scene.content.motion; content.combat = scene.content.combat;
            content.abilities = scene.content.abilities; content.fog = scene.content.fog;
            content.map_box = scene.content.map_box; content.victory = scene.victory; content.economy = scene.economy;
            auto child = tactical::TacticalSession::from_replay(parsed.value(), content);
            if (!child) { std::cerr << child.error().message << '\n'; return 1; }
            std::ostringstream reopened; reopened << "tick,sha256\n";
            for (int tick = 0; tick < 60; ++tick) {
                auto stepped = child.value().step(executor);
                if (!stepped) return 1;
                reopened << child.value().completed_tick() << ',' << stepped.value().state_hash.get() << '\n';
            }
            expect(reopened.str() == hashes.str(), "standalone campaign replay reproduces child hashes on 1/2/4/8 workers");
            auto wrong = parsed.value(); wrong.setup.skirmish->map_sha256[0] =
                wrong.setup.skirmish->map_sha256[0] == 'a' ? 'b' : 'a';
            expect(!campaign::load_standalone_space_replay(wrong, filesystem, catalog.catalog), "native replay rejects changed map digest");
            wrong = parsed.value(); wrong.setup.content_identity[0] ^= 1;
            expect(!campaign::load_standalone_space_replay(wrong, filesystem, catalog.catalog), "native replay rejects changed content identity");
        }
        std::ofstream binary(out / ("child-" + std::to_string(workers) + ".eawr-replay"), std::ios::binary);
        binary.write(reinterpret_cast<const char*>(replay.data()), static_cast<std::streamsize>(replay.size()));
        std::ofstream csv(out / ("child-" + std::to_string(workers) + ".csv"), std::ios::binary); csv << hashes.str();
    }
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int main(const int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--map-admission") {
        campaign_map_admission();
        return galactic_test::failures == 0 ? 0 : 1;
    }
    if (argc == 4 && std::string(argv[1]) == "--standalone-fondor")
        return export_installed(argv[2], argv[3], "data/art/maps/_space_planet_fondor_01.ted", true, true);
    if (argc == 4 && std::string(argv[1]) == "--fondor")
        return export_installed(argv[2], argv[3], "data/art/maps/_space_planet_fondor_01.ted", true);
    if (argc == 2 && std::string(argv[1]) == "--handoff") {
        handoff_corpus(); handoff_corpus(true);
        return galactic_test::failures == 0 ? 0 : 1;
    }
    if (argc == 3 || argc == 4) return export_installed(argv[1], argv[2], argc == 4 ? argv[3] : "");
    contracts(); queued_pending_alerts(); native_content_selection();
    return galactic_test::failures == 0 ? 0 : 1;
}

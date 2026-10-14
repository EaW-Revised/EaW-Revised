#include "galactic_support.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/campaign/space_rules.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace eawr;
namespace gc = sim::galactic;
namespace tt = sim::tactical;
using galactic_test::expect;
using sim::math::Fixed;
Fixed whole(std::int64_t n) { return Fixed::from_integer(n).value(); }
template<class T> T take(core::Result<T> result) { if (!result) throw std::runtime_error(result.error().message); return std::move(result).value(); }
void check(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void salvage_contracts() {
    tt::TacticalSetup setup; setup.seed = 17; setup.first_free_entity_id = 100;
    // Player fields are ID, team, faction, flags: owners 1 and 3 are allies; victim faction is 2.
    setup.players = {{1, 1, 1, tt::player_flag_commandable}, {2, 2, 2, tt::player_flag_commandable},
        {3, 1, 3, tt::player_flag_commandable}};
    for (const auto& [id, type, owner] : {std::tuple<sim::EntityId, tt::TypeId, tt::PlayerId>{10, 100, 2},
        {11, 100, 2}, {20, 200, 1}, {30, 201, 3}, {40, 202, 1}}) {
        tt::UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner; setup.units.push_back(unit);
    }
    setup.campaign.emplace();
    setup.campaign->players = {{1, {}, 0}, {2, whole(50), 0}, {3, whole(50), 0}};
    setup.campaign->players[0].credit_cap = whole(5);
    for (auto& player : setup.campaign->players) if (player.player != 1) player.credit_cap = whole(1000);
    setup.campaign->upgrades = {{200, 20, 1}, {202, 40, 1}, {201, 30, 3}};
    tt::TacticalContent content;
    for (const tt::TypeId type : {100U, 200U, 201U, 202U}) {
        tt::DurabilityProfile profile; profile.type_id = type; profile.max_hull = whole(100);
        content.durability.profiles.push_back(profile);
    }
    content.economy.campaign = true; content.economy.players = setup.campaign->players;
    content.economy.bounty_values = {{100, whole(10)}};
    const auto add = [&](tt::TypeId type, sim::math::Fixed percentage, std::int32_t category, bool allies) {
        tt::UpgradeProfile profile; profile.type = type;
        profile.bounties.push_back({percentage, category, {2}, allies}); content.economy.upgrades.push_back(profile);
    };
    add(200, take(sim::math::Fixed::from_decimal("0.5")), -1, false);
    add(201, whole(2), -1, true);
    add(202, take(sim::math::Fixed::from_decimal("0.25")), -2, false);
    std::string reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto battle = take(tt::TacticalSession::create(setup, content));
        check(battle.submit({{0, 1, 0}, {10}, tt::DamagePayload{whole(1000)}}));
        static_cast<void>(take(battle.step(executor)));
        expect(battle.ledgers()[0].credits == whole(5) && battle.ledgers()[0].collected_bounty == whole(23)
            && battle.ledgers()[2].credits == whole(70) && battle.ledgers()[2].collected_bounty == whole(20)
            && battle.ledgers()[1].credits == whole(50), "salvage uses category maxima, category sum, allied final-blow recipients and uncapped tally");
        const auto saved = take(battle.save_checkpoint_state());
        auto restored = take(tt::TacticalSession::create(setup, content));
        check(restored.submit({{0, 1, 0}, {10}, tt::DamagePayload{whole(1000)}}));
        check(restored.load_checkpoint_state(saved));
        expect(restored.ledgers()[0].collected_bounty == whole(23), "salvage tally survives checkpoint restore");
        for (auto* session : {&battle, &restored}) {
            check(session->submit({{1, 1, 0}, {30}, tt::DamagePayload{whole(1000)}}));
            check(session->submit({{1, 1, 1}, {11}, tt::DamagePayload{whole(1000)}}));
            static_cast<void>(take(session->step(executor)));
        }
        expect(battle.ledgers()[0].collected_bounty == whole(31) && battle.ledgers()[2].collected_bounty == whole(20),
            "an allied source death pays nothing and removes only that source before the next payout");
        expect(battle.state_sha256() == restored.state_sha256(), "restored salvage continues identically without repeating payments");
        if (reference.empty()) reference = battle.state_sha256();
        else expect(reference == battle.state_sha256(), "salvage is deterministic on 1/2/4/8 workers");
    }
    content.economy.bounty_collection_enabled = false;
    auto disabled = take(tt::TacticalSession::create(setup, content));
    check(disabled.submit({{0, 1, 0}, {10}, tt::DamagePayload{whole(1000)}}));
    static_cast<void>(take(disabled.step(platform::ThreadWorkerAdapter(1))));
    expect(disabled.ledgers()[0].collected_bounty == sim::math::Fixed{}, "disabled collection has no payout or tally");
    content.economy.bounty_collection_enabled = true;
    content.economy.bounty_values.front().second = take(Fixed::from_decimal("0.01"));
    auto minimum = take(tt::TacticalSession::create(setup, content));
    check(minimum.submit({{0, 1, 0}, {10}, tt::DamagePayload{whole(1000)}}));
    static_cast<void>(take(minimum.step(platform::ThreadWorkerAdapter(1))));
    expect(minimum.ledgers()[0].collected_bounty == whole(1) && minimum.ledgers()[2].collected_bounty == whole(1),
        "a positive sub-one tactical payout has minimum one");
    content.economy.bounty_values.front().second = {};
    auto zero = take(tt::TacticalSession::create(setup, content));
    check(zero.submit({{0, 1, 0}, {10}, tt::DamagePayload{whole(1000)}}));
    static_cast<void>(take(zero.step(platform::ThreadWorkerAdapter(1))));
    expect(zero.ledgers()[0].collected_bounty == Fixed{}, "zero victim bounty suppresses even the tactical minimum");
}
void script_manifest_contracts() {
    namespace foc = script::foc;
    namespace auth = script::authoritative;
    foc::AiSetup candidates;
    for (const auto* script_name : {"ObjectScript_VaderTIESquadron", "ObjectScript_MillenniumFalcon", "Sundered_Heart"}) {
        foc::AiType type; type.lua_script = script_name; candidates.content.types.push_back(std::move(type));
    }
    std::map<std::string, std::string> cached;
    for (const auto& path : foc::required_modules(candidates)) cached.emplace(path, "Marker = 17\n");
    for (auto list : {foc::plan_library_modules(), foc::selected_plans()})
        for (const auto& path : list) cached.emplace(path, "Marker = 23\n");

    foc::AiSetup battle;
    foc::AiType companion; companion.lua_script = "objectscript_vadertiesquadron";
    battle.content.types = {companion, companion}; // The reachable type can spawn after entry; no initial actor is needed.
    const auto modules = take(skirmish::select_ai_modules(cached, battle, true));
    const std::string vader = "Data/Scripts/GameObject/OBJECTSCRIPT_VADERTIESQUADRON.lua";
    const std::string state_machine = "Data/Scripts/Library/PGStateMachine.lua";
    expect(modules.contains(vader) && modules.contains(state_machine),
        "SOS-40/45: a reachable companion root and state-machine dependency enter the battle manifest");
    expect(!modules.contains("Data/Scripts/GameObject/OBJECTSCRIPT_MILLENNIUMFALCON.lua")
        && !modules.contains("Data/Scripts/GameObject/SUNDERED_HEART.lua")
        && !modules.contains("Data/Scripts/GameObject/HeroPlanAttach.lua"),
        "unrelated candidate object roots and their private dependencies stay out of the session");
    expect(modules.size() == foc::required_modules(battle).size() + foc::plan_library_modules().size() + foc::selected_plans().size(),
        "repeated and mixed-case attachment declarations select one root and retain campaign plan modules");
    auth::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) check(manifest.add(path, bytes));
    auto scheduler = take(auth::ScriptScheduler::create({}, std::move(manifest)));
    check(scheduler.create_instance(1, "data/scripts/gameobject/OBJECTSCRIPT_VADERTIESQUADRON.lua"));
    const auto marker = take(scheduler.read_global(1, "Marker"));
    expect(marker.has_value(), "the selected companion root resolves through canonical manifest lookup");

    const auto plain = take(skirmish::select_ai_modules(cached, {}, true));
    expect(!plain.contains(vader) && !plain.contains(state_machine), "a plain battle admits no unrelated object roots or state-machine dependency");
    auto incomplete = cached; incomplete.erase(vader);
    const auto missing_root = skirmish::select_ai_modules(incomplete, battle, true);
    expect(!missing_root && missing_root.error().code == auth::codes::module_rejected
        && missing_root.error().message.find(vader) != std::string::npos, "a missing reachable root fails during closure selection with its path");
    expect(static_cast<bool>(skirmish::select_ai_modules(incomplete, {}, true)), "an unrelated missing root does not refuse a plain battle");
    incomplete = cached; incomplete.erase(state_machine);
    expect(!skirmish::select_ai_modules(incomplete, battle, true), "a missing shared object dependency cannot be silently omitted");
    foc::AiSetup hero_plan; foc::AiType hero; hero.lua_script = "sundered_heart"; hero_plan.content.types.push_back(hero);
    const auto hero_modules = take(skirmish::select_ai_modules(cached, hero_plan));
    expect(hero_modules.contains("Data/Scripts/GameObject/HeroPlanAttach.lua"), "SOS-17: an admitted hero-plan script retains its private dependency");
}
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
            type.movement.space_layer = "Corvette"; type.campaign_ship_class="corvette";
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

campaign::SpaceEntryInputs entry_inputs(const units::UnitTables& tables) {
    campaign::SpaceEntryInputs input; input.map = synthetic_inputs(tables);
    input.map.placements = {marker(1, "Attacker Entry Position", -1000),
        marker(2, "Defending Forces Position", 1000), marker(3, "Space Station Position", 1200),
        marker(4, "Attacker Entry Position", 500), marker(5, "Space Station Position", 1400),
        marker(6, "Orbital Special Structure Position", 1600)};
    auto source = marker(7, "Weapon source", 1800); source.special_weapon_source = true;
    input.map.placements.push_back(source);
    input.sides = {{1, "Rebel", whole(2), true}, {2, "Empire", whole(2), false}};
    const auto add = [&](gc::TypeId id, std::string name, std::int64_t pop) {
        campaign::SpaceEntryType type; type.galactic = id; type.tactical = std::move(name); type.population = whole(pop);
        input.types.push_back(type);
    };
    add(1, "Ship", 1); input.types.back().escorts = {"ReserveShip"};
    add(2, "ReserveShip", 2); add(3, "Transport", 0);
    input.types.back().transport = true;
    add(4, "Commander", 0); input.types.back().attach_to_flagship = input.types.back().named_hero = true;
    add(5, "Weapon", 0); input.types.back().special_weapon = true;
    add(6, "Station", 0);
    add(7, "Pad", 0); input.types.back().orbital = true; input.types.back().base_position = "Orbital";
    input.station_type = "Station"; input.station_galactic_type = 6; input.planet_owner = 2;
    input.structures = {{40, 5, 2, {}}, {41, 7, 2, {}}};
    input.riders = {{{24, 4, 1, 10}, "Commander", 23, {}, {}}};
    input.riders.front().force.hero = true;
    return input;
}
units::UnitTables tables_for_entry() {
    auto tables = synthetic_tables();
    for (const auto name : {"Transport", "Commander", "Weapon"}) {
        auto type = tables.units.front(); type.id = name; type.production.population_value = 0;
        if (std::string_view(name)=="Transport") type.campaign_ship_class="transport";
        if (std::string_view(name) == "Commander") type.named_hero = true;
        tables.units.push_back(type);
    }
    // The campaign tactical economy reads these authored constants (WGT-27); synthetic values.
    for (const auto& [tag, value] : {std::pair{"Tactical_Build_Time_Multiplier", 1}, {"Space_Elevated_Vulnerability_Factor", -3},
        {"Space_Elevated_Vulnerability_Duration", 5}, {"Space_Reinforcement_Collision_Check_Distance", 10}}) {
        units::NamedConstant constant; constant.tag = tag; constant.value = whole(value);
        tables.constants.scalars.push_back(constant);
    }
    auto station = std::find_if(tables.units.begin(), tables.units.end(), [](const auto& t) { return t.id == "Station"; });
    station->spawner.emplace(); station->spawner->starting = {{"Squadron", units::no_index, 1}};
    station->spawner->delay_seconds = whole(1);
    station->spawner->reserves = {{"Squadron", units::no_index, -1}};
    units::Hardpoint bay; bay.id = "Bay"; bay.type = units::HardpointType::fighter_bay;
    bay.attachment.position = sim::math::Vec3{}; bay.health = whole(100);
    station->hardpoints.push_back(bay);
    return tables;
}
void nonparticipant_team_contracts() {
    auto tables = tables_for_entry();
    auto modifier = tables.units.front(); modifier.id = "RenamedPlanetModifier";
    modifier.production.upgrade_object = true; modifier.victory_relevant = false;
    modifier.affiliation = "RenamedThird; RenamedFourth";
    tables.units.push_back(modifier);
    auto input = entry_inputs(tables);
    input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear();
    input.sides.push_back({47, "RenamedThird", {}, true});
    input.sides.push_back({61, "RenamedFourth", {}, false});
    input.sides.push_back({55, "UnusedFaction", {}, false});
    input.map.factions.push_back({"RenamedThird", true, false, false, {}});
    input.map.factions.push_back({"RenamedFourth", true, false, false, {}});
    input.upgrade_players = {{47, "RenamedThird", true, {47,61}, {}, {modifier.id}, {}},
        {61, "RenamedFourth", true, {47,61}, {}, {modifier.id}, {}}};
    for (const auto a : {1U, 2U, 47U, 61U}) for (const auto b : {1U, 2U, 47U, 61U}) {
        const bool allied = a == b || (a >= 47 && b >= 47);
        input.relations.push_back({a, b, allied, !allied});
    }
    campaign::ConflictRecord record; record.id = 90; record.invader = 1; record.defender = 2;
    record.map_identity = input.map.map; record.forces = {{20, 1, 1, 10}, {30, 1, 2, 12}};
    const auto plan = take(campaign::prepare_space_entry(record, input, 100, 17));
    const auto label = [&](gc::PlayerId owner) {
        const auto at = std::find_if(plan.fixture.slots.begin(), plan.fixture.slots.end(), [&](const auto& s) { return s.slot == owner; });
        if (at == plan.fixture.slots.end()) throw std::runtime_error("missing scene owner");
        return at->team;
    };
    expect(label(1) == 0 && label(2) == 1 && label(47) == 2 && label(61) == 2,
        "WGT-123: sparse background owners share their sourced allied relation class, independent of owner arithmetic");
    expect(plan.fixture.slots.size() == 4, "unused galactic factions receive no invented tactical team");
    auto mapped = input; mapped.upgrade_players.clear();
    skirmish::MapPlacement structure; structure.record = 8; structure.type = "Pad"; structure.element = "SpaceStructure";
    structure.owner_index = 3; structure.position = sim::math::Vec3{}; structure.orientation_degrees = sim::math::Vec3{};
    structure.branched_discardable = false; mapped.map.placements.push_back(structure);
    const auto mapped_entry = take(campaign::build_space_entry(record, mapped, 100, 17));
    expect(std::any_of(mapped_entry.start.units.begin(), mapped_entry.start.units.end(), [](const auto& u) {
        return u.record == 8 && u.state.owner == 47;
    }), "WGT-123/WGT-121: admitted nonparticipant map structure retains its source owner instead of disappearing");
    auto filtered = mapped; filtered.map.factions[3].can_control_planets = true;
    filtered.map.placements.back().branched_discardable = true;
    const auto filtered_plan = take(campaign::prepare_space_entry(record, filtered, 100, 17));
    expect(filtered_plan.fixture.slots.size() == 2, "a rejected map structure does not create a background side");
    auto phantom = input; phantom.upgrade_players.clear();
    phantom.map.factions.push_back({"RenamedScenery", false, true, false, {}});
    auto unbound_structure = structure; unbound_structure.record = 9;
    unbound_structure.owner_index = static_cast<std::int32_t>(phantom.map.factions.size() - 1);
    unbound_structure.branched_discardable = true; phantom.map.placements.push_back(unbound_structure);
    const auto phantom_entry = take(campaign::build_space_entry(record, phantom, 100, 17));
    expect(std::any_of(phantom_entry.start.units.begin(), phantom_entry.start.units.end(), [](const auto& u) {
        return u.record == 9;
    }), "an unbound nonplayable map owner stays on the shared phantom-owner path without refusing entry");
    phantom.map.factions.back().playable = true;
    const auto unbound_playable = take(campaign::build_space_entry(record, phantom, 100, 17));
    expect(std::none_of(unbound_playable.start.units.begin(), unbound_playable.start.units.end(), [](const auto& u) {
        return u.record == 9;
    }), "an unbound playable map owner retains the shared removal path without refusing entry");
    auto legacy_pair = input; legacy_pair.upgrade_players.clear();
    for (auto& relation : legacy_pair.relations) if (relation.player == 2 && relation.other == 1) relation.enemy = false;
    expect(static_cast<bool>(campaign::prepare_space_entry(record, legacy_pair, 100, 17)),
        "an already-admitted asymmetric combatant pair retains the existing opposing-side contract");
    auto bound_scenery = phantom; bound_scenery.map.factions.back().playable = false;
    bound_scenery.sides.push_back({77,"RenamedScenery",{},false});
    for (const auto a : {1U,2U,77U}) for (const auto b : {1U,2U,77U}) if (a == 77 || b == 77)
        bound_scenery.relations.push_back({a,b,a==b,a!=b && !(a==77 && b==2)});
    const auto legacy_map = take(campaign::build_space_entry(record, bound_scenery, 100, 17));
    expect(std::any_of(legacy_map.start.units.begin(), legacy_map.start.units.end(), [](const auto& u) { return u.record == 9; })
        && std::none_of(legacy_map.conflict.tactical.players.begin(), legacy_map.conflict.tactical.players.end(), [](const auto& p) { return p.player_id == 77; }),
        "unrepresentable map-only NPCs retain the legacy phantom path instead of blocking a stock scene");
    const_cast<units::UnitType*>(tables.find(modifier.id))->affiliation += "; RenamedScenery";
    bound_scenery.upgrade_players.push_back({77,"RenamedScenery",false,{77},{},{modifier.id},{}});
    const auto required_scenery = campaign::prepare_space_entry(record, bound_scenery, 100, 17);
    expect(!required_scenery && required_scenery.error().message.find("WGT-123 relationship seam") != std::string::npos,
        "a required planet modifier owner cannot hide an unsupported relationship behind the map-only fallback");
    auto shuffled = input; std::reverse(shuffled.sides.begin(), shuffled.sides.end());
    std::reverse(shuffled.relations.begin(), shuffled.relations.end());
    const auto shuffled_plan = take(campaign::prepare_space_entry(record, shuffled, 100, 17));
    expect(std::equal(plan.fixture.slots.begin(), plan.fixture.slots.end(), shuffled_plan.fixture.slots.begin(), shuffled_plan.fixture.slots.end(),
        [](const auto& a, const auto& b) { return a.slot == b.slot && a.team == b.team; }),
        "relation labels are stable under provider traversal permutations");
    for (const auto invalid : {0U, 1U, 2U, 3U}) {
        auto unsupported = input;
        if (invalid == 0) unsupported.relations.clear();
        if (invalid == 1) unsupported.relations.pop_back();
        if (invalid == 2) unsupported.relations.push_back(unsupported.relations.front());
        if (invalid == 3) unsupported.relations[2].enemy = false; // directed neutral-to-enemy mismatch
        const auto refused = campaign::prepare_space_entry(record, unsupported, 100, 17);
        expect(!refused && refused.error().message.find("WGT-123 relationship seam") != std::string::npos,
            "incomplete, duplicate or nonrepresentable relationships refuse at the named seam");
    }
    std::vector<std::string> reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        const auto entry = take(campaign::build_space_entry(record, input, 100, 17));
        expect(std::all_of(entry.conflict.tactical.players.begin(), entry.conflict.tactical.players.end(), [](const auto& p) {
            return p.commandable() == (p.player_id == 1 || p.player_id == 2);
        }), "nonparticipant human metadata never creates a commandable seat");
        auto content = entry.content; content.fog.reset();
        content.victory.countdown_frames = 0;
        auto battle = take(tt::TacticalSession::create(entry.conflict.tactical, content));
        const auto players = battle.snapshot()->players();
        expect(tt::players_allied(players, 47, 61) && !tt::players_hostile(players, 47, 61)
            && tt::players_hostile(players, 1, 47) && tt::players_hostile(players, 2, 47),
            "target acquisition sees third-party allies and their hostility to both combatants");
        expect(content.victory.contenders == std::vector<tt::PlayerId>{1,2}, "modifier owners are excluded from victory contenders");
        check(battle.submit({{0, 1, 0}, {30}, tt::DamagePayload{whole(1000)}}));
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) hashes.push_back(take(battle.step(executor)).state_hash.get());
        expect(battle.outcome() && battle.outcome()->winner == 1 && battle.outcome()->winner_team == 0,
            "live nonparticipant modifiers do not prevent the surviving combatant's victory");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "nonparticipant relationships and victory are deterministic on 1/2/4/8 workers");
    }
    auto allied = input;
    for (auto& relation : allied.relations) {
        relation.allied = relation.player == relation.other || (relation.player != 2 && relation.other != 2);
        relation.enemy = !relation.allied;
    }
    const auto allied_plan = take(campaign::prepare_space_entry(record, allied, 100, 17));
    expect(std::all_of(allied_plan.fixture.slots.begin(), allied_plan.fixture.slots.end(), [](const auto& s) {
        return s.team == (s.slot == 2 ? 1U : 0U);
    }), "a third-party class allied with the invader coalesces into team zero");
    auto neutral = input;
    neutral.map.factions[3].neutral = true;
    for (auto& relation : neutral.relations) if (relation.player == 47 || relation.other == 47) {
        relation.allied = relation.player == relation.other; relation.enemy = false;
    }
    const auto neutral_plan = take(campaign::prepare_space_entry(record, neutral, 100, 17));
    std::vector<tt::SnapshotPlayer> neutral_players;
    for (const auto& slot : neutral_plan.fixture.slots) neutral_players.push_back({slot.slot, slot.team, slot.slot == 47, false});
    expect(!tt::players_allied(neutral_players, 1, 47) && !tt::players_hostile(neutral_players, 1, 47)
        && !tt::players_allied(neutral_players, 47, 61) && !tt::players_hostile(neutral_players, 47, 61),
        "a faction-neutral modifier remains neutral to both combatants and other scene owners");
}
void rider_bonus(tt::TacticalContent& content) {
    tt::CommandBonusProfile bonus; bonus.type = skirmish::type_id("Commander");
    bonus.bonus.applicable = {skirmish::type_id("Ship")}; bonus.bonus.percentages[0] = Fixed::from_ratio(1, 4).value();
    content.economy.command_bonuses = {bonus};
}
void reserve_order_contract() {
    auto tables = tables_for_entry(); tables.campaign_scene = true;
    units::NamedConstant credit_cap; credit_cap.tag = "Credit_Cap_Per_Planet"; credit_cap.value = whole(10000);
    tables.constants.scalars.push_back(credit_cap);
    const_cast<units::UnitType*>(tables.find("Ship"))->production.population_value = 1;
    const_cast<units::UnitType*>(tables.find("ReserveShip"))->production.population_value = 2;
    auto input = entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear();
    input.types.front().escorts.clear();
    for (auto& faction : input.map.factions) faction.space_unit_cap = 2;
    campaign::ConflictRecord record; record.id = 90; record.invader = 1; record.defender = 2; record.map_identity = input.map.map;
    record.forces = {{20,1,1,10},{21,2,1,10},{22,1,1,10},{25,1,1,10},{26,2,1,10},{27,1,1,10},{30,1,2,12}};
    input.roster_order = {22,21,20,26,27,25,30};
    const auto plan = take(campaign::prepare_space_entry(record, input, 100, 17));
    const std::vector<sim::EntityId> expected{22,20,21,26};
    expect(plan.reserve_order == expected, "positive-cost deployment removes bucket backs; reserves walk source leftovers forward by type");
    auto entry = take(campaign::build_space_entry(record, input, 100, 17));
    campaign::SpaceRuleInputs rules; rules.players = {{1,whole(100),0,5,1,{},false,{}},{2,whole(100),0,5,1,{},true,{}}};
    rules.reserve_order = plan.reserve_order;
    check(campaign::apply_space_rules(entry, input.map, rules));
    auto setup = entry.conflict.tactical;
    expect(setup.campaign->reserves.front().id == 20, "reserve manifest remains canonical for identity lookups");
    auto unordered = setup; unordered.campaign->reserve_order.clear();
    auto default_session = take(tt::TacticalSession::create(unordered, entry.content));
    expect(default_session.ledgers().front().pool_tokens == std::vector<std::uint64_t>({20,21,22,26}), "omitted order preserves the default ascending-ID pool");
    for (const auto& invalid : {std::vector<sim::EntityId>{22,20,21}, {22,20,21,21}, {22,20,21,99}}) {
        auto bad = setup; bad.campaign->reserve_order = invalid;
        expect(!tt::validate_setup(bad), "reserve order rejects omitted, repeated and unknown identities");
    }
    std::vector<std::string> reference;
    std::vector<std::uint8_t> replay_reference;
    for (const auto workers : {1U,2U,4U,8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        tt::TacticalReplay replay; replay.setup = setup;
        const auto bytes = take(tt::write_replay(replay));
        expect(take(tt::parse_replay(bytes)) == replay, "campaign source pool order survives replay encoding");
        auto session = take(tt::TacticalSession::from_replay(replay, entry.content));
        expect(session.ledgers().front().pool_tokens == expected, "tactical ledger consumes recorded source order");
        check(session.stage_remove(25)); check(session.stage_remove(27));
        (void)take(session.step(executor));
        const auto identity = checkpoint::tactical_identities(setup.content_identity);
        const auto saved = take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(), checkpoint::default_seats(setup), identity));
        checkpoint::TacticalSessionFactory factory = [&](const tt::TacticalReplay& source) { return tt::TacticalSession::from_replay(source, entry.content); };
        auto restored = take(checkpoint::load_tactical_checkpoint(saved, identity, factory, executor));
        expect(restored.session.ledgers().front().pool_tokens == expected, "fresh checkpoint preserves source order without rebuilding the fleet");
        const tt::PlayerCommand reinforce{{1,1,0},{},tt::ReinforcePayload{skirmish::type_id("Ship"),{whole(-5000),{},{}},session.ledgers().front().pool_tokens.front(),{}}};
        check(session.submit(reinforce)); check(restored.session.submit(reinforce));
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) {
            const auto step = take(session.step(executor)); hashes.push_back(step.state_sha256);
            expect(take(restored.session.step(executor)).state_sha256 == step.state_sha256, "save/load reinforcement continuation matches exactly");
        }
        expect(session.ledgers().front().pool_tokens == std::vector<std::uint64_t>({20,21,26}), "first published reinforcement token consumes source-first identity 22 instead of lowest ID 20");
        if (workers == 1) { reference = hashes; replay_reference = bytes; }
        else expect(reference == hashes && replay_reference == bytes, "reserve-order replay and continuation agree on 1/2/4/8 workers");
    }
}
gc::GalacticSetup galaxy_for_entry() {
    gc::GalacticSetup galaxy; galaxy.seed = 17;
    galaxy.content_identity.fill(17); // ADR-017: deliberately different from tactical tables.
    galaxy.players = {{1, 1, 0}, {2, 2, 0}};
    galaxy.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    galaxy.planets = {{1, {}, 1}, {2, {whole(12), {}, {}}, 2}};
    galaxy.fleets = {{10, 1, 1, {{20, 1}, {21, 2}, {22, 1}, {23, 3}}}, {12, 2, 2, {{30, 1}}}};
    galaxy.rules.maximum_fleet_movement_distance = whole(12);
    galaxy.rules.fiscal_cycle_frames = 1;
    galaxy.fleet_control.emplace();
    galaxy.movement.emplace(); auto& movement = *galaxy.movement; movement.mode_modifier = whole(1);
    for (gc::TypeId id = 1; id <= 6; ++id) {
        galaxy.fleet_control->types.push_back({id, false, true, false});
        movement.types.push_back({id, whole(2), whole(100), 0, true, false, false, false, id == 3});
    }
    movement.players = {{1, whole(1), whole(1), false}, {2, whole(1), whole(1), false}};
    movement.planets = {{1, whole(1), false}, {2, whole(1), false}};
    movement.contents = {{23, {{24, 4}}}};
    for (const auto& p : galaxy.players) for (const auto& planet : galaxy.planets)
        movement.access.push_back({p.player_id, planet.id, true, p.player_id == 1 && planet.id == 2});
    galaxy.fiscal = gc::FiscalSetup{whole(2500), {{1, true, whole(1)}, {2, false, whole(1)}}, {{1, {}}, {2, {}}}};
    galaxy.player_economy = gc::PlayerEconomySetup{{{1, {}, whole(10000), 0, 5}, {2, {}, whole(10000), 0, 5}}};
    galaxy.production.emplace(); galaxy.production->maximum_human_queue = 5;
    galaxy.production->players = {{1, true, false, true, whole(100), {}, {}, {}, {}}, {2, false, true, false, whole(100), {}, {}, {}, {}}};
    for (const auto& planet : galaxy.planets) {
        gc::PlanetProduction production; production.planet = planet.id; production.target_owner = planet.owner;
        if (planet.id == 2) production.objects = {{40, 5, 2, {}, gc::ProductionDomain::space, gc::ProductionKind::structure},
            {41, 7, 2, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
        galaxy.production->planets.push_back(production);
    }
    return galaxy;
}
void ground_passenger_contracts() {
    auto tables = tables_for_entry(); tables.campaign_scene = true;
    units::NamedConstant credit_cap; credit_cap.tag = "Credit_Cap_Per_Planet";
    credit_cap.value = whole(10000); tables.constants.scalars.push_back(credit_cap);
    auto input = entry_inputs(tables);
    for (auto& faction : input.map.factions) faction.space_unit_cap = 2;
    input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear();
    campaign::SpaceEntryType company; company.galactic = 8; company.tactical = "RenamedSurfaceCompany";
    campaign::SpaceEntryType member; member.galactic = 9; member.tactical = "RenamedSurfaceMember";
    input.types.push_back(company); input.types.push_back(member);
    tt::CarriedObject nested; nested.entity_id = 25; nested.type_id = skirmish::type_id(member.tactical);
    campaign::ConflictForce nested_force{25, 9, 1, 10}; nested_force.carrier = 24; nested_force.tactical_type = nested.type_id;
    input.riders = {{{24, 8, 1, 10}, company.tactical, 23, {nested}, {nested_force}},
        {{28, 9, 1, 10}, member.tactical, 26, {}, {}}, {{29, 9, 1, 10}, member.tactical, 27, {}, {}}};
    campaign::ConflictRecord record; record.id = 90; record.planet = 1; record.invader = 1; record.defender = 2;
    record.map_identity = input.map.map;
    record.forces = {{20, 1, 1, 10}, {23, 3, 1, 10}, {26, 3, 1, 10}, {27, 3, 1, 10}, {30, 1, 2, 12}};
    auto galaxy_setup = galaxy_for_entry(); galaxy_setup.fleets.front().members = {{20, 1}, {23, 3}, {26, 3}, {27, 3}};
    galaxy_setup.movement->contents = {{23, {{24, 8}}}, {24, {{25, 9}}}, {26, {{28, 9}}}, {27, {{29, 9}}}};
    for (const auto id : {8U, 9U}) {
        galaxy_setup.fleet_control->types.push_back({id, false, true, false});
        galaxy_setup.movement->types.push_back({id, whole(1), {}, 0, true, false, id == 8});
    }
    std::vector<std::string> reference;
    std::vector<std::uint8_t> reference_replay;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const platform::ThreadWorkerAdapter executor(workers);
        // WGT-07/122: ordering selects outer transports before riders attach.
        auto combined_record = record;
        combined_record.forces.push_back({32, 3, 1, 10});
        auto combined_input = input;
        combined_input.roster_order = {20, 23, 26, 32, 27, 30};
        combined_input.riders.push_back({{33, 9, 1, 10}, member.tactical, 32, {}, {}});
        const auto combined_plan = take(campaign::prepare_space_entry(combined_record, combined_input, 100, 17));
        auto bare_input = combined_input;
        std::erase_if(bare_input.riders, [](const auto& rider) { return rider.transport == 27 || rider.transport == 32; });
        const auto bare_plan = take(campaign::prepare_space_entry(combined_record, bare_input, 100, 17));
        expect(combined_plan.reserve_order == std::vector<sim::EntityId>{32, 27}
            && combined_plan.reserve_order == bare_plan.reserve_order,
            "ground riders leave outer transport leftover order unchanged");
        for (const auto& outer : bare_plan.conflict.forces) {
            const auto attached = std::find_if(combined_plan.conflict.forces.begin(), combined_plan.conflict.forces.end(),
                [&](const auto& force) { return force.id == outer.id; });
            expect(attached != combined_plan.conflict.forces.end() && attached->role == outer.role,
                "ground riders leave outer transport deployment unchanged");
        }
        auto combined = take(campaign::build_space_entry(combined_record, combined_input, 100, 17));
        campaign::SpaceRuleInputs combined_rules; combined_rules.reserve_order = combined_plan.reserve_order;
        for (const auto owner : {1U, 2U}) {
            campaign::SpaceRulePlayer player; player.player = owner; player.max_tech = 5; combined_rules.players.push_back(player);
        }
        for (const auto carrier : {27U, 32U}) {
            tt::UnitState outer; outer.entity_id = carrier;
            tt::CarriedObject rider; rider.entity_id = carrier == 32 ? 33 : 29;
            rider.type_id = skirmish::type_id(member.tactical);
            expect(tt::contain_object(outer, rider, false), "combined reserve rider attaches to its carrier");
            combined_rules.reserve_attachments.push_back({carrier, outer.contained});
        }
        check(campaign::apply_space_rules(combined, combined_input.map, combined_rules));
        auto combined_battle = take(tt::TacticalSession::create(combined.conflict.tactical, combined.content));
        expect(combined_battle.ledgers().front().pool_tokens == std::vector<std::uint64_t>{32, 27},
            "only outer transports receive ordered reserve tokens on 1/2/4/8 workers");
        expect(combined.conflict.tactical.campaign->reserves.back().contained.front().entity_id == 33,
            "the ordered outer transport retains its ground rider identity");
        auto entry = take(campaign::build_space_entry(record, input, 100, 17));
        campaign::SpaceRuleInputs rules;
        for (const auto owner : {1U, 2U}) {
            campaign::SpaceRulePlayer player; player.player = owner; player.max_tech = 5; rules.players.push_back(player);
        }
        tt::UnitState reserve; reserve.entity_id = 27;
        tt::CarriedObject passenger; passenger.entity_id = 29; passenger.type_id = skirmish::type_id(member.tactical);
        expect(tt::contain_object(reserve, passenger, false), "identity-only reserve passenger attaches");
        rules.reserve_attachments.push_back({27, reserve.contained});
        check(campaign::apply_space_rules(entry, input.map, rules));
        const auto& content = entry.content;
        expect(entry.conflict.tactical.campaign->reserves.size() == 1
            && entry.conflict.tactical.campaign->reserves.front().id == 27,
            "WGT-122: transports use a count allowance independently of ship population");
        const auto host = std::find_if(entry.conflict.tactical.units.begin(), entry.conflict.tactical.units.end(), [](const auto& u) { return u.entity_id == 23; });
        expect(host != entry.conflict.tactical.units.end() && host->type_id == skirmish::type_id("Transport")
            && host->contained.front().entity_id == 24 && host->contained.front().members.front().entity_id == 25,
            "WGT-122: renamed ground types stay contained without a spatial profile");
        auto standalone = record; standalone.forces.push_back({31, 8, 2, 12});
        expect(!campaign::build_space_entry(standalone, input, 100, 17), "unprepared standalone ground object still refuses");
        auto battle = take(tt::TacticalSession::create(entry.conflict.tactical, content));
        check(battle.submit({{0, 1, 0}, {23}, tt::DamagePayload{whole(1000)}}));
        const auto identity = checkpoint::tactical_identities(entry.conflict.tactical.content_identity);
        const auto bytes = take(checkpoint::save_tactical_checkpoint(battle, battle.pending_commands(), checkpoint::default_seats(entry.conflict.tactical), identity));
        const checkpoint::TacticalSessionFactory factory = [&](const auto& replay) { return tt::TacticalSession::from_replay(replay, content); };
        auto restored = take(checkpoint::load_tactical_checkpoint(bytes, identity, factory, executor));
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) {
            const auto stepped = take(battle.step(executor)); hashes.push_back(stepped.state_sha256);
            expect(take(restored.session.step(executor)).state_sha256 == stepped.state_sha256,
                "ground passenger save/load continues with identical loss state");
        }
        const auto replay = take(tt::write_replay(battle.record()));
        auto replayed = take(tt::TacticalSession::from_replay(take(tt::parse_replay(replay)), content));
        for (unsigned tick = 0; tick < 3; ++tick) (void)take(replayed.step(executor));
        expect(replayed.state_sha256() == battle.state_sha256(), "ground passenger replay retains nested identities");
        auto galaxy = take(gc::GalacticSession::create(galaxy_setup));
        campaign::BattleResult result; result.conflict = 90; result.winner = 2; result.retreat_player = 1;
        for (const auto& force : entry.conflict.forces) result.forces.push_back({force.id,
            force.id == 23 ? campaign::ForceDisposition::destroyed : campaign::ForceDisposition::retreated});
        const auto returning = take(campaign::plan_tactical_return(entry.conflict, result, *galaxy.snapshot()));
        expect(returning.destroyed == std::vector<sim::EntityId>{23, 24, 25}
            && returning.retreat_fleets == std::vector<gc::FleetId>{10},
            "transport losses close over ground constituents while surviving and reserve companies retreat");
        expect(!campaign::tactical_force_alive(entry.conflict.forces[2], battle.units()),
            "destroyed ground passenger is absent from the live contained tree");
        result.retreat_player.reset();
        campaign::TacticalReturnState state;
        check(campaign::apply_tactical_return(galaxy, entry.conflict,
            take(campaign::plan_tactical_return(entry.conflict, result, *galaxy.snapshot())), state, executor));
        const auto after = galaxy.snapshot();
        expect(after->movement() && after->movement()->contents == std::vector<gc::MovementContents>{
            {26, {{28, 9}}}, {27, {{29, 9}}}},
            "return removes destroyed company constituents and keeps deployed/reserve company identities");
        if (workers == 1) { reference = hashes; reference_replay = replay; }
        else expect(reference == hashes && reference_replay == replay, "ground company hashes and replay bytes equal on 1/2/4/8 workers");
    }
}
void admitted_cycle() {
    const auto tables = tables_for_entry(); const auto input = entry_inputs(tables); const auto galaxy = galaxy_for_entry();
    const campaign::GameSession::ContentResolver content = [&](const auto& record) {
        auto bound = campaign::space_entry_content(record.tactical, input.map);
        if (bound) rider_bonus(bound.value());
        return bound;
    };
    const campaign::GameSession::ConflictService admission = [&](const auto& snapshot, const auto& event, sim::EntityId next) {
        campaign::ConflictRecord record; record.id = next; record.planet = event.value;
        record.invader = 1; record.defender = 2; record.map_identity = input.map.map; record.station_level = 2;
        record.tactical.first_free_entity_id = next + 1; record.tactical.content_identity = galaxy.content_identity;
        record.seats = {{{1}, 1}, {{2}, 2}};
        for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == event.value)
            for (const auto& member : fleet.members) record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        return core::Result<std::optional<campaign::RecordedAdmission>>::success(campaign::RecordedAdmission{
            snapshot.completed_tick(), std::move(record), {campaign::ConflictRoute::pending_tactical, false, true}});
    };
    std::vector<std::string> reference;
    std::vector<std::uint8_t> reference_replay;
    for (const auto workers : std::array<std::size_t, 4>{1, 2, 4, 8}) {
        const platform::ThreadWorkerAdapter executor(workers);
        auto game = take(campaign::GameSession::create(galaxy));
        game.set_conflict_service(admission, content);
        game.set_entry_resolver([&](const auto& record, const auto&, auto next) {
            auto built = campaign::build_space_entry(record, input, next, galaxy.seed);
            if (!built) return core::Result<campaign::ResolvedConflictEntry>::failure(built.error());
            rider_bonus(built.value().content);
            return core::Result<campaign::ResolvedConflictEntry>::success({built.value().conflict, built.value().content});
        });
        check(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}));
        std::vector<std::string> hashes;
        while (!game.pending_conflict() && game.completed_tick() < 20) hashes.push_back(take(game.step(executor)).state_hash.get());
        expect(game.pending_conflict() != nullptr, "live movement admits an unresolved over-cap conflict");
        if (!game.pending_conflict()) return;
        const auto admitted = *game.pending_conflict();
        expect(!admitted.tactical.skirmish, "admission has no synthetic tactical setup");
        hashes.push_back(take(game.step(executor)).state_hash.get());
        check(game.submit({{game.completed_tick(), {1}, 2}, gc::BattleChoice{admitted.id, true}}));
        hashes.push_back(take(game.step(executor)).state_hash.get());
        expect(game.battle() && game.conflict(), "real entry resolver stages the prepared child");
        if (!game.battle()) return;
        const auto resolved = *game.conflict();
        const auto hash_before_presentation = game.state_sha256();
        const auto visual = take(campaign::space_entry_presentation_start(resolved, input.map));
        const auto fresh_visual = take(campaign::space_entry_presentation_start(resolved, input.map));
        expect(visual.setup == resolved.tactical && fresh_visual.setup == visual.setup
            && visual.markers.empty() && visual.units.size() == resolved.tactical.units.size(),
            "fresh-provider visual projection uses exact actors on a campaign map without lobby metadata");
        expect(std::equal(visual.units.begin(), visual.units.end(), resolved.tactical.units.begin(),
            [](const auto& row, const auto& actor) { return row.state == actor; })
            && visual.players.front().faction == "Rebel" && visual.players.front().human,
            "projected visual actors and faction bindings preserve sparse native identities");
        expect(game.state_sha256() == hash_before_presentation,
            "visual projection leaves parent and retained child state hashes unchanged");
        expect(resolved.map_identity == input.map.map && resolved.station_level == 2,
            "planet-bound map and current station level survive entry");
        const auto force = [&](sim::EntityId id) -> const campaign::ConflictForce& {
            return *std::find_if(resolved.forces.begin(), resolved.forces.end(), [id](const auto& f) { return f.id == id; });
        };
        expect(force(21).role == campaign::ForceRole::reinforcement && force(21).type == 2
            && force(21).tactical_type == skirmish::type_id("ReserveShip"), "over-cap reserve keeps its GC identity and explicit tactical binding");
        expect(force(23).role == campaign::ForceRole::retained && force(24).role == campaign::ForceRole::rider
            && force(24).carrier == 20, "empty transport survives without spawning; strict flagship tie carries persistent rider");
        expect(std::count_if(resolved.forces.begin(), resolved.forces.end(), [](const auto& f) { return f.type == 2 && f.role == campaign::ForceRole::escort && f.destroy_after_tactical && !f.parent; }) == 3,
            "each initial ship creates a child-only escort discarded on return");
        expect(resolved.structure_markers == std::vector<std::pair<sim::EntityId, std::uint32_t>>{{41, 6}},
            "orbital structure remembers its authored marker by persistent structure identity");
        const auto station = std::find_if(resolved.tactical.units.begin(), resolved.tactical.units.end(), [&](const auto& u) { return u.entity_id == resolved.station; });
        expect(station != resolved.tactical.units.end() && station->owner == 2 && station->garrison_enabled,
            "current station is defender-owned with authored garrison enabled");
        const auto spawner = std::find_if(game.battle()->motion().squadrons.spawners.begin(), game.battle()->motion().squadrons.spawners.end(), [](const auto& p) { return p.type_id == skirmish::type_id("Station"); });
        expect(spawner != game.battle()->motion().squadrons.spawners.end() && spawner->entries.front().reserve == -1,
            "campaign station uses its authored unlimited garrison reserve");
        expect(resolved.tactical.first_free_entity_id && *resolved.tactical.first_free_entity_id > resolved.station,
            "shared frontier covers station, defences and battle-only identities");
        expect(std::any_of(resolved.tactical.units.begin(), resolved.tactical.units.end(), [](const auto& u) {
            return u.type_id == skirmish::type_id("Weapon") && u.owner == 2 && u.position.x == whole(1800);
        }), "planetary weapon uses first authored source and planet owner");
        const auto carrier = std::find_if(resolved.tactical.units.begin(), resolved.tactical.units.end(), [](const auto& u) { return u.entity_id == 20; });
        expect(carrier != resolved.tactical.units.end() && carrier->contained.size() == 1
            && carrier->contained[0].entity_id == 24 && carrier->contained[0].parent == 20,
            "rider is contained, outside the spatial roster, with exact parent identity");
        const auto save = take(checkpoint::save_game_checkpoint(game, checkpoint::game_identities(galaxy)));
        checkpoint::GameLoadOptions load; load.content = content;
        auto restored = take(checkpoint::load_game_checkpoint(save, checkpoint::game_identities(galaxy), load, executor));
        expect(restored.session.state_sha256() == game.state_sha256(),
            "fresh checkpoint restores permanent orbital roles and retained source links");
        for (int tick = 0; tick < 40; ++tick) hashes.push_back(take(game.step(executor)).state_hash.get());
        const auto live = game.battle()->snapshot();
        expect(std::any_of(live->instances().begin(), live->instances().end(), [](const auto& u) {
            return u.type_id == skirmish::type_id("Craft") && u.owner == 2;
        }), "defender station launches authored garrison craft through the shared launch service");
        expect(std::any_of(live->instances().begin(), live->instances().end(), [](const auto& u) {
            return u.entity_id == 22 && u.durability && u.durability->max_hull == whole(125);
        }), "synthetic initial contained rider registers the existing Combat_Bonus_Ability consumer");
        auto replay = *game.battle_record(); const auto bytes = take(tt::write_replay(replay));
        expect(take(tt::parse_replay(bytes)) == replay, "exported child replay retains initial carrier tree and sparse IDs");
        auto replayed = take(tt::TacticalSession::from_replay(replay, take(content(resolved))));
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) (void)take(replayed.step(executor));
        expect(replayed.state_sha256() == game.battle()->state_sha256(), "ordinary child replay reproduces live child state");
        const auto recording = game.recording();
        expect(recording.conflicts.size() == 1 && recording.conflicts.front().conflict == resolved,
            "history stores resolved entry, not the unresolved admission manifest");
        auto parent_replay = take(campaign::GameSession::from_recording(recording, content));
        for (std::uint64_t tick = 0; tick < recording.commands.final_tick_count; ++tick) (void)take(parent_replay.step(executor));
        expect(parent_replay.state_sha256() == game.state_sha256(),
            "parent recording consumes the resolved selection without re-running entry placement");
        if (workers == 1) { reference = hashes; reference_replay = bytes; }
        else { expect(reference == hashes, "admitted entry and child equal on 1/2/4/8 workers"); expect(reference_replay == bytes, "exported child bytes equal on 1/2/4/8 workers"); }
    }
}
void preparation_contracts() {
    const auto tables = tables_for_entry(); auto input = entry_inputs(tables);
    campaign::ConflictRecord record; record.id = 90; record.invader = 1; record.defender = 2;
    record.station_level = 2; record.map_identity = input.map.map;
    record.forces = {{20, 1, 1, 10}, {21, 2, 1, 10}, {22, 1, 1, 10}, {23, 3, 1, 10}, {30, 1, 2, 12}};
    auto prepared = take(campaign::prepare_space_entry(record, input, 100, 17));
    expect(prepared.marker_draws == 1, "multiple station markers consume one synchronized draw");
    expect(std::all_of(prepared.placements.begin(), prepared.placements.end(), [](const auto& p) {
        return p.force != 20 && p.force != 22 ? true : p.marker == 4;
    }), "closest attacker marker wins with authored list tie order");
    auto repeat = take(campaign::prepare_space_entry(record, input, 100, 17));
    expect(prepared.conflict == repeat.conflict && prepared.first_free == repeat.first_free, "preparation is deterministic and side-effect free");
    auto remembered = input;
    remembered.map.placements.push_back(marker(8, "Orbital Special Structure Position", 2000));
    remembered.structures.back().remembered_marker = 8;
    expect(take(campaign::prepare_space_entry(record, remembered, 100, 17)).conflict.structure_markers
        == std::vector<std::pair<sim::EntityId, std::uint32_t>>{{41, 8}}, "remembered orbital marker precedes free fallback markers");
    auto unique = input; unique.types[3].unique_space_unit = "ReserveShip";
    const auto shipped = take(campaign::build_space_entry(record, unique, 100, 17));
    const auto passenger = std::find_if(shipped.conflict.forces.begin(), shipped.conflict.forces.end(), [](const auto& f) { return f.id == 24; });
    expect(passenger->carrier >= 100 && passenger->role == campaign::ForceRole::rider,
        "unique passenger ship allocates only its container and preserves the original rider identity");
    const auto passenger_ship = std::find_if(shipped.conflict.forces.begin(), shipped.conflict.forces.end(), [&](const auto& f) { return f.id == passenger->carrier; });
    expect(passenger_ship->role == campaign::ForceRole::escort && passenger_ship->destroy_after_tactical && passenger_ship->parent == 0,
        "unique passenger ship keeps a temporary role without claiming a persistent fleet member");
    auto nested = input;
    tt::CarriedObject member; member.entity_id = 25; member.type_id = skirmish::type_id("Commander"); member.generic_hero = true;
    nested.riders.front().members = {member};
    campaign::ConflictForce member_force{25, 4, 1, 10}; member_force.carrier = 24; member_force.tactical_type = member.type_id;
    nested.riders.front().member_forces = {member_force};
    const auto tree = take(campaign::build_space_entry(record, nested, 100, 17));
    const auto carrier = std::find_if(tree.start.units.begin(), tree.start.units.end(), [](const auto& u) { return u.state.entity_id == 20; });
    expect(carrier->state.contained.front().members.front().entity_id == 25
        && carrier->state.contained.front().members.front().parent == 24,
        "nested passengers retain their identities and immediate parent");
    auto empty = input; empty.riders.clear();
    const auto empty_plan = take(campaign::prepare_space_entry(record, empty, 100, 17));
    expect(std::find_if(empty_plan.conflict.forces.begin(), empty_plan.conflict.forces.end(), [](const auto& f) { return f.id == 23; })->role
        == campaign::ForceRole::retained, "empty transport is retained without a tactical object or reserve");
    auto barred = input; barred.types[1].space_allowed = false;
    const auto barred_plan = take(campaign::prepare_space_entry(record, barred, 100, 17));
    expect(std::find_if(barred_plan.conflict.forces.begin(), barred_plan.conflict.forces.end(), [](const auto& f) { return f.id == 21; })->role
        == campaign::ForceRole::retained, "units barred from space do not enter the reserve pool");
    input.map.placements.erase(input.map.placements.begin() + 4);
    expect(take(campaign::prepare_space_entry(record, input, 100, 17)).marker_draws == 0, "single station marker consumes no draw");
    auto missing = input; std::erase_if(missing.map.placements, [](const auto& p) { return p.type == "Attacker Entry Position"; });
    const auto no_attack = take(campaign::build_space_entry(record, missing, 100, 17));
    expect(std::none_of(no_attack.start.units.begin(), no_attack.start.units.end(), [](const auto& u) { return u.state.owner == 1; })
        && std::none_of(no_attack.start.markers.begin(), no_attack.start.markers.end(), [](const auto& m) { return m.player == 1; }),
        "WGT-52: absent attacker markers skip initial objects and their facing update");
    auto no_defender = input;
    std::erase_if(no_defender.map.placements, [](const auto& p) { return p.type == "Defending Forces Position"; });
    const auto no_defence = take(campaign::prepare_space_entry(record, no_defender, 100, 17));
    expect(std::none_of(no_defence.placements.begin(), no_defence.placements.end(), [](const auto& p) { return p.force == 30; })
        && std::all_of(no_defence.placements.begin(), no_defence.placements.end(), [](const auto& p) {
            return p.force != 20 && p.force != 22 ? true : p.marker == 1;
        }) && std::count_if(no_defence.map.placements.begin(), no_defence.map.placements.end(), [](const auto& p) {
            return p.type == "Attacker Entry Position" && (p.record == 1 || p.record == 4);
        }) == 2,
        "WGT-53: absent defenders skip their fleet and preserve the whole authored attacker sequence");
    auto no_station = input;
    std::erase_if(no_station.map.placements, [](const auto& p) { return p.type == "Space Station Position"; });
    const auto station_skip = take(campaign::build_space_entry(record, no_station, 100, 17));
    expect(station_skip.conflict.station == 0 && take(campaign::prepare_space_entry(record, no_station, 100, 17)).marker_draws == 0,
        "WGT-52: absent station markers skip the station without a random draw");
    auto no_source = input;
    std::erase_if(no_source.map.placements, [](const auto& p) { return p.special_weapon_source; });
    const auto origin = take(campaign::prepare_space_entry(record, no_source, 100, 17));
    expect(std::any_of(origin.fixed_poses.begin(), origin.fixed_poses.end(), [](const auto& p) {
        return p.second.type == "campaign weapon source" && p.second.position == sim::math::Vec3{}
            && p.second.orientation_degrees == sim::math::Vec3{};
    }), "WGT-54: an eligible planetary weapon without a source uses zero position and facing");
    auto wrong = record; wrong.forces[0].type = 999;
    expect(!campaign::prepare_space_entry(wrong, input, 100, 17), "unbound GC type refuses rather than treating numeric IDs as tactical IDs");
    auto no_orbit = input; std::erase_if(no_orbit.map.placements, [](const auto& p) { return p.type == "Orbital Special Structure Position"; });
    auto skipped = take(campaign::prepare_space_entry(record, no_orbit, 100, 17));
    expect(std::none_of(skipped.conflict.forces.begin(), skipped.conflict.forces.end(), [](const auto& f) { return f.type == 5; }),
        "WGT-11 structure gate includes planetary weapons");
}
void scene_contracts() {
    auto tables = tables_for_entry(); auto input = entry_inputs(tables);
    campaign::ConflictRecord record; record.id = 90; record.planet = 2; record.invader = 1; record.defender = 2;
    record.map_identity = input.map.map; record.station_level = 1;
    record.forces = {{20, 1, 1, 10}, {21, 3, 1, 10}, {22, 3, 1, 10}, {30, 1, 2, 12}};
    input.types[0].escorts.clear(); input.riders.clear();
    input.map.placements.push_back(marker(8, "Shared specific", 2100));
    input.map.placements.back().marker_for.push_back({"Pad", {}});
    auto expansion = input.types.back(); expansion.galactic = 8; expansion.every_marker_type = "Pad";
    input.types.push_back(expansion);
    input.structures = {{41, 7, 2, 8}, {42, 8, 2, {}}};
    const auto plan = take(campaign::prepare_space_entry(record, input, 100, 17));
    const auto expanded = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(), [](const auto& f) { return f.parent == 42; });
    const auto ordinary = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(), [](const auto& f) { return f.parent == 41; });
    expect(expanded != plan.conflict.forces.end() && ordinary != plan.conflict.forces.end()
        && expanded->role == campaign::ForceRole::orbital && !expanded->destroy_after_tactical,
        "WGT-115: permanent every-marker copies retain source identities");
    const auto marker_of = [&](const auto& p, sim::EntityId id) { return std::find_if(p.placements.begin(), p.placements.end(), [id](const auto& v) { return v.force == id; })->marker; };
    expect(marker_of(plan, expanded->id) == 8 && marker_of(plan, ordinary->id) == 6,
        "every-marker expansion consumes shared specific markers before remembered placement");
    const auto first_ship = std::find_if(plan.placements.begin(), plan.placements.end(), [](const auto& p) { return p.force == 20; });
    expect(plan.placements.front().force == plan.conflict.station && first_ship->creation_order > 2,
        "station and permanent structures precede ordinary initial forces");
    auto transport_input = input;
    transport_input.structures.clear(); transport_input.station_type.clear(); record.station_level = 0;
    std::erase_if(transport_input.map.placements, [](const auto& p) { return p.type == "Defending Forces Position"; });
    transport_input.riders = {{{24, 4, 1, 10}, "Commander", 21, {}, {}}, {{25, 4, 1, 10}, "Commander", 21, {}, {}}, {{26, 4, 1, 10}, "Commander", 22, {}, {}}};
    transport_input.types[3].attach_to_flagship = false;
    const auto stowed = take(campaign::prepare_space_entry(record, transport_input, 100, 17));
    expect(marker_of(stowed, 21) == 1 && marker_of(stowed, 22) == 4 && marker_of(stowed, 20) == 1,
        "WGT-114: multiple stowed passengers advance once per transport, then wrap");
    transport_input.types[3].attach_to_flagship = true;
    const auto deferred = take(campaign::prepare_space_entry(record, transport_input, 100, 17));
    expect(marker_of(deferred, 20) == 1 && deferred.riders.front().force.carrier == 20,
        "all-deferred flagship passengers retain the same marker cursor");
    auto absent_rating = transport_input; absent_rating.types[0].purchase_type = skirmish::type_id("Unbound_Source_Profile");
    expect(!campaign::prepare_space_entry(record, absent_rating, 100, 17), "missing flagship source durability refuses admission");
    transport_input.types[0].source_flagship_rating = Fixed{};
    const auto zero = take(campaign::prepare_space_entry(record, transport_input, 100, 17));
    expect(zero.riders.front().force.carrier == 21 && marker_of(zero, 22) == 4,
        "WGT-119: zero source rating selects no flagship despite positive deployed hull");
    // Three independent upgrade families, operating on the scene before ordinary ships.
    units::UnitType upgrade; upgrade.id = "Upgrade"; upgrade.affiliation = "Rebel, Empire";
    upgrade.production.upgrade_object = true; upgrade.production.next_upgrade = "Upgrade2";
    tables.units.push_back(upgrade); upgrade.id = "Upgrade2"; upgrade.production.next_upgrade.clear(); tables.units.push_back(upgrade);
    auto* station = const_cast<units::UnitType*>(tables.find("Station"));
    station->production.buildable_campaign = {{"Rebel", {"Upgrade"}}};
    input = entry_inputs(tables); input.riders.clear(); input.types[0].escorts.clear();
    input.sides.push_back({3, "Rebel", {}, false});
    input.upgrade_players = {{1, "Rebel", true, {1,2}, {"Upgrade"}, {}, {}}, {2, "Empire", true, {1,2}, {}, {}, {}},
        {3, "Rebel", true, {3}, {}, {"Upgrade2", "Upgrade2"}, {}}};
    for (const auto a : {1U,2U,3U}) for (const auto b : {1U,2U,3U}) input.relations.push_back({a,b,a==b,a!=b});
    record.station_level = 1; record.upgrades = {{skirmish::type_id("Upgrade"), 20, 1}};
    const auto up_plan = take(campaign::prepare_space_entry(record, input, 100, 17));
    expect(std::all_of(up_plan.conflict.forces.begin(), up_plan.conflict.forces.end(), [&](const auto& force) {
        return force.id < up_plan.first_free && force.parent < up_plan.first_free && force.carrier < up_plan.first_free;
    }) && std::all_of(up_plan.placements.begin(), up_plan.placements.end(), [&](const auto& p) { return p.force < up_plan.first_free; }),
        "modifier-bearing scene reserves every force, parent, carrier and placement below plan.first_free");
    auto entry = take(campaign::build_space_entry(record, input, 100, 17));
    // WGT-14/WGCH-163: rows with no upgrade object in this layer are skipped, never a reason to refuse the battle.
    auto unbound_record = record;
    const campaign::ReturnedUpgrade galactic_only_a{skirmish::type_id("Galactic_Only_Upgrade"), up_plan.conflict.planet, 1},
        galactic_only_b{skirmish::type_id("Galactic_Only_Upgrade"), up_plan.conflict.planet, 2},
        not_an_upgrade{skirmish::type_id("Ship"), up_plan.conflict.planet, 1},
        nonparticipant{skirmish::type_id("Galactic_Only_Upgrade"), up_plan.conflict.planet, 3}; // owner is neither invader nor defender
    unbound_record.upgrades.push_back(galactic_only_a);
    unbound_record.upgrades.push_back(not_an_upgrade);
    unbound_record.upgrades.push_back(galactic_only_b);
    unbound_record.upgrades.push_back(nonparticipant);
    const auto unbound_entry = take(campaign::build_space_entry(unbound_record, input, 100, 17));
    expect(unbound_entry.scene_upgrades == entry.scene_upgrades,
        "galactic-only and non-upgrade retained rows leave the admitted scene upgrades unchanged");
    expect(campaign::space_entry_unbound_upgrades(unbound_record, tables)
        == std::vector<campaign::ReturnedUpgrade>{galactic_only_a, not_an_upgrade, galactic_only_b, nonparticipant}
        && campaign::space_entry_unbound_upgrades(record, tables).empty(),
        "the return keeps exactly the rows entry did not instantiate");
    auto absent_owner = input; absent_owner.upgrade_players.erase(absent_owner.upgrade_players.begin());
    auto absent_entry = entry;
    expect(!campaign::space_entry_upgrades(record, absent_owner, up_plan, absent_entry.start), "missing retained owner is refused");
    auto missing_binding = entry; std::erase_if(missing_binding.start.setup.players, [](const auto& p) { return p.player_id == 3; });
    expect(!campaign::space_entry_upgrades(record, input, up_plan, missing_binding.start), "planet modifier needs a sourced player/team binding");
    const auto& upgrades = entry.scene_upgrades;
    expect(std::count_if(upgrades.begin(), upgrades.end(), [&](const auto& u) { return u.owner == 1 && u.holder == up_plan.conflict.station; }) == 1,
        "WGT-116: playable allied station uses upgrade-owner faction list before ordinary ships exist");
    expect(std::count_if(upgrades.begin(), upgrades.end(), [](const auto& u) { return u.owner == 3; }) == 2,
        "WGT-118: nonparticipant planet lists preserve duplicate higher-level entries");
    auto independent = input; independent.upgrade_players.front().allies = {1};
    auto origin_entry = take(campaign::build_space_entry(record, independent, 100, 17));
    const auto& origin_upgrades = origin_entry.scene_upgrades;
    expect(std::any_of(origin_upgrades.begin(), origin_upgrades.end(), [&](const auto& u) {
        const auto at = std::find_if(origin_entry.start.units.begin(), origin_entry.start.units.end(), [&](const auto& actor) { return actor.state.entity_id == u.holder; });
        return u.owner == 1 && at != origin_entry.start.units.end() && at->state.type_id == u.type && at->state.position == sim::math::Vec3{};
    }), "WGT-117: no holder still creates galaxy upgrade at zero pose");
    auto wrong_level = independent; wrong_level.upgrade_players.front().nonfirst_galaxy = {"Upgrade"};
    auto wrong_entry = take(campaign::build_space_entry(record, wrong_level, 100, 17));
    const auto& wrong_upgrades = wrong_entry.scene_upgrades;
    expect(std::none_of(wrong_upgrades.begin(), wrong_upgrades.end(), [](const auto& u) { return u.owner == 1; }),
        "first-level admission rejects an explicitly qualified later-level galaxy declaration");
    auto force_only_tables = tables;
    auto* empty_station = const_cast<units::UnitType*>(force_only_tables.find("Station")); empty_station->production.buildable_campaign.clear();
    auto* late_ship = const_cast<units::UnitType*>(force_only_tables.find("Ship")); late_ship->production.buildable_campaign = {{"Rebel", {"Upgrade"}}};
    auto force_only = input; force_only.map.tables = &force_only_tables; force_only.upgrade_players.front().galaxy.clear();
    force_only.upgrade_players.back().planet.clear();
    const auto before_forces = take(campaign::build_space_entry(record, force_only, 100, 17));
    expect(before_forces.scene_upgrades.empty(), "WGT-113: retained home lookup happens before initial ships, even when a later ship has the eligible list");
    // Shared-parent death commits only one permanent identity and survives another entry.
    auto galaxy = take(gc::GalacticSession::create(galaxy_for_entry()));
    campaign::ConflictRecord orbital; orbital.id = 90; orbital.planet = 2; orbital.invader = 1; orbital.defender = 2;
    orbital.forces = {{100, 7, 2, 41, campaign::ForceRole::orbital}, {101, 7, 2, 41, campaign::ForceRole::orbital}};
    campaign::BattleResult outcome; outcome.conflict = 90; outcome.winner = 1;
    outcome.forces = {{100, campaign::ForceDisposition::destroyed}, {101, campaign::ForceDisposition::destroyed}};
    const auto returning = take(campaign::plan_tactical_return(orbital, outcome, *galaxy.snapshot()));
    expect(returning.destroyed == std::vector<sim::EntityId>{41}, "duplicate tactical orbital deaths reconcile one parent source");
    campaign::TacticalReturnState state; check(campaign::apply_tactical_return(galaxy, orbital, returning, state, sim::InlineExecutor{}));
    expect(galaxy.snapshot()->production()->planets.back().objects.size() == 1
        && galaxy.snapshot()->production()->planets.back().objects.front().id == 40,
        "return removes the permanent orbital source, leaving unrelated structures for second entry");
}
void orbital_owner_admission() {
    auto galaxy=galaxy_for_entry(); galaxy.players.push_back({3,3,0});
    galaxy.seats.push_back({{3},3,gc::SeatKind::ai,1,true,0});
    galaxy.movement->players.push_back({3,whole(1),whole(1),false});
    for(const auto& planet:galaxy.planets) galaxy.movement->access.push_back({3,planet.id,true,false});
    galaxy.fiscal->players.push_back({3,false,whole(1)});
    galaxy.player_economy->players.push_back({3,{},whole(10000),0,5});
    galaxy.production->players.push_back({3,false,true,false,whole(100),{},{},{},{}});
    galaxy.production->planets.back().objects.back().owner=3;
    const auto tables=tables_for_entry(); auto input=entry_inputs(tables);
    input.sides.push_back({3,"Neutral",{},false}); input.types.back().every_marker_type="Pad";
    for (const auto a : {1U,2U,3U}) for (const auto b : {1U,2U,3U})
        input.relations.push_back({a,b,a==b,a!=b && a!=3 && b!=3});
    input.map.placements.push_back(marker(8,"Owner-specific",2000)); input.map.placements.back().marker_for.push_back({"Pad",{}});
    for(auto& structure:input.structures) if(structure.id==41) structure.owner=3;
    campaign::ConflictRecord record; record.id=90; record.planet=2; record.invader=1; record.defender=2;
    record.map_identity=input.map.map; record.station_level=1; record.seats={{{1},1},{{2},2}};
    record.forces={{20,1,1,10},{21,2,1,10},{22,1,1,10},{23,3,1,10},{30,1,2,12}};
    auto entry=take(campaign::build_space_entry(record,input,100,17));
    expect(entry.conflict.tactical.skirmish->slots.size()==2 && entry.conflict.tactical.players.size()>=3,
        "nonparticipant owner bindings are retained without becoming recorded commandable seats");
    expect(std::any_of(entry.conflict.forces.begin(),entry.conflict.forces.end(),[](const auto& force){return force.role==campaign::ForceRole::orbital && force.owner==3 && force.parent==41;}),
        "every-marker orbital copy retains its nonparticipant source owner");
    campaign::GameSession::ConflictService unused=[](const auto&,const auto&,sim::EntityId){return core::Result<std::optional<campaign::RecordedAdmission>>::success(std::nullopt);};
    campaign::GameSession::ContentResolver resolver=[&](const auto& conflict){return campaign::space_entry_content(conflict.tactical,input.map);};
    auto game=take(campaign::GameSession::create(galaxy)); game.set_conflict_service(unused,resolver);
    check(game.queue_conflict(entry.conflict,{campaign::ConflictRoute::pending_tactical,false,true}));
    auto staged=take(campaign::GameSession::create(galaxy)); check(staged.stage_battle(entry.conflict,entry.content));
    auto unknown=entry.conflict; for(auto& force:unknown.forces) if(force.role==campaign::ForceRole::orbital) force.owner=4;
    auto refused=take(campaign::GameSession::create(galaxy)); refused.set_conflict_service(unused,resolver);
    expect(!refused.queue_conflict(unknown,{campaign::ConflictRoute::pending_tactical,false,true}),"unknown nonparticipant orbital owner is refused");
}
void formation_contracts() {
    auto tables = tables_for_entry();
    const_cast<units::UnitType*>(tables.find("Ship"))->campaign_ship_class = "capital";
    for (const auto count : {1U,2U,3U,4U,5U,6U,7U,10U,17U,26U}) {
        auto input = entry_inputs(tables); input.sides[0].cap = whole(count);
        input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear();
        input.map.placements[3].position->x = whole(-3000);
        campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map;
        for (unsigned i=0;i<count;++i) record.forces.push_back({20+i,1,1,10});
        record.forces.push_back({60,1,2,12});
        const auto plan = take(campaign::prepare_space_entry(record,input,100,17));
        auto scene = take(campaign::build_space_entry(record,input,100,17)).start;
        auto other = scene;
        auto absent = take(campaign::form_space_entry(scene,input,plan,std::nullopt));
        auto target = take(campaign::form_space_entry(other,input,plan,sim::math::Vec3{whole(9999),whole(-8888),{}}));
        expect(!absent.attacker_cinematic_admitted && target.attacker_cinematic_admitted, "target presence admits intro only");
        for (std::size_t i=0;i<scene.units.size();++i) expect(scene.units[i].state == other.units[i].state, "target coordinates leave every formed pose unchanged");
        const auto position = [&](sim::EntityId id) { return std::find_if(scene.units.begin(),scene.units.end(),[id](const auto& u){return u.state.entity_id==id;})->state.position; };
        // WGT-07: the split takes the last-listed force first, so registration (WGT-19/45) runs 22, 21, 20.
        if (count==3) expect(position(22)==sim::math::Vec3{whole(-999),whole(1),{}} && position(20)==sim::math::Vec3{whole(-1001),{},{}}, "three-object remainder table and yaw-plus-270 central grid");
        if (count==4) expect(position(21)==sim::math::Vec3{whole(-1001),{},{}}, "equal adjacent rows apply sourced lateral-shift toggle");
    }
    auto* capital=const_cast<units::UnitType*>(tables.find("Ship"));
    capital->collision=units::CollisionBounds{{whole(-2),whole(-4),whole(-1)},{whole(2),whole(4),whole(1)}};
    {
        auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear(); input.sides[0].cap=whole(3);
        campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map; record.forces={{20,1,1,10},{21,1,1,10},{22,1,1,10},{60,1,2,12}};
        const auto plan=take(campaign::prepare_space_entry(record,input,100,17)); auto scene=take(campaign::build_space_entry(record,input,100,17)).start;
        // Registration order is 22, 21, 20 (WGT-07): the first row holds 22 and 21.
        const auto separation=[&](){return sim::math::subtract(std::find_if(scene.units.begin(),scene.units.end(),[](const auto& u){return u.state.entity_id==22;})->state.position.y,std::find_if(scene.units.begin(),scene.units.end(),[](const auto& u){return u.state.entity_id==21;})->state.position.y).value();};
        expect(separation()==whole(8),"unrotated collision-model width controls central spacing");
        for(auto& unit:scene.units) if(unit.state.entity_id>=20 && unit.state.entity_id<=22) unit.state.rotation=take(tt::yaw_rotation(whole(90)));
        (void)take(campaign::form_space_entry(scene,input,plan,std::nullopt));
        // A 90 degree pose leaves a few raw units of cosine residue in the fixed-point frame.
        expect(std::abs(separation().raw()-whole(4).raw())<32,"formation reads current-pose collision bounds, without using movement footprint overrides");
    }
    capital->collision.reset();
    auto* squad = const_cast<units::UnitType*>(tables.find("Squadron"));
    squad->members[0].offset = sim::math::Vec3{whole(10),{}, {}}; squad->members[1].offset = sim::math::Vec3{whole(-10),{}, {}};
    auto* craft = const_cast<units::UnitType*>(tables.find("Craft")); craft->campaign_ship_class="fighter"; craft->movement.space_layer="Fighter"; craft->movement.layer_z_adjust=whole(7);
    auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear();
    input.map.placements[3].position->x=whole(-3000);
    campaign::SpaceEntryType team; team.galactic=8; team.tactical="Squadron"; team.population=whole(1); team.squadron=true; team.source_flagship_rating=Fixed{}; input.types.push_back(team);
    campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map;
    record.forces={{20,1,1,10},{21,8,1,10},{60,1,2,12}};
    auto entry=take(campaign::build_space_entry(record,input,100,17));
    const auto company = std::find_if(entry.start.units.begin(),entry.start.units.end(),[](const auto& u){return u.state.entity_id==21;});
    const auto team_binding=std::find_if(entry.start.setup.squadrons.begin(),entry.start.setup.squadrons.end(),[](const auto& b){return b.container==21;});
    const auto child=std::find_if(entry.start.units.begin(),entry.start.units.end(),[&](const auto& u){return u.state.entity_id==team_binding->members.front();});
    expect(child->state.position.x==sim::math::add(company->state.position.x,whole(10)).value() && child->state.position.z==whole(7), "team leader class, authored member offsets and original craft height survive lead placement");
}
void team_origin_contract() {
    auto tables=tables_for_entry(); auto* squad=const_cast<units::UnitType*>(tables.find("Squadron"));
    for(auto& member:squad->members) member.offset=sim::math::Vec3{whole(5),{},{}};
    const_cast<units::UnitType*>(tables.find("Craft"))->campaign_ship_class="fighter";
    auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear(); input.sides[0].cap=whole(3);
    input.map.placements[3].position->x=whole(-3000); // keep the attacker on the first entry marker, as the formation contract does
    campaign::SpaceEntryType team; team.galactic=8; team.tactical="Squadron"; team.population=whole(1); team.squadron=true; team.source_flagship_rating=Fixed{}; input.types.push_back(team);
    campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map; record.forces={{20,8,1,10},{21,8,1,10},{22,8,1,10},{60,1,2,12}};
    const auto entry=take(campaign::build_space_entry(record,input,100,17));
    const auto position=[&](sim::EntityId id){return std::find_if(entry.start.units.begin(),entry.start.units.end(),[id](const auto& unit){return unit.state.entity_id==id;})->state.position;};
    expect(position(22).x==whole(-997) && position(20).x==whole(-1003),"origin-inclusive team union gives half-extent 3 for two members at offset 5 with half-extent 1");
    tt::SensorProfile sensor; sensor.type_id=1; sensor.reveal_range=Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    const std::array<tt::Player,1> players={tt::Player{1,1,1,1}};
    const std::array<tt::SensorProfile,1> sensors={sensor}; const std::array<std::pair<tt::PlayerId,Fixed>,1> multipliers={std::pair{1U,whole(2)}};
    expect(!tt::SensorField::build(players,{},sensors,{},multipliers),"overflowing reveal modifiers refuse sensor-field construction rather than dropping revelation");
}
void formation_stage_contracts() {
    auto tables=tables_for_entry();
    for(const auto& [name,ship_class]:{std::pair{"Fighter","fighter"},{"Super","super"}}) {auto type=*tables.find("Ship"); type.id=name; type.campaign_ship_class=ship_class; tables.units.push_back(type);}
    // WGT-21: a transport left empty is removed, so each fixture transport carries one stay-aboard passenger.
    const auto make=[&](std::vector<campaign::ConflictForce> forces,std::vector<sim::EntityId> carriers={}) {
        auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear(); input.sides[0].cap=whole(30);
        std::erase_if(input.map.placements,[](const auto& marker){return marker.record==4;});
        campaign::SpaceEntryType fighter; fighter.galactic=8; fighter.tactical="Fighter"; fighter.population=whole(1); input.types.push_back(fighter);
        auto super=fighter; super.galactic=9; super.tactical="Super"; input.types.push_back(super);
        campaign::SpaceEntryType cargo; cargo.galactic=10; cargo.tactical="Commander"; cargo.population=whole(0); input.types.push_back(cargo);
        for(std::size_t i=0;i<carriers.size();++i) input.riders.push_back({{static_cast<sim::EntityId>(24+i),10,1,10},"Commander",carriers[i],{},{}});
        campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map; record.forces=std::move(forces); record.forces.push_back({70,1,2,12});
        return take(campaign::build_space_entry(record,input,100,17));
    };
    const auto position=[](const auto& scene,sim::EntityId id){return std::find_if(scene.start.units.begin(),scene.start.units.end(),[id](const auto& unit){return unit.state.entity_id==id;})->state.position;};
    auto transports=make({{20,3,1,10},{21,3,1,10},{22,3,1,10}},{20,21,22});
    expect(position(transports,20)==sim::math::Vec3{whole(-999),whole(1),{}} && position(transports,22)==sim::math::Vec3{whole(-1001),{},{}},"transport-only fleet uses the central grid");
    auto rear=make({{20,1,1,10},{21,3,1,10},{22,3,1,10},{23,3,1,10}},{21,22,23});
    expect(position(rear,21)==sim::math::Vec3{whole(-1201),whole(11),{}} && position(rear,22)==sim::math::Vec3{whole(-1201),whole(-11),{}} && position(rear,23)==sim::math::Vec3{whole(-1223),{},{}},"rear grid applies sourced column/row gap and footprint setback");
    auto supers=make({{20,9,1,10},{21,9,1,10}});
    expect(position(supers,20)==sim::math::Vec3{whole(-1000),{},{}} && position(supers,21)==position(supers,20),"super class has its separate zero-spacing grid");
    const auto close=[](Fixed value,std::string_view expected){return std::abs(value.raw()-Fixed::from_decimal(expected).value().raw())<32;};
    const_cast<units::UnitType*>(tables.find("Ship"))->campaign_ship_class="capital";
    auto screened=make({{20,1,1,10},{21,2,1,10},{22,8,1,10}});
    expect(close(sim::math::add(position(screened,21).x,whole(1000)).value(),"2.82842712474619") && close(sim::math::add(position(screened,22).x,whole(1000)).value(),"5.65685424949238"),"capital core screens medium class and leads the later fighter class");
    const_cast<units::UnitType*>(tables.find("Ship"))->campaign_ship_class="corvette";
    auto led=make({{20,1,1,10},{21,8,1,10}});
    expect(close(sim::math::add(position(led,21).x,whole(1000)).value(),"2.82842712474619"),"corvette core preserves the intentional empty screen bucket and leads fighters");
}
void mixed_class_contracts() {
    // WGT-45: same movement layer, different authored classes, and asymmetric mixes.
    for (const std::string kinds:{"FFBB","FFBBBBB","FFFFFBB"}) {
        auto tables=tables_for_entry();
        for(const auto name:{"Fighter","Bomber"}) { auto type=*tables.find("Ship"); type.id=name; type.campaign_ship_class="capital"; tables.units.push_back(type); }
        auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.riders.clear(); input.types[0].escorts.clear(); input.sides[0].cap=whole(kinds.size());
        campaign::SpaceEntryType fighter; fighter.galactic=8; fighter.tactical="Fighter"; fighter.population=whole(1); input.types.push_back(fighter);
        auto bomber=fighter; bomber.galactic=9; bomber.tactical="Bomber"; input.types.push_back(bomber);
        campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map;
        for(std::size_t i=0;i<kinds.size();++i) record.forces.push_back({20+i,kinds[i]=='F'?8U:9U,1,10});
        record.forces.push_back({60,1,2,12});
        const auto plan=take(campaign::prepare_space_entry(record,input,100,17));
        const auto central=take(campaign::build_space_entry(record,input,100,17));
        const_cast<units::UnitType*>(tables.find("Fighter"))->campaign_ship_class="fighter";
        const_cast<units::UnitType*>(tables.find("Bomber"))->campaign_ship_class="bomber";
        const auto mixed=take(campaign::build_space_entry(record,input,100,17));
        const auto position=[](const auto& scene,sim::EntityId id){return std::find_if(scene.start.units.begin(),scene.start.units.end(),[id](const auto& unit){return unit.state.entity_id==id;})->state.position;};
        // Registration order is the plan's creation order (WGT-07/19); the capital-class scene places it row by row.
        auto ranked=plan.placements; std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.creation_order<b.creation_order;});
        std::vector<sim::EntityId> registered,fighters,bombers,expected;
        for(const auto& placement:ranked) if(placement.force>=20 && placement.force<20+kinds.size()) {
            registered.push_back(placement.force); (kinds[placement.force-20]=='F'?fighters:bombers).push_back(placement.force);
        }
        // WGT-45 balanced interleave over the registered kinds.
        std::size_t f{},b{};
        const auto pairs=[&](std::size_t count){for(std::size_t i=0;i<count;++i){expected.push_back(bombers[b++]);expected.push_back(fighters[f++]);}};
        if(bombers.size()>fighters.size()){
            for(const auto leading=(bombers.size()-fighters.size())/2;b<leading;) expected.push_back(bombers[b++]);
            pairs(fighters.size());
            while(b<bombers.size()) expected.push_back(bombers[b++]);
        } else {
            pairs(bombers.size()/2);
            for(std::size_t i=0;i<fighters.size()-bombers.size();++i) expected.push_back(fighters[f++]);
            pairs(bombers.size()-b);
        }
        expect(expected.size()==kinds.size() && registered.size()==kinds.size(),"every mixed-class force is registered once");
        for(std::size_t rank=0;rank<expected.size() && rank<registered.size();++rank) expect(position(mixed,expected[rank])==position(central,registered[rank]),"mixed class balanced interleave preserves within-kind registration order");
    }
}
void garrison_contracts() {
    auto tables=tables_for_entry(); tables.campaign_scene=true;
    auto second=*tables.find("Squadron"); second.id="OtherSquadron"; tables.units.push_back(second);
    auto* station=const_cast<units::UnitType*>(tables.find("Station"));
    station->spawner->campaign_starting[0]={{"Squadron",units::no_index,1}};
    station->spawner->campaign_starting[2]={{"Squadron",units::no_index,5}};
    station->spawner->campaign_reserves[0]={{"Squadron",units::no_index,-1}};
    station->spawner->campaign_reserves[3]={{"OtherSquadron",units::no_index,9}};
    auto input=entry_inputs(tables); input.riders.clear(); input.types[0].escorts.clear();
    campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map; record.forces={{20,1,1,10},{30,1,2,12}};
    auto entry=take(campaign::build_space_entry(record,input,100,17)); auto setup=entry.conflict.tactical;
    setup.campaign.emplace(); setup.campaign->players={{1,{},30,false,{},0,5},{2,{},30,true,{},3,5}};
    for(auto& player:setup.campaign->players) player.credit_cap=whole(1000000);
    auto content=take(campaign::space_entry_content(setup,input.map));
    const auto* low=content.motion.squadrons.find_spawner(skirmish::type_id("Station"),1);
    const auto* high=content.motion.squadrons.find_spawner(skirmish::type_id("Station"),2);
    expect(low && high && low->entries.size()==1 && low->entries.front().starting==1 && low->entries.front().reserve==-1, "owner Tech0 keeps the authored unlimited reserve sentinel");
    expect(high && high->entries.size()==1 && high->entries.front().starting==5 && high->entries.front().reserve==0, "Tech3 chooses independent complete starting/reserve tiers; reserve-only type makes no spawn row and no lower per-type fallback");
    setup.campaign->players.front().start_tech=6; setup.campaign->players.front().max_tech=6;
    expect(!campaign::space_entry_content(setup,input.map),"invalid owner tech is refused");
    auto legacy=tables; legacy.campaign_scene=false; legacy.campaign_ability_locks={{1,"Commander","Defense",false}};
    auto unchanged=legacy; unchanged.campaign_ability_locks.clear(); unchanged.units.back().campaign_ship_class="super"; unchanged.units.back().production.battlefield_bonuses={{"x",0,whole(1),{},true,true}};
    expect(units::canonical_encoding(legacy)==units::canonical_encoding(unchanged),"ordinary skirmish content encoding excludes every opt-in campaign extension");
}
void commander_contracts() {
    std::vector<std::string> reference;
    for (const auto workers : {1U,2U,4U,8U}) for (const auto scenario : {0U,1U,2U,3U,4U,5U,6U,7U,8U,9U}) {
        auto tables=tables_for_entry(); tables.campaign_scene=true;
        units::NamedConstant cap; cap.tag="Credit_Cap_Per_Planet"; cap.value=whole(10000); tables.constants.scalars.push_back(cap);
        auto* commander=const_cast<units::UnitType*>(tables.find("Commander"));
        commander->production.battlefield_bonuses={{"Defense",0,Fixed::from_ratio(1,4).value(),{},false,true},{"Reveal",0,{},Fixed::from_ratio(1,2).value(),true,true}, {"Disabled",0,whole(1),whole(1),true,false}};
        if(scenario==1) tables.campaign_ability_locks={{1,"Commander","Defense",false}};
        if(scenario==4) { commander->production.battlefield_bonuses[0].category=7; commander->production.battlefield_bonuses[1].category=7; }
        if(scenario==5) { commander->production.battlefield_bonuses[0].enabled=false; tables.campaign_ability_locks={{1,"Commander","Defense",true}}; }
        auto input=entry_inputs(tables); input.structures.clear(); input.station_type.clear(); input.types[0].escorts.clear();
        // WGT-21/49/50: an authored officer's name is not a dispatch key.
        commander->id = "Mod_Officer";
        input.types[3].tactical = "Mod_Officer";
        for (auto& rider : input.riders) if (rider.force.type == 4) rider.tactical = "Mod_Officer";
        for (auto& lock : tables.campaign_ability_locks) lock.type = "Mod_Officer";
        if (scenario == 8) commander->production.battlefield_bonuses.push_back(
            {"OtherCategory",8,Fixed::from_ratio(1,8).value(),Fixed::from_ratio(1,4).value(),false,true});
        input.sides.push_back({3,"Rebel",{},false}); input.upgrade_players={{1,"Rebel",true,{1,3},{},{},{}},{2,"Empire",true,{2},{},{},{}},{3,"Rebel",true,{1,3},{},{},{}}};
        // This fixture adds an allied ship after entry. Declare its scene owner
        // through a harmless modifier instead of relying on an unused side.
        units::UnitType ally_binding; ally_binding.id="CommanderAllyBinding";
        ally_binding.affiliation="Rebel"; ally_binding.production.upgrade_object=true;
        tables.units.push_back(ally_binding); input.upgrade_players.back().planet={ally_binding.id};
        for (const auto a : {1U,2U,3U}) for (const auto b : {1U,2U,3U}) {
            const bool allied=a==b || (a!=2 && b!=2); input.relations.push_back({a,b,allied,!allied});
        }
        for(auto& faction:input.map.factions) faction.space_unit_cap=30;
        campaign::ConflictRecord record; record.id=90; record.invader=1; record.defender=2; record.map_identity=input.map.map;
        record.forces={{20,1,1,10},{22,1,1,10},{23,3,1,10},{30,2,2,12}};
        if(scenario==3 || scenario==4 || scenario==9) input.riders.push_back({{26,4,1,10},"Mod_Officer",23,{}, {}});
        auto entry=take(campaign::build_space_entry(record,input,100,17));
        if (scenario == 9) {
            auto& host = *std::find_if(entry.start.setup.units.begin(), entry.start.setup.units.end(), [](const auto& unit) { return unit.entity_id == 20; });
            auto& other = *std::find_if(entry.start.setup.units.begin(), entry.start.setup.units.end(), [](const auto& unit) { return unit.entity_id == 22; });
            other.contained.push_back(host.contained.back()); host.contained.pop_back();
            other.contained.back().parent = 22;
        }
        for(auto& unit:entry.start.setup.units) {
            unit.position={unit.entity_id==20?whole(5000):unit.entity_id==22?whole(scenario==6?2000:300):Fixed{}, {},{}}; unit.rotation=sim::math::identity_quat();
        }
        entry.start.setup.units.push_back({25,skirmish::type_id("Ship"),3,{scenario==6?whole(300):whole(800),{},{}},sim::math::identity_quat(),{}});
        std::sort(entry.start.setup.units.begin(),entry.start.setup.units.end(),[](const auto&a,const auto&b){return a.entity_id<b.entity_id;});
        entry.conflict.tactical=entry.start.setup;
        campaign::SpaceRuleInputs rules; rules.players={{1,whole(100),0,5,1,{},false,{}},{2,whole(100),0,5,1,{},true,{}}};
        check(campaign::apply_space_rules(entry,input.map,rules));
        auto& content=entry.content; content.victory={}; content.motion.squadrons={};
        tt::SensorProfile own_sensor; own_sensor.type_id=skirmish::type_id("Ship"); own_sensor.reveal_range=whole(100);
        tt::SensorProfile enemy_sensor; enemy_sensor.type_id=skirmish::type_id("ReserveShip"); enemy_sensor.reveal_range=whole(700);
        content.sensors={own_sensor,enemy_sensor};
        std::sort(content.sensors.begin(),content.sensors.end(),[](const auto&a,const auto&b){return a.type_id<b.type_id;});
        tt::CombatProfile shooter; shooter.type_id=skirmish::type_id("ReserveShip"); shooter.max_attack_distance=whole(700);
        tt::WeaponProfile weapon; weapon.range=whole(700); weapon.pulse_count=1; weapon.cone_width=whole(175); weapon.cone_height=whole(160);
        weapon.opportunity_when_idle=true; weapon.opportunity_when_targeting=true; weapon.shot=tt::ShotProfile{whole(10),0,whole(25),whole(700),true,true,{}}; shooter.weapons={weapon};
        tt::CombatProfile target; target.type_id=skirmish::type_id("Ship"); target.collision=tt::CollisionBox{{whole(-10),whole(-10),whole(-10)},{whole(10),whole(10),whole(10)}};
        content.combat.profiles={shooter,target}; std::sort(content.combat.profiles.begin(),content.combat.profiles.end(),[](const auto&a,const auto&b){return a.type_id<b.type_id;});
        if (scenario==2 || scenario==7) {
            tt::FogRules fog; fog.map_left=whole(-1000); fog.map_top=whole(1000); fog.cell_size=whole(100);
            fog.cells_wide=64; fog.cells_tall=32; fog.service_period=1; fog.ramp_down_step=238; content.fog=fog;
        }
        auto session=take(tt::TacticalSession::create(entry.conflict.tactical,content)); platform::ThreadWorkerAdapter executor(workers);
        const auto reveal=[&](tt::TacticalSession& live,sim::EntityId id){auto view=live.snapshot();return std::find_if(view->instances().begin(),view->instances().end(),[id](const auto&u){return u.entity_id==id;})->reveal_range;};
        const auto initial_reveal=whole(scenario==8?175:150);
        expect(reveal(session,22)==initial_reveal && reveal(session,25)==whole(150),"limbo rider reveal applies to owner and playable ally, disabled handler contributes nothing");
        if (scenario==2 || scenario==7) expect(session.fog_cells()->values(0)[10*64+15]==255,"stationary ship initially holds the bonus-only fog cell");
        const sim::EntityId victim=scenario==6?25:22;
        check(session.submit({{0,2,0},{30},tt::AttackPayload{victim}})); check(session.submit({{1,2,1},{30},tt::StopPayload{}}));
        if(scenario==2 || scenario==9) check(session.submit({{2,1,0},{20},tt::DamagePayload{whole(1000)}}));
        (void)take(session.step(executor));
        if (scenario==7) check(session.stage_remove(20));
        const auto identity=checkpoint::tactical_identities(entry.conflict.tactical.content_identity);
        auto bytes=take(checkpoint::save_tactical_checkpoint(session,session.pending_commands(),checkpoint::default_seats(entry.conflict.tactical),identity));
        checkpoint::TacticalSessionFactory factory=[&](const tt::TacticalReplay& replay){return tt::TacticalSession::from_replay(replay,content);};
        auto restored=take(checkpoint::load_tactical_checkpoint(bytes,identity,factory,executor));
        std::vector<std::string> hashes;
        for(unsigned frame=1;frame<30;++frame){auto step=take(session.step(executor)); hashes.push_back(step.state_sha256); expect(take(restored.session.step(executor)).state_sha256==step.state_sha256,"fresh checkpoint preserves player modifiers and queued carrier death");}
        const auto amount=scenario==8?Fixed::from_ratio(25,4).value()
            :(scenario==0 || scenario==3 || scenario==4 || scenario==5 || scenario==9)?Fixed::from_ratio(15,2).value():whole(10);
        expect(session.durability_state(victim)->hull==sim::math::subtract(whole(100),amount).value(),"player defence qualifies unrelated recipient type; explicit lock and carrier death remove the source before impact");
        if(scenario==2 || scenario==7) {
            expect(reveal(session,22)==whole(100) && reveal(session,25)==whole(100),"carrier death removes both rider player-ledger effects, including allied reveal");
            expect(session.fog_cells()->values(0)[10*64+15]==0,"range-change event releases stationary bonus-only fog cells, including after staged removal and fresh checkpoint");
        }
        if (scenario == 9) expect(reveal(session,22)==whole(150) && reveal(session,25)==whole(150),
            "removing one same-category source preserves the surviving commander's defence and allied reveal");
        if(workers==1) reference.insert(reference.end(),hashes.begin(),hashes.end());
        else expect(std::equal(hashes.begin(),hashes.end(),reference.begin()+static_cast<std::ptrdiff_t>(scenario*hashes.size())),"commander continuation is identical on 1/2/4/8 workers");
    }
}
void ai_retreat_completion() {
    namespace auth = script::authoritative;
    std::vector<std::string> reference;
    std::vector<std::uint8_t> reference_replay;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        tt::TacticalSetup initial; initial.seed = 17;
        initial.players = {{1, 1, 1, tt::player_flag_commandable}, {2, 2, 2, tt::player_flag_commandable}};
        initial.cinematic_seats = {{1, true}, {2, false}};
        initial.units = {{20, 1, 1}, {30, 1, 2, {whole(1000), {}, {}}}};
        tt::TacticalContent content;
        content.motion.rules = {whole(45), whole(100)};
        content.motion.profiles.push_back({1, whole(1), whole(1), whole(1), whole(1), whole(1)});
        tt::DurabilityProfile hull; hull.type_id = 1; hull.max_hull = whole(100); content.durability.profiles.push_back(hull);
        content.victory.condition = tt::VictoryCondition::all_enemy_units_destroyed;
        content.victory.relevant_types = {1}; content.victory.contenders = content.victory.controlled_players = {1, 2};
        content.retreat.emplace(); content.retreat->rules.campaign = true;
        content.retreat->rules.players = {{1, 2, true}, {2, 2, true}};
        content.retreat->actors = {{20, 1, true, true, true, true}, {30, 2, true, true, true, true}};
        script::foc::AiSetup ai; ai.seed = initial.seed; ai.perception.campaign_game = true;
        ai.players = {{1, "REBEL", false, false, {}, true}, {2, "EMPIRE", false, true, {}}};
        ai.freestore_module = "AI.LUA";
        ai.campaign_can_retreat = [](const auto& snapshot, const auto player) {
            return std::any_of(snapshot.retreat_views().begin(), snapshot.retreat_views().end(), [&](const auto& view) {
                return view.player == player && view.state.phase == tt::RetreatPhase::idle && view.refusal == tt::RetreatRefusal::none;
            });
        };
        ai.campaign_retreat_order = [](const tt::PlayerId player) {
            return core::Result<auth::TacticalOrder>::success({player, {}, tt::RetreatPayload{tt::RetreatAction::start}});
        };
        auto prepared = take(script::foc::prepare_session(ai, {{"AI.LUA",
            "function Base_Definitions() ServiceRate = 1000; UnitServiceRate = 1000 end\n"
            "function main() coroutine.yield(true) end\nfunction On_Unit_Service(unit) end\n"},
            {"Data/Scripts/Library/PGAICommands.lua", "EnemyContrastTypes = {}; FriendlyContrastTypes = {}; function Base_Definitions() end\n"}}));
        auto child = take(auth::ScriptedTacticalSession::create(take(tt::TacticalSession::create(initial, content)), std::move(prepared.scripts)));
        check(prepared.bind(child));
        const platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const std::array start{tt::retreat_command({0, 2, 1}, tt::RetreatAction::start)};
        for (std::uint32_t tick = 0; tick < 240; ++tick) {
            const auto stepped = take(child.step(executor, tick == 0 ? std::span<const tt::PlayerCommand>(start) : std::span<const tt::PlayerCommand>{}));
            expect(stepped.scripts.diagnostics.empty(), "AI withdrawal service has no script diagnostics");
            hashes.push_back(stepped.state_sha256);
            if (child.world().outcome() && child.completed_tick() > child.world().outcome()->end_tick) break;
        }
        expect(child.world().retreat_state().phase == tt::RetreatPhase::escaped, "AI retreat reaches escaped without a viewer");
        expect(child.world().outcome() && child.world().outcome()->winner == 1
            && child.completed_tick() > child.world().outcome()->end_tick, "AI escape ends the battle for the enemy winner");
        const auto replay = child.record();
        expect(std::none_of(replay.commands.begin(), replay.commands.end(), [](const auto& command) {
            return std::holds_alternative<tt::RetreatCinematicPayload>(command.payload);
        }), "disabled AI cinematics complete escape with zero playback commands");
        const auto bytes = take(tt::write_replay(replay));
        auto replayed = take(tt::TacticalSession::from_replay(take(tt::parse_replay(bytes)), content));
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) (void)take(replayed.step(executor));
        expect(replayed.state_sha256() == child.world().state_sha256(), "recorded AI retreat and completion replay exactly");
        if (workers == 1) { reference = hashes; reference_replay = bytes; }
        else expect(reference == hashes && reference_replay == bytes, "AI retreat hashes and replay bytes equal on 1/2/4/8 workers");
    }
}
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
            bool fleet_attacked = false, spawned_attacked = false, defence_started = false;
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
            expect(fleet_attacked, "fleet behind its station receives an attack within 1800 frames");
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
void installed(bool garrison_only = false, bool population_only = false, bool commander_only = false) {
    std::optional<std::string> root;
#if defined(_MSC_VER)
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") == 0 && value) { root = value; std::free(value); }
#else
    if (const auto* value = std::getenv("EAWR_EAW_GAME_ROOT")) root = value;
#endif
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
    fixture.locations = {"Shola", "Kamino"}; fixture.planets = fixture.locations; fixture.starting_active_player = "Rebel";
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
} // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("EAWR_VERIFY_GALACTIC_HASH", "1");
#else
    setenv("EAWR_VERIFY_GALACTIC_HASH", "1", 1);
#endif
    try { if (argc > 1 && std::string_view(argv[1]) == "--installed-commanders") installed(false, false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-space-population") installed(false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-garrison") installed(true); else if (argc > 1 && std::string_view(argv[1]) == "--installed") installed(); else { salvage_contracts(); script_manifest_contracts(); preparation_contracts(); nonparticipant_team_contracts(); scene_contracts(); reserve_order_contract(); orbital_owner_admission(); formation_contracts(); team_origin_contract(); formation_stage_contracts(); mixed_class_contracts(); garrison_contracts(); commander_contracts(); ground_passenger_contracts(); admitted_cycle(); ai_retreat_completion(); } }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

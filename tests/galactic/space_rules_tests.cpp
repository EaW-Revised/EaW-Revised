#include "galactic_support.hpp"
#include "eawr/campaign/space_rules.hpp"
#include "eawr/campaign/space_briefing.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/ai.hpp"

#include <array>
#include <cstdlib>
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
void proof() {
    auto tables = synthetic_tables();
    for (const auto& [tag, value] : {std::pair{"Tactical_Build_Time_Multiplier", 1},
        {"Space_Elevated_Vulnerability_Factor", -3}, {"Space_Elevated_Vulnerability_Duration", 5},
        {"Space_Reinforcement_Collision_Check_Distance", 10}, {"Credit_Cap_Per_Planet", 1000}, {"MP_Default_Credits", 4321}})
        tables.constants.scalars.push_back({tag, whole(value), {}});
    tables.ai_credit_multiplier = whole(1);
    for (auto& type : tables.units) type.production.population_value = 1;
    auto* station = &tables.units[1];
    station->production.buildable_campaign = {{"Empire", {"Upgrade"}}};
    station->production.buildable = {{"Empire", {"ReserveShip"}}};
    units::UnitType upgrade; upgrade.id = "Upgrade"; upgrade.production.upgrade_object = true;
    upgrade.production.build_cost_campaign = whole(123);
    upgrade.production.build_cost_multiplayer = whole(999);
    upgrade.production.build_time_seconds = whole(1);
    upgrade.production.production_queue = "Tactical_Upgrades";
    upgrade.production.increments_tech = true;
    upgrade.child_persists = true;
    units::CombatBonus health; health.stacking_category = 1; health.types = {"Ship", "ReserveShip"};
    health.percentages[0] = Fixed::from_raw(Fixed::scale / 2);
    upgrade.production.combat_bonuses.push_back(health);
    tables.units.push_back(upgrade);
    auto inputs = synthetic_inputs(tables);
    inputs.factions[0].space_unit_cap = 2; inputs.factions[1].space_unit_cap = 7;
    auto fixture = fixture_for(inputs);
    fixture.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    campaign::ConflictRecord conflict; conflict.id = 90; conflict.planet = 2;
    conflict.invader = 1; conflict.defender = 2; conflict.ai_tactical_credits = true;
    conflict.map_identity = inputs.map; conflict.station = 45; conflict.station_type = skirmish::type_id("Station");
    conflict.forces = {{7, skirmish::type_id("Ship"), 1}, {21, skirmish::type_id("Ship"), 2},
        {45, conflict.station_type, 2}, {79, skirmish::type_id("ReserveShip"), 1, 0, campaign::ForceRole::reinforcement},
        {80, skirmish::type_id("ReserveShip"), 1, 0, campaign::ForceRole::reinforcement},
        {81, skirmish::type_id("ReserveShip"), 1, 0, campaign::ForceRole::reinforcement}};
    auto built = campaign::build_conflict_entry(conflict, fixture, inputs,
        std::vector<campaign::ForcePlacement>{{7, 1}, {21, 2}, {45, 3}}, 100, 17);
    expect(static_cast<bool>(built), "synthetic persistent space entry builds");
    if (!built) { std::cerr << built.error().message << '\n'; return; }
    campaign::SpaceRuleInputs rules;
    auto ordinary = built.value().start;
    for (auto& player : ordinary.players) if (player.player.player_id == 1 || player.player.player_id == 2) player.lobby = true;
    const auto multiplayer = skirmish::economy_rules(ordinary, inputs, tables, false);
    expect(multiplayer && multiplayer.value().players.size() == 2
        && multiplayer.value().players[0].credits == whole(4321) && multiplayer.value().players[1].credits == whole(4321)
        && !multiplayer.value().players[0].strategic_credits && !multiplayer.value().players[1].strategic_credits,
        "skirmish/multiplayer preserves MP_Default_Credits without campaign category switching");
    rules.players = {{1, whole(777), 3, 5, 1, whole(90), false, {}},
        {2, whole(2345), 2, 5, 3, whole(180), true, {{whole(600), Fixed::from_raw(Fixed::scale / 2)}}}};
    rules.restricted_reserves = {80};
    auto loaded_entry = built.value();
    loaded_entry.conflict.forces[3].purchase_type = skirmish::type_id("Ship");
    campaign::ConflictForce passenger{82, skirmish::type_id("Craft"), 1, 0, campaign::ForceRole::reinforcement};
    passenger.carrier = 79; loaded_entry.conflict.forces.push_back(passenger);
    passenger.id = 83; passenger.carrier = 82; loaded_entry.conflict.forces.push_back(passenger);
    auto loaded_rules = rules;
    tactical::CarriedObject cargo; cargo.entity_id = 82; cargo.type_id = skirmish::type_id("Craft");
    cargo.parent = 79; cargo.named_hero = true;
    tactical::CarriedObject child; child.entity_id = 83; child.type_id = skirmish::type_id("Craft");
    child.parent = 82; child.combat_preserved = true; cargo.members.push_back(child);
    loaded_rules.reserve_attachments = {{79, {cargo}}};
    expect(static_cast<bool>(campaign::apply_space_rules(loaded_entry, inputs, loaded_rules)), "loaded transport campaign rules bind qualified passengers");
    std::vector<std::string> loaded_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto loaded = tactical::TacticalSession::create(loaded_entry.conflict.tactical, loaded_entry.content).value();
        expect(loaded.snapshot()->economy()[0].pool_tokens == std::vector<std::uint64_t>{79, 80, 81}, "passengers have no separate pool offers");
        expect(static_cast<bool>(loaded.submit({{0, 1, 1}, {}, tactical::ReinforcePayload{skirmish::type_id("ReserveShip"), {whole(-500), {}, {}}, 79}})), "loaded transport deployment command");
        std::vector<std::string> hashes{loaded.step(executor).value().state_hash.get()};
        const auto units = loaded.units();
        const auto root = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 79; });
        expect(root != units.end() && root->purchase_type == skirmish::type_id("Ship") && root->contained == std::vector<tactical::CarriedObject>{cargo},
            "loaded transport preserves original purchase identity and recursive persistent passengers");
        expect(loaded.next_entity_id() == *loaded_entry.conflict.tactical.first_free_entity_id, "persistent passengers consume no replacement IDs");
        const auto identity = checkpoint::tactical_identities(loaded_entry.conflict.tactical.content_identity);
        const auto saved = checkpoint::save_tactical_checkpoint(loaded, {}, checkpoint::default_seats(loaded_entry.conflict.tactical), identity).value();
        const checkpoint::TacticalSessionFactory factory = [&](const tactical::TacticalReplay& replay) {
            auto content = loaded_entry.content;
            const auto rebound = campaign::bind_space_rules(replay.setup, inputs, content);
            if (!rebound) return core::Result<tactical::TacticalSession>::failure(rebound.error());
            return tactical::TacticalSession::from_replay(replay, content);
        };
        auto restored = checkpoint::load_tactical_checkpoint(saved, identity, factory, executor).value();
        for (int tick = 1; tick < 12; ++tick) {
            const auto hash = loaded.step(executor).value().state_hash.get(); hashes.push_back(hash);
            expect(restored.session.step(executor).value().state_hash.get() == hash, "loaded transport checkpoint continuation preserves identities");
        }
        auto replayed = factory(tactical::parse_replay(tactical::write_replay(loaded.record()).value()).value()).value();
        for (const auto& hash : hashes) expect(replayed.step(executor).value().state_hash.get() == hash, "loaded transport replay metadata preserves identities");
        if (loaded_reference.empty()) loaded_reference = hashes; else expect(hashes == loaded_reference, "loaded transport agrees on 1/2/4/8 workers");
    }
    auto missing_attachment = loaded_rules; missing_attachment.reserve_attachments.clear();
    const auto before_missing = loaded_entry.conflict.tactical;
    expect(!campaign::apply_space_rules(loaded_entry, inputs, missing_attachment) && loaded_entry.conflict.tactical == before_missing,
        "unqualified passenger refuses transactionally");
    auto switched_entry = built.value();
    switched_entry.conflict.tactical.match_policy = tactical::SkirmishMatchPolicy{false, false, true, false};
    auto& incoming_match = switched_entry.conflict.tactical.skirmish->match;
    incoming_match.allow_heroes = false;
    incoming_match.allow_superweapons = false;
    incoming_match.free_starting_units = true;
    incoming_match.pre_built_base = false;
    expect(static_cast<bool>(tactical::validate_setup(switched_entry.conflict.tactical)), "non-default incoming switches are valid");
    expect(static_cast<bool>(campaign::apply_space_rules(switched_entry, inputs, rules)), "campaign policy replaces non-default switches");
    const auto& switched_setup = switched_entry.conflict.tactical;
    const auto& recorded_match = switched_setup.skirmish->match;
    expect(switched_setup.match_policy == tactical::SkirmishMatchPolicy{true, true, false, true}
        && recorded_match.allow_heroes && recorded_match.allow_superweapons
        && !recorded_match.free_starting_units && recorded_match.pre_built_base,
        "all four recorded switches match the authoritative campaign policy");
    expect(static_cast<bool>(tactical::validate_setup(switched_setup)) && switched_entry.start.setup == switched_setup,
        "decorated entry and recorded start retain the same valid setup");
    expect(static_cast<bool>(tactical::TacticalSession::create(switched_setup, switched_entry.content)),
        "non-default incoming switches do not reject campaign child creation");
    expect(static_cast<bool>(campaign::apply_space_rules(built.value(), inputs, rules)), "campaign policy applies transactionally");
    const auto& entry = built.value();
    if (!entry.conflict.tactical.campaign) return;
    const auto& setup = entry.conflict.tactical;
    expect(entry.content.economy.players[0].credits == whole(777)
        && entry.content.economy.players[0].start_tech == 3
        && entry.content.economy.players[1].credits == whole(2345)
        && !entry.content.economy.players[1].strategic_credits, "WGA-201 campaign space retains ordinary credits/tech without a land switch");
    const auto* menu = entry.content.economy.menu(conflict.station_type, skirmish::faction_id("Empire"));
    expect(menu && menu->options.size() == 1 && menu->options[0].price == whole(123), "campaign list/cost excludes multiplayer ship menu");
    expect(entry.content.victory.condition == tactical::VictoryCondition::all_enemy_units_destroyed
        && entry.content.victory.countdown_frames == 210 && setup.free_garrisons.empty()
        && !setup.skirmish->match.free_starting_units && setup.skirmish->match.game_timer == 0,
        "campaign victory and no free template or lobby timer");
    std::vector<std::string> capped_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto capped = tactical::TacticalSession::create(setup, entry.content).value();
        const std::vector<tactical::PlayerCommand> commands{
            {{0, 2, 1}, {}, tactical::CreditGrantPayload{whole(2665)}},
            {{0, 2, 2}, {}, tactical::CampaignCreditReleasePayload{whole(100)}},
            {{1, 2, 3}, {}, tactical::CampaignCreditReleasePayload{whole(-1)}},
            {{2, 2, 4}, {45}, tactical::BuyPayload{skirmish::type_id("Upgrade")}},
            {{3, 2, 5}, {}, tactical::CampaignCreditReleasePayload{whole(-1)}},
            {{4, 2, 6}, {}, tactical::CampaignCreditReleasePayload{whole(100)}},
        };
        for (const auto& command : commands) expect(static_cast<bool>(capped.submit(command)), "capped transfer command");
        std::vector<std::string> hashes{capped.step(executor).value().state_hash.get()};
        const auto account = capped.snapshot()->economy()[1];
        expect(account.credits == whole(3000) && account.campaign_extra_releases == Fixed{},
            "campaign space release has no land strategic account");
        expect(account.credits == whole(3000),
            "ordinary shared account clamps the windfall at its cap");
        const auto identity = checkpoint::tactical_identities(setup.content_identity);
        const auto saved = checkpoint::save_tactical_checkpoint(capped, capped.pending_commands(),
            checkpoint::default_seats(setup), identity).value();
        const checkpoint::TacticalSessionFactory factory = [&](const tactical::TacticalReplay& replay) {
            auto content = entry.content;
            const auto rebound = campaign::bind_space_rules(replay.setup, inputs, content);
            if (!rebound) return core::Result<tactical::TacticalSession>::failure(rebound.error());
            return tactical::TacticalSession::from_replay(replay, content);
        };
        auto restored = checkpoint::load_tactical_checkpoint(saved, identity, factory, executor).value();
        for (int tick = 1; tick < 5; ++tick) {
            const auto hash = capped.step(executor).value().state_hash.get(); hashes.push_back(hash);
            expect(restored.session.step(executor).value().state_hash.get() == hash, "capped transfer checkpoint continuation");
            const auto current = capped.snapshot()->economy()[1];
            expect(current.credits == whole(tick < 2 ? 3000 : 2877)
                && current.campaign_extra_releases == Fixed{},
                "campaign space has no land release pool after spending");
            expect(current.credits == whole(tick < 2 ? 3000 : 2877),
                "ordinary purchase charges the single account once");
        }
        auto replayed = factory(tactical::parse_replay(tactical::write_replay(capped.record()).value()).value()).value();
        for (const auto& hash : hashes) expect(replayed.step(executor).value().state_hash.get() == hash, "capped transfer replay agrees");
        if (capped_reference.empty()) capped_reference = hashes; else expect(capped_reference == hashes, "capped transfers agree on 1/2/4/8 workers");
    }
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        // WPR-51: the station holder excludes its own type, not its allied ships.
        auto retained_setup = setup; retained_setup.campaign->upgrades = {{skirmish::type_id("Upgrade"), 45, 2}};
        retained_setup.campaign->reserves.front().owner = 2;
        auto retained_content = entry.content;
        expect(static_cast<bool>(campaign::bind_space_rules(retained_setup, inputs, retained_content)), "retained upgrade rebinds");
        auto retained = tactical::TacticalSession::create(retained_setup, retained_content).value();
        expect(retained.durability_state(21)->hull == whole(150), "retained planet upgrade applies to initial fleet");
        expect(static_cast<bool>(retained.submit({{0, 2, 1}, {}, tactical::ReinforcePayload{skirmish::type_id("ReserveShip"), {whole(500), {}, {}}, 79}})), "retained upgrade reserve command");
        expect(static_cast<bool>(retained.step(executor)), "retained upgrade deployment step");
        expect(retained.durability_state(79)->hull == whole(150), "persistent ID below allocator receives retained upgrade on arrival");
        for (const bool station_loss : {false, true}) {
            auto condition_setup = setup;
            if (station_loss) condition_setup.campaign->victory_station = 45;
            else condition_setup.campaign->victory_leaders = {7};
            auto condition_content = entry.content;
            expect(static_cast<bool>(campaign::bind_space_rules(condition_setup, inputs, condition_content)), "extra campaign condition rebinds");
            auto branch = tactical::TacticalSession::create(condition_setup, condition_content).value();
            expect(static_cast<bool>(branch.submit({{0, station_loss ? 1U : 2U, 1}, {station_loss ? 45U : 7U}, tactical::DamagePayload{whole(1000)}})), "campaign condition casualty");
            expect(static_cast<bool>(branch.step(executor)), "campaign condition step");
            expect(branch.outcome() && branch.outcome()->condition == (station_loss
                ? tactical::VictoryCondition::campaign_station_destroyed : tactical::VictoryCondition::enemy_leader_destroyed)
                && branch.outcome()->winner == (station_loss ? 1U : 2U), "extra campaign condition wins while other enemy units remain");
        }
    }
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto made = tactical::TacticalSession::create(setup, entry.content);
        expect(static_cast<bool>(made), "campaign tactical child creates");
        if (!made) { std::cerr << made.error().message << '\n'; return; }
        auto live = std::move(made).value();
        const auto reserve = skirmish::type_id("ReserveShip");
        const std::vector<tactical::PlayerCommand> commands{
            {{0, 1, 1}, {}, tactical::ReinforcePayload{reserve, {whole(-500), {}, {}}, 79}},
            {{0, 1, 2}, {}, tactical::ReinforcePayload{reserve, {whole(-500), {}, {}}, 79}},
            {{0, 1, 3}, {}, tactical::ReinforcePayload{reserve, {whole(-500), {}, {}}, 80}},
            {{0, 1, 4}, {}, tactical::ReinforcePayload{reserve, {whole(-500), {}, {}}, 0}},
            {{0, 1, 5}, {}, tactical::ReinforcePayload{reserve, {whole(-500), {}, {}}, 81}},
            {{0, 2, 1}, {45}, tactical::BuyPayload{skirmish::type_id("Upgrade")}},
            {{10, 2, 2}, {}, tactical::CampaignCreditReleasePayload{whole(100)}},
            {{36, 2, 3}, {}, tactical::CampaignCreditReleasePayload{whole(-1)}},
            {{37, 2, 4}, {}, tactical::CampaignCreditReleasePayload{whole(-1)}},
            {{200, 2, 5}, {7, 79}, tactical::DamagePayload{whole(1000)}}};
        for (const auto& command : commands) expect(static_cast<bool>(live.submit(command)), "campaign command admitted");
        const auto first = live.step(executor).value();
        const auto units = live.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& u) { return u.entity_id == 79; }) == 1,
            "reserve arrives once using persistent ID");
        expect(std::count_if(first.snapshot->events().begin(), first.snapshot->events().end(), [](const auto& e) {
            return e.kind == tactical::EventKind::order_rejected && e.order == tactical::OrderKind::reinforce;
        }) == 4, "duplicate/restricted/type-only/over-cap reserve orders refuse without consuming identity");
        expect(live.snapshot()->economy()[0].population == 2 && live.snapshot()->economy()[0].pool_tokens == std::vector<std::uint64_t>{80, 81},
            "initial campaign population and identity pool drive faction cap");
        expect(live.arrivals().at(79).direction.y == whole(1), "campaign reserve uses authored side facing");
        expect(live.next_entity_id() == *setup.first_free_entity_id, "reserve preserves shared allocator");
        std::vector<std::string> hashes{first.state_hash.get()};
        for (int tick = 1; tick < 35; ++tick) hashes.push_back(live.step(executor).value().state_hash.get());
        const auto snapshot = live.snapshot();
        const auto ledger = snapshot->economy();
        expect(ledger[1].credits == whole(2222) && ledger[1].campaign_extra_releases == Fixed{}
            && ledger[1].tech_level == 3 && ledger[1].completed.size() == 1,
            "campaign upgrade costs galactic tactical budget and completes");
        auto returned = campaign::space_rule_return(setup, tables, ledger);
        expect(returned && returned.value().credits[1].credits == whole(2222)
            && returned.value().upgrades == std::vector<tactical::CampaignUpgrade>{{skirmish::type_id("Upgrade"), 45, 2}},
            "ordered return supplies exact strategic credits and planet upgrade");
        const auto identity = checkpoint::tactical_identities(setup.content_identity);
        const auto saved = checkpoint::save_tactical_checkpoint(live, live.pending_commands(), checkpoint::default_seats(setup), identity);
        expect(static_cast<bool>(saved), "campaign checkpoint captures pending casualty command");
        if (!saved) { std::cerr << saved.error().message << '\n'; return; }
        const checkpoint::TacticalSessionFactory factory = [&](const tactical::TacticalReplay& replay) {
            auto content = entry.content;
            const auto rebound = campaign::bind_space_rules(replay.setup, inputs, content);
            if (!rebound) return core::Result<tactical::TacticalSession>::failure(rebound.error());
            return tactical::TacticalSession::from_replay(replay, content);
        };
        auto restored = checkpoint::load_tactical_checkpoint(saved.value(), identity, factory, executor);
        expect(static_cast<bool>(restored), "campaign checkpoint restores ledger, reserve consumption and policy");
        if (!restored) { std::cerr << restored.error().message << '\n'; return; }
        for (int tick = 35; tick < 412; ++tick) {
            const auto frame = live.step(executor).value();
            hashes.push_back(frame.state_hash.get());
            expect(restored.value().session.step(executor).value().state_hash.get() == frame.state_hash.get(), "checkpoint continuation matches");
        }
        expect(live.outcome() && live.outcome()->winner == 2 && live.outcome()->end_tick == 410,
            "campaign all-enemy-units victory finishes with sourced delay");
        const auto final = campaign::space_rule_return(setup, tables, live.snapshot()->economy());
        expect(final && final.value().credits == returned.value().credits && final.value().upgrades == returned.value().upgrades,
            "completed campaign victory returns exact credits and retained planet upgrade");
        expect(live.snapshot()->economy()[1].campaign_extra_releases == Fixed{}, "campaign space release-all remains inactive");
        const auto replay = tactical::parse_replay(tactical::write_replay(live.record()).value()).value();
        expect(replay.setup == setup, "campaign replay metadata roundtrips");
        auto replayed = factory(replay).value();
        for (const auto& hash : hashes) expect(replayed.step(executor).value().state_hash.get() == hash, "campaign replay matches");
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "1/2/4/8 workers agree");
    }
    auto invalid = setup; invalid.campaign->reserves[0].id = 7;
    expect(!tactical::validate_setup(invalid), "reserve cannot reuse a spatial identity");
    invalid = setup; invalid.campaign->reserves[0].owner = 99;
    expect(!tactical::validate_setup(invalid), "reserve owner requires a campaign account");
    tactical::PlayerEconomy balance; balance.credits = whole(999);
    expect(tactical::change_credits(balance, setup.campaign->players[0], whole(10)) && balance.credits == whole(1000),
        "positive changes obey galactic credit cap");
}
void carrier_codec() {
    tactical::TacticalReplay replay;
    replay.setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    replay.setup.units = {{7, 10, 1}}; replay.setup.first_free_entity_id = 100;
    auto& carrier = replay.setup.units.front(); carrier.purchase_type = 11;
    tactical::CarriedObject rider; rider.entity_id = 8; rider.type_id = 12; rider.parent = 7; rider.named_hero = true;
    tactical::CarriedObject member; member.entity_id = 9; member.type_id = 13; member.parent = 8;
    rider.members.push_back(member); carrier.contained.push_back(rider);
    const auto bytes = tactical::write_replay(replay);
    expect(bytes && tactical::parse_replay(bytes.value()).value() == replay, "initial carrier tree has canonical ext-8 roundtrip");
    auto bad = replay; bad.setup.units.front().contained.front().members.front().parent = 7;
    expect(!tactical::write_replay(bad), "wrong nested parent refuses");
    bad = replay; bad.setup.units.front().contained.front().entity_id = 7;
    expect(!tactical::write_replay(bad), "carried/spatial identity reuse refuses");
    bad = replay; bad.setup.units.front().contained.front().model_visible = true;
    expect(!tactical::write_replay(bad), "spatial carried flags refuse");
    bad = replay; bad.setup.units.front().contained.front().entity_id = 100;
    expect(!tactical::write_replay(bad), "carried frontier collision refuses");
}
void reserve_admission() {
    const auto type = skirmish::type_id("ReserveShip");
    const std::array<tactical::CampaignReserve, 3> reserves{{{79, type, 1, false, 0, {}}, {80, type, 2, false, 0, {}}, {81, type, 1, true, 0, {}}}};
    const std::array<std::uint64_t, 3> tokens{79, 80, 81};
    tactical::SeatBinding binding{sim::SeatId{1}, 1};
    tactical::SeatCommand intent; intent.key = {0, sim::SeatId{1}, 0}; intent.player = 1;
    intent.payload = tactical::ReinforcePayload{type, {}, 79};
    tactical::SeatAdmission lan;
    expect(!tactical::human_wire_payload(intent.payload) && !tactical::admit_human_command(binding, intent)
        && !lan.admit(binding, intent), "standalone/skirmish/LAN refuse nonzero engine purchase tokens");
    tactical::CampaignReserveAdmission context{true, 1, reserves, tokens};
    const auto admitted = tactical::admit_human_command(binding, intent, context);
    expect(admitted && std::get<tactical::ReinforcePayload>(admitted.value().command.payload).pool_token == 79,
        "trusted active campaign binding admits an owned available persistent reserve");
    context.active = false;
    expect(!tactical::admit_human_command(binding, intent, context), "inactive campaign context cannot admit a reserve");
    context.active = true; context.player = 2;
    expect(!tactical::admit_human_command(binding, intent, context), "campaign context must match the trusted human faction");
    context.player = 1; context.available_tokens = {};
    expect(!tactical::admit_human_command(binding, intent, context), "consumed campaign reserve is not available for admission");
    context.available_tokens = tokens;
    for (const auto id : {80U, 81U, 999U}) {
        std::get<tactical::ReinforcePayload>(intent.payload).pool_token = id;
        expect(!tactical::admit_human_command(binding, intent, context), "foreign/restricted/unknown reserve identity refuses");
    }
    std::get<tactical::ReinforcePayload>(intent.payload) = {type + 1, {}, 79};
    expect(!tactical::admit_human_command(binding, intent, context), "reserve type cannot be substituted");
    intent.payload = tactical::CampaignCreditReleasePayload{whole(10)};
    expect(tactical::payload_authority(intent.payload) == tactical::PayloadAuthority::engine_only
        && !tactical::admit_human_command(binding, intent, context), "campaign context cannot admit engine credit release");
}
void presentation_gates() {
    bool done{};
    campaign::SpaceBriefingContent content{{{10, 11, 12, "OR_FIRST"}, {10, 0, 0, "OR_SECOND"}},
        {{10, 11, 0, "AND"}}, "DEFAULT", "MOVIE"};
    const std::array<std::uint64_t, 2> present{10, 11};
    campaign::SpaceBriefingGate gate;
    gate.logical_frame = 299;
    expect(!campaign::service_space_briefing(done, gate, content, present) && !done, "briefing waits for ten logical seconds");
    gate.logical_frame = 300; gate.cinematic_playing = true;
    expect(!campaign::service_space_briefing(done, gate, content, present) && !done, "briefing waits for cinematic completion");
    gate.cinematic_playing = false; gate.speech_queue_empty = false;
    expect(!campaign::service_space_briefing(done, gate, content, present) && !done, "briefing waits for empty speech queue");
    gate.speech_queue_empty = true;
    const auto request = campaign::service_space_briefing(done, gate, content, present);
    expect(request && request->speech == "OR_FIRST" && request->command_bar_movie == "MOVIE" && done,
        "first qualifying OR precedes AND and default");
    expect(!campaign::service_space_briefing(done, gate, content, present), "briefing never retries");
    done = false; const std::array<std::uint64_t, 3> excluded{10, 11, 12};
    expect(campaign::service_space_briefing(done, gate, content, excluded)->speech == "OR_SECOND", "C excludes an authored conditional row");
    done = false; gate.parent_mode = false;
    expect(!campaign::service_space_briefing(done, gate, content, present) && done, "standalone has no campaign briefing");
    campaign::SpaceIntroGate intro{true, true, true, false, true, true, false};
    expect(campaign::space_intro_allowed(intro), "eligible local space attacker gets cinematic attempt");
    intro.network = true; expect(!campaign::space_intro_allowed(intro), "network intro falls back to fade");
    script::foc::AiSetup ai; campaign::ConflictRecord conflict; conflict.defender = 2; conflict.station_level = 4;
    campaign::configure_space_ai(ai, conflict);
    expect(ai.perception.campaign_game && ai.perception.campaign_defender == 2U && ai.perception.base_level == 4,
        "AI context carries campaign, per-player defender and current base level");
}
std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* value = nullptr; std::size_t length{};
    if (_dupenv_s(&value, &length, "EAWR_EAW_GAME_ROOT") != 0 || !value) return {};
    std::string result(value); std::free(value);
    return result.empty() ? std::nullopt : std::optional<std::string>{result};
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    return value && *value ? std::optional<std::string>{value} : std::nullopt;
#endif
}
void installed() {
    const auto root = game_root();
    if (!root) { std::cout << "SKIPPED: campaign space rules require EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"}) {
        auto manifest = vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "installed manifest"); if (!manifest) return;
        mounts.push_back(manifest.value().mount);
    }
    auto files = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(files), "installed VFS"); if (!files) return;
    auto catalog = data::load_catalog(files.value(), data::Profile::foc);
    expect(static_cast<bool>(catalog), "installed catalog"); if (!catalog) return;
    units::LoadInput load; load.catalog = &catalog.value().catalog; load.filesystem = &files.value();
    load.types = {"Empire_Star_Base_1"};
    auto tables = units::load_unit_tables(load);
    expect(static_cast<bool>(tables), "installed campaign production closure"); if (!tables) return;
    auto input = skirmish::read_start_inputs(skirmish::m2_fixture(), files.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(input), "installed start inputs"); if (!input) return;
    const auto* station = tables.value().find("Empire_Star_Base_1");
    expect(station && !station->production.buildable_campaign.empty(), "stock station authors campaign menu"); if (!station) return;
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, skirmish::faction_id("Rebel"), tactical::player_flag_commandable},
        {2, 2, skirmish::faction_id("Empire"), tactical::player_flag_commandable}};
    setup.units = {{45, skirmish::type_id(station->id), 2}};
    setup.first_free_entity_id = 100;
    setup.match_policy = tactical::SkirmishMatchPolicy{true, true, false, true};
    setup.campaign.emplace();
    for (const tactical::PlayerId id : {1U, 2U}) {
        tactical::EconomyPlayer account{id, whole(777), 20, id == 2, {}, 3, 5}; account.credit_cap = whole(1000);
        setup.campaign->players.push_back(account);
    }
    tactical::TacticalContent content;
    const auto bound = campaign::bind_space_rules(setup, input.value(), content);
    expect(static_cast<bool>(bound), "installed campaign economy binds");
    if (!bound) { std::cerr << bound.error().message << '\n'; return; }
    std::size_t checked{};
    for (const auto& group : station->production.buildable_campaign) {
        const auto* menu = content.economy.menu(skirmish::type_id(station->id), skirmish::faction_id(group.faction));
        expect(menu && menu->options.size() == group.types.size(), "installed authored campaign menu has exact entries");
        if (!menu) continue;
        for (const auto& name : group.types) {
            const auto* type = tables.value().find(name);
            const auto option = std::find_if(menu->options.begin(), menu->options.end(), [&](const auto& row) { return row.type == skirmish::type_id(name); });
            expect(type && option != menu->options.end() && option->price == type->production.build_cost_campaign.value_or(Fixed{}),
                "installed menu uses authored campaign cost");
            ++checked;
        }
    }
    expect(checked != 0 && content.economy.players[0].credits == whole(777)
        && content.economy.players[0].start_tech == 3, "stock content keeps nondefault GC ledger and tech");
    skirmish::SkirmishStart start; start.setup = setup;
    start.players = {{setup.players[0], "Rebel", true, true, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}}, {setup.players[1], "Empire", true, false, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}}};
    auto ai = skirmish::ai_setup(start, input.value(), tables.value());
    campaign::ConflictRecord conflict; conflict.defender = 2; conflict.station_level = 1;
    campaign::configure_space_ai(ai, conflict);
    const auto goals = skirmish::enable_goal_system(files.value(), ai);
    expect(static_cast<bool>(goals), "stock campaign AI goal system loads"); if (!goals) return;
    const auto modules = skirmish::ai_modules(files.value(), ai, true);
    expect(static_cast<bool>(modules), "stock campaign AI modules load"); if (!modules) return;
    auto prepared = script::foc::prepare_session(ai, modules.value());
    expect(static_cast<bool>(prepared), "stock template prepares scheduler and shared AI host"); if (!prepared) return;
    auto world = tactical::TacticalSession::create(setup, content);
    expect(static_cast<bool>(world), "stock campaign child creates"); if (!world) return;
    auto child = script::authoritative::ScriptedTacticalSession::create(std::move(world).value(), std::move(prepared.value().scripts));
    expect(child && prepared.value().bind(child.value()), "prepared AI binds existing child owner");
    if (child) expect(static_cast<bool>(child.value().step(sim::InlineExecutor{})), "campaign child services stock AI");
}
}
int main(int argc, char**) {
    try { if (argc > 1) installed(); else { proof(); carrier_codec(); reserve_admission(); presentation_gates(); } }
    catch (const std::exception& error) { expect(false, error.what()); }
    return galactic_test::failures == 0 ? 0 : 1;
}

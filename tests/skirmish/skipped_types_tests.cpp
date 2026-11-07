// AG-11: authored profile errors omit their placements and future launches; the match keeps stepping.
#include "../support/check.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>

#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

eawr::core::Diagnostic skip(const std::string& id, const std::string& why) {
    eawr::core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-UNITS-0005";
    diagnostic.message = "skipped unit type '" + id + "': " + why;
    return diagnostic;
}

eawr::battle::StartUnit unit(const std::string& type, std::uint32_t entity) {
    eawr::battle::StartUnit placed;
    placed.type = type;
    placed.state.entity_id = entity;
    placed.state.type_id = eawr::battle::type_id(type);
    placed.state.owner = 1;
    return placed;
}

} // namespace

int main() {
    eawr::testing::Checker checker;
    const auto check = [&](bool condition, std::string_view message) { checker.check(condition, message, std::cerr); };

    const std::vector<eawr::core::Diagnostic> diagnostics{
        skip("Republic_Light_Assault_Cruiser", "bad rows"), skip("Republic_Light_Assault_Cruiser", "again"),
        skip("Another_Type", "worse")};
    auto note = diagnostics.front();
    note.message = "projectile 'P' authors an ion stun reduction beyond +-1024; it saturates there (IS-10)";
    auto with_note = diagnostics;
    with_note.push_back(note);
    const auto ids = eawr::skirmish::skipped_type_ids(with_note);
    check(ids.size() == 2 && ids[0] == "Another_Type" && ids[1] == "Republic_Light_Assault_Cruiser",
        "skip ids are sorted and unique; warnings are not types");
    auto warning = note;
    warning.severity = eawr::core::Severity::warning;
    warning.message = "unit 'Retained_Type' ability 'HARMONIC_BOMB': creation skipped; ability retained";
    const std::vector<eawr::core::Diagnostic> warnings_only{warning, warning};
    check(eawr::skirmish::content_load_summary(warnings_only) == "0 unit types skipped, 1 unit with content warnings"
            && eawr::skirmish::skipped_type_ids(warnings_only).empty(),
        "#5064: warning-only content retains its unit and counts it once");
    with_note.push_back(warning);
    with_note.push_back(warning);
    warning.message = "unit type 'Retained_Type' authors a repeated ability";
    with_note.push_back(warning);
    warning.message = "unit 'Another_Type' ability warning before its rejection";
    with_note.push_back(warning);
    check(eawr::skirmish::content_load_summary(with_note) == "2 unit types skipped, 1 unit with content warnings",
        "#5064: duplicate diagnostics count types once; rejected types are excluded from retained warnings");
    warning.message = "projectile 'P' has a content warning";
    with_note.push_back(warning);
    check(eawr::skirmish::content_load_summary(with_note)
            == "2 unit types skipped, 1 unit with content warnings, 1 other content warning",
        "#5064: warnings without a unit owner remain visible separately");
    check(eawr::skirmish::content_load_summary({}) == "0 unit types skipped, 0 units with content warnings",
        "#5064: clean content has zero rejected and warned types");

    // A rejected craft removes its whole squadron and garrison references, preserving IDs.
    eawr::battle::BattleStart start;
    start.units.push_back(unit("Victory_Class_Star_Destroyer", 1));
    start.units.push_back(unit("republic_light_assault_cruiser", 2));
    start.units.push_back(unit("Squadron_A", 3));
    start.units.push_back(unit("Another_Type", 4));
    start.units.push_back(unit("Healthy_Craft", 5));
    start.units[2].craft_type = "Another_Type";
    start.units.push_back(unit("Survivor", 8));
    for (const auto& placed : start.units) start.setup.units.push_back(placed.state);
    start.setup.squadrons.push_back({3, {4, 5}});
    eawr::sim::tactical::FreeGarrisonSetup garrison;
    garrison.player = 1;
    garrison.templates = {eawr::battle::type_id("Squadron_A"), eawr::battle::type_id("Healthy_Craft"),
        eawr::battle::type_id("Survivor")};
    garrison.registered = {4, 5, 8};
    start.setup.free_garrisons.push_back(garrison);
    start.launches.resize(2);
    start.launches[0].spawner = 1;
    start.launches[0].squadron = "Another_Type";
    start.launches[1].spawner = 2;
    start.launches[1].squadron = "Healthy_Craft";
    // The last setup unit is deliberately not the highest ID.
    std::swap(start.setup.units[1], start.setup.units.back());
    start.arrivals.add(1, "Another_Type", 1, eawr::battle::ArrivalSource::launch, 1);
    start.arrivals.add(1, "Healthy_Craft", 1, eawr::battle::ArrivalSource::launch, 2);
    auto recorded = start.setup;
    std::vector<eawr::core::LoadNote> notes;
    eawr::skirmish::drop_skipped_starts(start, diagnostics, &notes);
    check(start.units.size() == 2 && start.units[0].state.entity_id == 1 && start.units[1].state.entity_id == 8
            && start.setup.units.size() == 2 && start.setup.squadrons.empty() && start.launches.empty(),
        "rejected placements, whole squadrons and dependent launches are omitted; surviving IDs remain");
    check(start.setup.free_garrisons.size() == 1 && start.setup.free_garrisons[0].registered == std::vector<eawr::sim::EntityId>{8}
            && start.setup.free_garrisons[0].templates == std::vector<eawr::sim::tactical::TypeId>{
                eawr::battle::type_id("Healthy_Craft"), eawr::battle::type_id("Survivor")},
        "garrisons omit rejected teams and registrations while retaining independent healthy craft templates");
    check(start.arrivals.rows.empty() && std::any_of(notes.begin(), notes.end(), [](const auto& load) {
            return load.object.starts_with("arrival ");
        }) && std::any_of(notes.begin(), notes.end(), [](const auto& load) {
            return load.object.starts_with("free garrison ");
        }), "arrival and garrison template omissions emit load notes");
    check(!notes.empty() && notes.front().code == "EAWR-LOAD-0450"
            && notes.front().classification == eawr::core::LoadCategory::unknown_degrade
            && notes.front().action == eawr::core::LoadAction::skipped_placement
            && notes.front().message.find("bad rows") != std::string::npos,
        "placement notes preserve the original diagnostic and identify the omitted placement");
    const auto filtered = start.setup;
    const auto count = notes.size();
    eawr::skirmish::drop_skipped_starts(start, diagnostics, &notes);
    check(start.setup == filtered && notes.size() == count, "filtering is idempotent");
    eawr::skirmish::drop_skipped_setup(recorded, diagnostics, &notes);
    check(recorded == filtered, "recorded and live setups omit the same squadron and garrison references");
    check(recorded.first_free_entity_id == 9, "allocator uses the maximum ID when setup units are out of order");
    auto riders = filtered;
    riders.first_free_entity_id.reset();
    eawr::sim::tactical::CarriedObject carried;
    carried.entity_id = 50;
    carried.type_id = eawr::battle::type_id("Healthy_Team");
    carried.parent = 1;
    eawr::sim::tactical::CarriedObject child;
    child.entity_id = 51;
    child.type_id = eawr::battle::type_id("Another_Type");
    child.parent = 50;
    carried.members.push_back(child);
    riders.units.front().contained.push_back(carried);
    auto survivor = carried;
    survivor.entity_id = 60;
    survivor.members.clear();
    riders.units.front().contained.push_back(survivor);
    eawr::skirmish::drop_skipped_setup(riders, diagnostics, &notes);
    check(riders.units.front().contained.size() == 1 && riders.units.front().contained.front().entity_id == 60
            && riders.first_free_entity_id == 61 && std::any_of(notes.begin(), notes.end(), [](const auto& load) {
                return load.object.starts_with("contained placement 50");
            }), "rejected nested riders omit their whole carried placement, retain healthy riders and preserve the allocator");
    auto clean = filtered;
    eawr::skirmish::drop_skipped_setup(clean, {});
    check(clean == filtered, "clean content retains exact setup bytes");

    // Synthetic authored ability error: both a direct ship launch and a squadron of
    // rejected craft must disappear from the actual spawner rules, including reserves.
    namespace t = eawr::sim::tactical;
    using Fixed = eawr::sim::math::Fixed;
    eawr::units::UnitTables tables;
    eawr::units::UnitType bad;
    bad.id = "Bad_Ship";
    bad.hull = Fixed::from_raw(100 * Fixed::scale);
    eawr::units::Ability ability;
    ability.type = "DEFEND";
    ability.modifiers.push_back({"SPEED_MULTIPLIER", Fixed::from_raw(-Fixed::scale)});
    bad.abilities.push_back(ability);
    tables.units.push_back(bad);
    auto craft = bad;
    craft.id = "Bad_Craft";
    craft.kind = eawr::units::UnitKind::craft;
    craft.movement.max_speed = Fixed::from_raw(Fixed::scale);
    craft.movement.max_rate_of_turn = Fixed::from_raw(Fixed::scale);
    tables.units.push_back(craft);
    eawr::units::UnitType team;
    team.id = "Bad_Team";
    team.kind = eawr::units::UnitKind::squadron;
    team.members.push_back({"Bad_Craft", 1, std::nullopt});
    tables.units.push_back(team);
    eawr::units::UnitType carrier;
    carrier.id = "Carrier";
    carrier.hull = bad.hull;
    carrier.spawner.emplace();
    carrier.spawner->starting.push_back({"Bad_Ship", 0, 1});
    carrier.spawner->starting.push_back({"Bad_Team", 2, 1});
    carrier.spawner->reserves.push_back({"Bad_Ship", 0, 5});
    carrier.spawner->reserves.push_back({"Bad_Team", 2, 5});
    carrier.spawner->reserves_used = true;
    tables.units.push_back(carrier);
    std::vector<eawr::core::Diagnostic> rejected;
    auto content = eawr::skirmish::session_content(tables, {}, {}, eawr::units::ContentErrors::skip_object, &rejected);
    check(static_cast<bool>(content), "authored ability errors accept the retained content table");
    if (content) {
        const auto* spawner = content.value().motion.squadrons.find_spawner(eawr::battle::type_id("Carrier"));
        check(spawner && spawner->entries.empty(), "actual spawner rules omit rejected ships, teams and their reserves");
        check(eawr::skirmish::skipped_type_ids(rejected) == eawr::skirmish::skipped_type_ids_of(tables),
            "live and replay identity include the same dependent rejected teams");
        check(std::any_of(content.value().load_notes.begin(), content.value().load_notes.end(), [](const auto& load) {
            return load.code == "EAWR-LOAD-0450" && load.tag == "Starting_Spawned_Units_Tech_0";
        }), "omitted future launches carry a load note");
        t::TacticalSetup setup;
        t::Player player;
        player.player_id = 1;
        setup.players.push_back(player);
        setup.units.push_back(unit("Carrier", 1).state);
        setup.units.push_back(unit("Bad_Ship", 2).state);
        eawr::skirmish::drop_skipped_setup(setup, rejected, &content.value().load_notes);
        auto session = t::TacticalSession::create(setup, content.value().sensors, content.value().durability,
            content.value().motion, std::nullopt, content.value().combat, {}, content.value().abilities);
        check(static_cast<bool>(session), "filtered authored-error setup creates a running match");
        if (session) {
            const eawr::sim::InlineExecutor executor;
            for (int tick = 0; tick < 3; ++tick) check(static_cast<bool>(session.value().step(executor)), "match keeps stepping after skipped launches");
            check(session.value().units().size() == 1, "no rejected type appears through delayed or reserve launches");
        }
    }

    // AG-12: authored powers can exceed fixed storage and the consumer's supported
    // rating range without rejecting the content or ending a match.
    eawr::units::UnitTables arithmetic_tables;
    eawr::units::UnitType power_craft;
    power_craft.id = "Power_Craft";
    power_craft.kind = eawr::units::UnitKind::craft;
    power_craft.ai_combat_power = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    arithmetic_tables.units.push_back(power_craft);
    eawr::units::UnitType power_team;
    power_team.id = "Power_Team";
    power_team.kind = eawr::units::UnitKind::squadron;
    power_team.members.assign(2, {"Power_Craft", 0, std::nullopt});
    arithmetic_tables.units.push_back(power_team);
    eawr::units::Projectile power_projectile;
    power_projectile.id = "Power_Projectile";
    power_projectile.ai_combat_power = power_craft.ai_combat_power;
    arithmetic_tables.projectiles.push_back(power_projectile);
    eawr::units::UnitType power_ship;
    power_ship.id = "Power_Ship";
    power_ship.ai_combat_power = power_craft.ai_combat_power;
    power_ship.hull = Fixed::from_raw(100 * Fixed::scale);
    eawr::units::Hardpoint power_hardpoint;
    power_hardpoint.id = "Power_Hardpoint";
    power_hardpoint.type = eawr::units::HardpointType::weapon_laser;
    power_hardpoint.weapon.emplace();
    power_hardpoint.weapon->projectile_index = 0;
    power_ship.hardpoints.assign(2, power_hardpoint);
    power_ship.spawner.emplace();
    power_ship.spawner->starting.push_back({"Power_Team", 1, 2});
    arithmetic_tables.units.push_back(power_ship);
    auto arithmetic_content = eawr::skirmish::session_content(arithmetic_tables);
    check(static_cast<bool>(arithmetic_content), "power sum overflow and oversized ratings accept the content");
    if (arithmetic_content) {
        const auto maximum_rating = Fixed::from_raw(t::max_weapon_combat_power * Fixed::scale);
        check(arithmetic_content.value().abilities.combat_rating(eawr::battle::type_id("Power_Team")) == maximum_rating,
            "squadron rating saturates and clamps to the consuming table's range");
        const auto* power_profile = arithmetic_content.value().combat.find(eawr::battle::type_id("Power_Ship"));
        check(power_profile && power_profile->weapons.size() == 2
                && power_profile->weapons.front().ai_combat_power == maximum_rating,
            "hardpoint power sums and weighted ratings degrade safely");
        check(std::any_of(arithmetic_content.value().load_notes.begin(), arithmetic_content.value().load_notes.end(), [](const auto& load) {
            return load.code == "EAWR-LOAD-0250" && !load.message.empty();
        }), "saturated power retains the arithmetic diagnostic in a class C note");
        t::TacticalSetup arithmetic_setup;
        t::Player arithmetic_player;
        arithmetic_player.player_id = 1;
        arithmetic_setup.players.push_back(arithmetic_player);
        arithmetic_setup.units.push_back(unit("Power_Ship", 1).state);
        auto arithmetic_session = t::TacticalSession::create(arithmetic_setup, arithmetic_content.value().sensors,
            arithmetic_content.value().durability, arithmetic_content.value().motion, std::nullopt,
            arithmetic_content.value().combat, {}, arithmetic_content.value().abilities);
        check(static_cast<bool>(arithmetic_session), "saturated power content creates a match");
        if (arithmetic_session) {
            const eawr::sim::InlineExecutor arithmetic_executor;
            check(static_cast<bool>(arithmetic_session.value().step(arithmetic_executor)), "saturated power match keeps stepping");
        }
    }
    eawr::skirmish::Fixture power_fixture;
    power_fixture.map = "synthetic-power-map";
    power_fixture.map_sha256 = std::string(64, 'a');
    power_fixture.slots.push_back({1, "Synthetic", 0, true, {}});
    eawr::skirmish::StartInputs power_inputs;
    power_inputs.map = power_fixture.map;
    power_inputs.map_sha256 = power_fixture.map_sha256;
    power_inputs.tables = &arithmetic_tables;
    eawr::battle::MapPlacement power_marker;
    power_marker.record = 1;
    power_marker.type = "Synthetic_Marker";
    power_marker.marker = true;
    power_marker.position = eawr::units::Vec3{};
    power_marker.orientation_degrees = eawr::units::Vec3{};
    power_inputs.placements.push_back(power_marker);
    const std::vector<eawr::skirmish::SelectedStartUnit> power_selected{
        {1, "Power_Ship", eawr::battle::UnitRole::fleet, 1}, {1, "Power_Ship", eawr::battle::UnitRole::fleet, 1}};
    const auto power_start = eawr::skirmish::build_selected_start(power_fixture, power_inputs, power_selected);
    check(static_cast<bool>(power_start), "oversized authored starting and launch power builds a start");
    if (power_start) {
        check(power_start.value().players.front().combat_power_tick_zero.raw() == std::numeric_limits<std::int64_t>::max()
                && power_start.value().players.front().combat_power_launches.raw() == std::numeric_limits<std::int64_t>::max()
                && !power_start.value().load_notes.empty(),
            "starting totals and launch products saturate with notes instead of becoming zero");
    }

    // Distinct authored names with an equal CRC. The earlier source definition is
    // deliberately second in load-reference order and has a different unit kind.
    eawr::units::UnitTables collision_tables;
    eawr::units::UnitType crc_loser;
    crc_loser.id = "SYNTH_OIVRHNPFF4K4";
    crc_loser.table_order = 9;
    crc_loser.kind = eawr::units::UnitKind::squadron;
    crc_loser.unit_ai = true;
    crc_loser.team_reveal_range = Fixed::from_raw(100 * Fixed::scale);
    crc_loser.team_abilities.push_back(ability); // invalid; it must never reach a derived table
    eawr::units::UnitType crc_winner;
    crc_winner.id = "SYNTH_QLU1MOI5PTT3";
    crc_winner.table_order = 2;
    crc_winner.hull = Fixed::from_raw(100 * Fixed::scale);
    crc_winner.ai_combat_power = Fixed::from_raw(7 * Fixed::scale);
    crc_winner.unit_ai = true;
    crc_winner.reveal = true;
    crc_winner.fow_reveal_range = Fixed::from_raw(10 * Fixed::scale);
    crc_winner.movement.max_speed = Fixed::from_raw(Fixed::scale);
    crc_winner.movement.max_rate_of_turn = Fixed::from_raw(Fixed::scale);
    crc_winner.movement.space_layer = "Frigate";
    crc_winner.targeting_priority_set_index = 0;
    auto crc_ability = ability;
    crc_ability.modifiers.front().value = Fixed::from_raw(Fixed::scale / 2);
    crc_winner.abilities.push_back(crc_ability);
    collision_tables.units = {crc_loser, crc_winner};
    collision_tables.priority_sets.emplace_back();
    const auto collision_id = eawr::battle::type_id(crc_winner.id);
    check(collision_id == eawr::battle::type_id(crc_loser.id), "synthetic names exercise a real name-CRC collision");
    auto collision_content = eawr::skirmish::session_content(collision_tables);
    check(static_cast<bool>(collision_content), "cross-kind CRC collision accepts one coherent content table");
    if (collision_content) {
        const auto& crc_content = collision_content.value();
        check(crc_content.combat.profiles.size() == 1 && crc_content.combat.retaliation_team_types.empty()
                && crc_content.combat.footprints.size() == 1 && crc_content.combat.unit_ai_types.size() == 1
                && crc_content.combat.priority_sets.size() == 1 && crc_content.combat.priority_sets[0].rows.size() == 1,
            "combat profiles, footprints, AI types, retaliation teams and priority rows share the first definition");
        check(crc_content.abilities.profiles.size() == 1
                && crc_content.abilities.combat_rating(collision_id) == *crc_winner.ai_combat_power
                && crc_content.motion.profiles.size() == 1 && crc_content.durability.profiles.size() == 1
                && crc_content.sensors.size() == 1 && crc_content.sensors.front().reveal_range == *crc_winner.fow_reveal_range,
            "ability, motion, durability and sensor rows share that same source-order winner");
        check(std::any_of(crc_content.load_notes.begin(), crc_content.load_notes.end(), [&](const auto& load) {
            return load.code == "EAWR-LOAD-0251" && load.object == crc_loser.id && load.effective_value == crc_winner.id;
        }), "CRC collision notes identify the omitted definition and retained winner");
    }
    auto collision_inputs = power_inputs;
    collision_inputs.tables = &collision_tables;
    const std::vector<eawr::skirmish::SelectedStartUnit> collision_selected{
        {1, crc_loser.id, eawr::battle::UnitRole::fleet, 1}};
    const auto collision_start = eawr::skirmish::build_selected_start(power_fixture, collision_inputs, collision_selected);
    check(collision_start && collision_start.value().units.size() == 1
            && collision_start.value().units.front().type == crc_winner.id
            && collision_start.value().units.front().combat_power == crc_winner.ai_combat_power
            && !collision_start.value().load_notes.empty(),
        "starting a colliding name uses the same source-order winner and publishes its diagnostic");

    // B1: purchasable types of a skipped type leave the menus, with a log line each.
    eawr::sim::tactical::EconomyRules rules;
    rules.menus.resize(1);
    rules.menus[0].station = 7;
    for (const char* name : {"Victory_Class_Star_Destroyer", "Another_Type", "Republic_Light_Assault_Cruiser"}) {
        eawr::sim::tactical::BuildOption option;
        option.type = eawr::battle::type_id(name);
        rules.menus[0].options.push_back(option);
    }
    std::vector<std::string> log;
    check(eawr::skirmish::drop_skipped_menu_options(rules, ids, &log) == 2 && log.size() == 2
            && rules.menus[0].options.size() == 1
            && rules.menus[0].options[0].type == eawr::battle::type_id("Victory_Class_Star_Destroyer"),
        "menu entries of skipped types are dropped and logged");
    check(eawr::skirmish::drop_skipped_menu_options(rules, {}, &log) == 0, "no skip set drops nothing");

    // N3: the skip set binds into the match identity; none leaves it unchanged.
    eawr::battle::BattleStart bound;
    bound.identity = eawr::sim::compute_match_identity({});
    bound.setup.match_identity = bound.identity;
    const auto before = bound.identity.sha256;
    eawr::skirmish::bind_skipped_types(bound, {});
    check(bound.identity.sha256 == before, "an empty skip set leaves the identity");
    eawr::skirmish::bind_skipped_types(bound, ids);
    const auto first = bound.identity.sha256;
    check(first != before && bound.setup.match_identity && bound.setup.match_identity->sha256 == first,
        "a skip set changes the start's and the setup's identity alike");
    eawr::battle::BattleStart other;
    other.identity = eawr::sim::compute_match_identity({});
    eawr::skirmish::bind_skipped_types(other, std::vector<std::string>{"Another_Type"});
    check(other.identity.sha256 != first, "peers with different skip sets get different identities");
    eawr::skirmish::bind_skipped_types(bound, ids);
    check(bound.identity.sha256 == first, "binding the same set again is stable");

    return checker.finish();
}

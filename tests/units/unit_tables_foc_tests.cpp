#include "../support/env.hpp"
#include "unit_tables_foc_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace unit_tables_test_support {


void foc_fleet(const bool formation_only, const bool fire_bones_only) {
    const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC fleet: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return;
    // #793 (R-10, R-13, C-06): the installed FoC profile loads with no failed registry and no failed include.
    expect(catalog.value().health.registry_invalid == 0 && catalog.value().health.include_failed == 0 &&
               !catalog.value().health.failed(),
           ("FoC catalog load health: " + eawr::data::describe_load_health(catalog.value().health)).c_str());
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    if (fire_bones_only) {
        foc_asymmetric_release_geometry(input);
        return;
    }
    if (formation_only) {
        foc_formation_deceleration(input);
        return;
    }
    foc_bomber_torpedo_volley(input);
    foc_passive_projectile_defence(input);
    foc_active_projectile_defence(input);
    foc_space_healing(input);
    foc_stealth(input);
    foc_laser_defence(input);
    foc_redirect_blaster(input);
    foc_super_capitals(input);
    foc_hazard_maps(input);
    foc_mass_drivers(input);
    foc_diamond_boron_cluster(input);
    foc_barrage_buttons(input);
    foc_full_salvo(input);
    foc_self_destruct(input);
    foc_corrupt_systems(input);
    foc_peacebringer_blast(input);
    foc_asymmetric_release_geometry(input);
    foc_special_without_blast(input);
    foc_leech_shields(input);
    foc_lure(input);
    foc_hardpoint_ranks(input);
    foc_land_companies(input);
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "FoC unit tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();
    for (const auto& unit : tables.units) for (const auto& hardpoint : unit.hardpoints) {
        if (!hardpoint.destroyable) continue;
        expect(hardpoint.health && hardpoint.health->raw() > 0,
            "HD-06: stock destroyable hardpoint has authored positive Health: " + unit.id + "/" + hardpoint.id);
    }
    foc_planetary_sources(tables, input);
    // WWP-72: only the Nebulon-B in the pinned ship/craft roster has nonzero aim height.
    // Live map structures are checked separately by the resource-container fixture.
    for (const auto& id : eawr::units::pinned_m2_types()) {
        const auto* type = tables.find(id);
        const auto expected = id == "Nebulon_B_Frigate" ? raw(35) : 0;
        expect(type && type->ranged_target_z_adjust.value_or(Fixed{}).raw() == expected,
            "WWP-72: pinned M2 aim adjustment for " + std::string{id});
        if (!type) continue;
        for (const auto& member : type->members) {
            const auto* craft = tables.find(member.craft);
            expect(craft && craft->ranged_target_z_adjust.value_or(Fixed{}).raw() == 0,
                "WWP-72: pinned M2 member craft has zero/absent aim adjustment: " + member.craft);
        }
    }
    // WHE-04/49: all eleven authored skirmish purchases and five conversions load.
    auto hero_input = input;
    hero_input.types = {"Han_Solo_Team_Space_MP", "Sundered_Heart", "Rogue_Squadron_Space", "Home_One",
            "Boba_Fett_Team_Space_MP", "Darth_Team_Space_MP", "Admonitor_Star_Destroyer", "Accuser_Star_Destroyer",
            "Bossk_Team_Space_MP", "IG88_Team_Space_MP", "The_Peacebringer", "Captain_Piet"};
    // The pinned M2 roster has Rebel/Empire stations; an explicit purchase closure
    // also covers the Underworld companies without expanding that fixture's roster.
    const auto hero_loaded = eawr::units::load_unit_tables(hero_input);
    expect(static_cast<bool>(hero_loaded), "WHE-04: three-faction hero purchase closure loads");
    for (const auto& name : hero_input.types)
        expect(hero_loaded && hero_loaded.value().find(name) != nullptr, std::string("WHE-04: menu purchase loaded: ") + name);
    for (const auto& [purchase, deployment, riders] : {
            std::tuple{"Han_Solo_Team_Space_MP", "Millennium_Falcon", false},
            std::tuple{"Boba_Fett_Team_Space_MP", "Slave_I", true},
            std::tuple{"Darth_Team_Space_MP", "Darth_Vader_TIE_Fighter_Squadron", true},
            std::tuple{"Bossk_Team_Space_MP", "HoundsTooth", false},
            std::tuple{"IG88_Team_Space_MP", "IG-2000", false}}) {
        const auto* company = hero_loaded ? hero_loaded.value().find(purchase) : nullptr;
        expect(company && company->deployed_space_type == deployment && company->creates_carried_heroes == riders,
            std::string("WHE-49: company conversion: ") + purchase);
    }
    if (hero_loaded) {
        for (const auto* name : {"Boba_Fett", "Captain_Piet"}) {
            const auto* carried = hero_loaded.value().find(name);
            expect(carried && !carried->hull && carried->model_path.empty(),
                std::string("WHE-07/49: installed carried hero retains no independent space body: ") + name);
        }
        const auto* capital = hero_loaded.value().find("Accuser_Star_Destroyer");
        expect(capital && capital->hull && capital->kind == UnitKind::ship,
            "WHE-04: installed capital hero retains its existing standalone body");
        bool concentrate_found = false;
        for (const auto& unit : hero_loaded.value().units) for (const auto& special : unit.special_abilities) {
            if (special.kind != eawr::sim::tactical::SpecialAbilityKind::concentrate_fire) continue;
            concentrate_found = true;
            expect(special.target_speed_decrease.raw() == 0,
                "WHE-25: installed FoC retains its strict zero target-speed contribution");
        }
        expect(concentrate_found, "WHE-25: installed hero closure qualifies the concentrate-fire handler");
        const auto hero_motion = eawr::units::motion_table(hero_loaded.value());
        expect(static_cast<bool>(hero_motion), "WHE-SQ-02: hero locomotors bind");
        for (const auto* name : {"Slave_I", "Millennium_Falcon", "HoundsTooth", "IG-2000"}) {
            const auto* fighter = hero_loaded.value().find(name);
            const auto id = eawr::assets::object_type_crc(name);
            expect(fighter && fighter->kind == UnitKind::craft && hero_motion
                && hero_motion.value().find(id) == nullptr && hero_motion.value().squadrons.find_craft(id)
                && hero_motion.value().squadrons.find_squadron(id),
                std::string("WHE-SQ-02: hero uses fighter flight and self-represented group: ") + name);
        }
    }
    for (const auto* name : {"Darth_Vader_TIE_Fighter_Squadron", "Rogue_Squadron_Space", "TIE_Fighter_Squadron"}) {
        const auto* team = hero_loaded ? hero_loaded.value().find(name) : nullptr;
        // The normal TIE squadron is in the pinned tables, independent of the hero closure.
        if (team == nullptr) team = tables.find(name);
        expect(team && team->homogeneous, std::string("WHE-SQ-01: one authored/default team card: ") + name);
        if (team && std::string_view{name} == "Darth_Vader_TIE_Fighter_Squadron") {
            expect(team->members.size() == 7 && team->members.front().craft == "TIE_Prototype"
                && std::all_of(team->members.begin() + 1, team->members.end(),
                    [](const auto& member) { return member.craft == "Escort_TIE_Fighter"; }),
                "WHE-SQ-01: Vader has one ordered leader and six escorts under one authored team");
        }
        if (team && std::string_view{name} == "Rogue_Squadron_Space")
            expect(team->members.size() == 6, "WHE-SQ-01: Rogue retains all six authored craft");
    }
    // EN-07/08: production and station ion weapons retain their authored projectile flags.
    const auto ion_combat = eawr::units::combat_table(tables);
    expect(std::none_of(tables.notes.begin(), tables.notes.end(), [](const auto& note) {
        return note.reason.find("WAB-36: targeted ability row skipped") != std::string::npos;
    }), "WAB-36: installed FoC keeps strict named-handler resolution without skipped rows");
    expect(static_cast<bool>(ion_combat), "ion production combat table loads");
    if (ion_combat) {
        // Keep the installed FoC corpus under the stricter stock-frame audit even though
        // W-07 admits mod-authored nonorthogonal fire frames at runtime.
        for (const auto& profile : ion_combat.value().profiles) {
            for (const auto& weapon : profile.weapons) {
                if (!weapon.fire_axes) continue;
                const auto& axes = *weapon.fire_axes;
                for (std::size_t left = 0; left < axes.size(); ++left) {
                    for (std::size_t right = left; right < axes.size(); ++right) {
                        const auto product = eawr::sim::math::dot(axes[left], axes[right]);
                        const auto expected = left == right ? Fixed::scale : 0;
                        expect(product && std::llabs(product.value().raw() - expected)
                                <= eawr::sim::tactical::frame_tolerance_raw,
                            "W-07: installed FoC fire frames still pass strict orthogonality validation");
                    }
                }
            }
        }
        for (const auto* name : {"Nebulon_B_Frigate", "Corellian_Corvette", "Skirmish_Empire_Star_Base_1", "TIE_Fighter_Squadron"}) {
            const auto* type = tables.find(name);
            expect(type && type->unit_ai && std::binary_search(ion_combat.value().unit_ai_types.begin(),
                ion_combat.value().unit_ai_types.end(), eawr::assets::object_type_crc(name)),
                std::string("WTA-29: installed UNIT_AI admission includes ship, station and runtime team: ") + name);
        }
        const auto team_id = eawr::assets::object_type_crc("TIE_Fighter_Squadron");
        const auto& companies = ion_combat.value().retaliation_team_types;
        const auto company = std::find_if(companies.begin(), companies.end(),
            [team_id](const auto& row) { return row.type_id == team_id; });
        const auto* purchase = tables.find("TIE_Fighter_Squadron");
        expect(purchase && company != companies.end() && company->category_bits == purchase->category_bits
            && !ion_combat.value().find(team_id)
            && ion_combat.value().find_target_type(team_id) == &*company,
            "WTA-29: company category metadata is bound without an active team combat profile");
        for (const auto& priorities : ion_combat.value().priority_sets) {
            expect(std::any_of(priorities.rows.begin(), priorities.rows.end(),
                [team_id](const auto& row) { return row.type_id == team_id; }),
                "WTA-29: normalized company attackers have resolved priority rows");
        }
        auto changed = tables;
        const auto admitted = std::find_if(changed.units.begin(), changed.units.end(), [](const auto& unit) { return unit.unit_ai; });
        if (admitted != changed.units.end()) {
            admitted->unit_ai = false;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
                "WTA-29: unit-AI behavior contributes to bound content identity");
        }
        for (const auto* faction : {"Rebel", "Empire"}) for (unsigned level = 3; level <= 5; ++level) {
            const auto name = std::string("Skirmish_") + faction + "_Star_Base_" + std::to_string(level);
            const auto* station = ion_combat.value().find(eawr::assets::object_type_crc(name));
            std::size_t count = 0;
            if (station != nullptr) for (const auto& weapon : station->weapons) {
                if (!weapon.shot || !weapon.shot->energy_damage) continue;
                ++count;
                expect(!weapon.shot->hitpoint_damage && !weapon.shot->ion_stun && !weapon.shot->disable_engines_frames
                    && weapon.shot->damage.raw() == raw(20), "EN-07: station ion drains without hull damage/stun: " + name);
            }
            expect(count == level - 2, "station ion count grows from one to three: " + name);
        }
        for (const auto* name : {"B-Wing", "TIE_Defender"}) {
            const auto* craft = ion_combat.value().find(eawr::assets::object_type_crc(name));
            std::size_t count = 0;
            if (craft != nullptr) for (const auto& weapon : craft->weapons) {
                if (!weapon.shot || !weapon.shot->energy_damage) continue;
                ++count;
                expect(weapon.shot->disable_engines_frames == 600U && !weapon.shot->hitpoint_damage && !weapon.shot->ion_stun,
                    std::string("EN-08: stock small ion carries a 20-second engine disable: ") + name);
                if (weapon.ability_shot) expect(!weapon.ability_shot->disable_engines_frames && weapon.ability_shot->ion_stun,
                    "EN-08: targeted override does not inherit ordinary small-ion engine disable");
            }
            expect(count == 2, std::string("two ordinary small-ion weapons: ") + name);
        }
        auto changed_duration = tables;
        for (auto& projectile : changed_duration.projectiles) {
            if (projectile.disables_engines_when_power_drained) projectile.disable_engines_duration = Fixed::from_raw(raw(21));
        }
        expect(eawr::units::content_identity(changed_duration) != eawr::units::content_identity(tables),
            "EN-08: engine-disable duration contributes to content identity");
    }

    const std::vector<std::string> ids{"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1", "Corellian_Corvette",
        "Nebulon_B_Frigate", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Calamari_Cruiser", "Rebel_X-Wing_Squadron",
        "Y-Wing_Squadron",
        "TIE_Interceptor_Squadron", "TIE_Fighter_Squadron", "TIE_Bomber_Squadron", "X-Wing", "Y-Wing", "TIE_Interceptor",
        "TIE_Fighter", "TIE_Bomber"};
    expect(tables.units.size() > ids.size(), "pinned fleet plus five craft and live capture/construction types");
    for (const auto name : {"Defense_Satellite_Laser_Pad", "Mineral_Extractor_Pad", "Skirmish_Merchant_Dock"}) {
        const auto* point = tables.find(name);
        expect(point != nullptr && point->capture_point && point->capture_radius && point->capture_seconds,
            "WBP-01/02: map capture type has live capture data");
        expect(point != nullptr && !point->locomotion, "WSU-21: live pads and the merchant lack locomotion");
    }
    for (const auto name : {"UC_Empire_Mineral_Extractor", "UC_Rebel_Mineral_Extractor", "UC_Underworld_Mineral_Extractor"}) {
        const auto* child = tables.find(name);
        expect(child != nullptr && child->under_construction && tables.find(child->constructed_type) != nullptr,
            "WBP-06/18: pad menus load UC and completed types");
        if (child != nullptr) {
            const auto* completed = tables.find(child->constructed_type);
            expect(!child->locomotion && completed != nullptr && !completed->locomotion,
                "WSU-21: UC and completed pad structures lack locomotion");
        }
    }
    for (std::size_t index = 0; index < ids.size() && index < tables.units.size(); ++index) {
        expect(tables.units[index].id == ids[index], "unit order: " + ids[index]);
    }
    for (const auto& type : tables.units) {
        if (type.id.find("Defense_Satellite_") == std::string::npos || type.id.find("Pad") != std::string::npos) continue;
        expect(type.tactical_sale != type.under_construction,
            "WBP-30 only completed satellite types author tactical-sale behavior");
        if (type.tactical_sale) expect(!type.tactical_sell_percentage,
            "WBP-31 stock satellites use the verified half-cost sale default");
    }
    const std::span fleet(tables.units.data(), std::min(ids.size(), tables.units.size()));
    const auto unit = [&](const std::string_view id) { return tables.find(id); };
    const auto power = [&](const std::string_view id) -> std::int64_t {
        const auto* found = unit(id);
        if (found == nullptr) return 0;
        if (found->kind != UnitKind::squadron) return found->ai_combat_power ? found->ai_combat_power->raw() / Fixed::scale : 0;
        std::int64_t sum = 0;
        for (const auto& member : found->members) {
            const auto* craft = unit(member.craft);
            if (craft != nullptr && craft->ai_combat_power) sum += craft->ai_combat_power->raw() / Fixed::scale;
        }
        return sum;
    };
    expect(power("Skirmish_Rebel_Star_Base_1") + 2 * power("Rebel_X-Wing_Squadron") + power("Y-Wing_Squadron") +
                   power("Corellian_Corvette") + power("Nebulon_B_Frigate") + power("Calamari_Cruiser") == 13525,
           "SK-24 Rebel tick-zero power");
    expect(power("Skirmish_Empire_Star_Base_1") + 2 * power("TIE_Interceptor_Squadron") + power("Tartan_Patrol_Cruiser") +
                   power("Acclamator_Assault_Ship") == 9590,
           "SK-24 Empire tick-zero power");

    const std::map<std::string, std::pair<std::size_t, std::size_t>> hardpoints{
        {"Corellian_Corvette", {8, 0}}, {"Nebulon_B_Frigate", {5, 5}}, {"Tartan_Patrol_Cruiser", {5, 0}},
        {"Acclamator_Assault_Ship", {8, 8}}, {"Calamari_Cruiser", {7, 7}}, {"Skirmish_Rebel_Star_Base_1", {7, 7}}, {"Skirmish_Empire_Star_Base_1", {7, 7}}};
    for (const auto& [id, counts] : hardpoints) {
        const auto* found = unit(id);
        if (found == nullptr) {
            expect(false, "hardpoint owner loads: " + id);
            continue;
        }
        std::size_t targetable = 0;
        for (const auto& hardpoint : found->hardpoints) {
            targetable += hardpoint.targetable ? 1U : 0U;
            if (hardpoint.targetable) expect(hardpoint.attachment.position.has_value(), "targetable hardpoint has a position: " + hardpoint.id);
        }
        expect(found->hardpoints.size() == counts.first && targetable == counts.second, "hardpoint counts: " + id);
    }
    const std::map<std::string, std::size_t> members{{"Rebel_X-Wing_Squadron", 5}, {"Y-Wing_Squadron", 3},
        {"TIE_Interceptor_Squadron", 7}, {"TIE_Fighter_Squadron", 7}, {"TIE_Bomber_Squadron", 4}};
    for (const auto& [id, count] : members) {
        const auto* found = unit(id);
        expect(found != nullptr && found->members.size() == count, "squadron composition: " + id);
        expect(found && found->team_hull == std::optional{Fixed::from_raw(raw(100))}
            && !found->team_shield_points && !found->hull,
            "WSQ-60: every M2 container has its own default hull and no authored shield: " + id);
    }
    const auto* nebulon = unit("Nebulon_B_Frigate");
    expect(nebulon != nullptr && nebulon->lua_script == "ObjectScript_PowerToShields", "SK-47 Lua_Script reference");
    expect(nebulon != nullptr && nebulon->abilities.size() == 1 && nebulon->abilities[0].type == "DEFEND" &&
               nebulon->abilities[0].supports_autofire,
           "Nebulon-B DEFEND supports autofire");
    // SK-22 (owner, Q1 amended 2026-09-28): the Rebel MC80. SK-47's script and DEFEND autofire, the
    // 8500 hull and 2000 shield, and no hangar (SK-23).
    const auto* mc80 = unit("Calamari_Cruiser");
    expect(mc80 != nullptr && mc80->lua_script == "ObjectScript_PowerToShields" && !mc80->spawner &&
               mc80->abilities.size() == 1 && mc80->abilities[0].type == "DEFEND" && mc80->abilities[0].supports_autofire,
           "SK-22 MC80: SK-47 script, DEFEND autofire, no hangar");
    if (mc80 != nullptr) {
        std::size_t ion = 0;
        std::size_t laser = 0;
        std::size_t engines = 0;
        for (const auto& hardpoint : mc80->hardpoints) {
            ion += hardpoint.type == eawr::units::HardpointType::weapon_ion_cannon ? 1U : 0U;
            laser += hardpoint.type == eawr::units::HardpointType::weapon_laser ? 1U : 0U;
            engines += hardpoint.type == eawr::units::HardpointType::engine ? 1U : 0U;
        }
        expect(ion == 2 && laser == 4 && engines == 1, "SK-22 MC80: 2 ion cannons, 4 lasers and the engines");
    }
    const auto* acclamator = unit("Acclamator_Assault_Ship");
    expect(acclamator != nullptr && acclamator->spawner && acclamator->spawner->starting.size() == 2 &&
               acclamator->spawner->delay_seconds && acclamator->spawner->delay_seconds->raw() == raw(5) &&
               acclamator->spawner->reserves.size() == 1 && !acclamator->spawner->reserves_used,
           "SK-23 Acclamator hangar; SK-36 reserves unused");
    expect(acclamator != nullptr && acclamator->abilities.size() == 1 && acclamator->abilities[0].type == "POWER_TO_WEAPONS" &&
               acclamator->abilities[0].authored_type == "power_to_weapons",
           "lower-case ability type");
    for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
        const auto* station = unit(id);
        expect(station != nullptr && station->spawner && station->spawner->starting.size() == 2 &&
                   station->spawner->starting[0].count == 2 && station->spawner->delay_seconds &&
                   station->spawner->delay_seconds->raw() == raw(10),
               std::string("SK-23 station launches: ") + id);
    }
    // #68 authored ranges (Space_FOW_Reveal_Range); Skirmish_Rebel_Star_Base_1 inherits its
    // 2000 from Rebel_Star_Base_1, Corellian_Corvette authors 1200 then 1000 (the last
    // wins, as for every single-value tag).
    const std::map<std::string, std::int64_t> authored{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"X-Wing", 500}, {"TIE_Fighter", 500},
        {"TIE_Interceptor", 500}, {"Y-Wing", 600}, {"TIE_Bomber", 600}};
    // #271 sensors (V-01, V-03): REVEAL types with their own range; squadrons through their
    // team container (`Team` 800; Y-Wing_Squadron names Y_Wing_Squadron_Container, a variant
    // of Darth_Vader_TIE_Fighter_Container, 800 then 1000). Of the craft only the Y-Wing
    // authors REVEAL.
    const std::map<std::string, std::int64_t> sensors{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"Y-Wing", 600},
        {"Rebel_X-Wing_Squadron", 800}, {"TIE_Interceptor_Squadron", 800}, {"TIE_Fighter_Squadron", 800},
        {"TIE_Bomber_Squadron", 800}, {"Y-Wing_Squadron", 1000}};
    for (const auto& item : fleet) {
        const auto expected = authored.find(item.id);
        expect(item.selectable && !item.decoration, "WSU-15: the FoC fleet is selectable and not decoration: " + item.id);
        expect(item.locomotion == (item.kind != UnitKind::station), "WSU-21: stations lack locomotion; ships, craft and teams have it: " + item.id);
        if (expected == authored.end()) {
            expect(item.kind == UnitKind::squadron && !item.fow_reveal_range, "squadron authors no range: " + item.id);
        } else {
            expect(item.fow_reveal_range && item.fow_reveal_range->raw() == raw(expected->second),
                   "authored range: " + item.id);
        }
        const auto sensor = sensors.find(item.id);
        const auto range = eawr::units::sensor_range(item);
        expect(sensor == sensors.end() ? !range : range && range->raw() == raw(sensor->second), "sensor range: " + item.id);
    }
    // #665 (walk WSU-12): every M2 craft sets Mouse_Collide_Override_Sphere_Radius 50; ships and
    // squadron containers set none and are picked by their collision meshes.
    const std::set<std::string> pick_spheres{"X-Wing", "Y-Wing", "TIE_Fighter", "TIE_Interceptor", "TIE_Bomber"};
    for (const auto& item : fleet) {
        const bool sphere = pick_spheres.contains(item.id);
        expect(sphere ? item.mouse_collide_sphere_radius && item.mouse_collide_sphere_radius->raw() == raw(50)
                      : !item.mouse_collide_sphere_radius,
               "pick sphere (WSU-12): " + item.id);
    }
    std::size_t spawners = 0;
    for (const auto& item : fleet) spawners += item.spawner ? 1U : 0U;
    expect(spawners == 3, "no other roster ship spawns squadrons (SK-23)");
    expect(tables.priority_sets.size() >= 5, "Corvette, Frigate, Capital, Fighter and Bomber priority sets");
    // #270: the FoC space sets (SpaceUnitTargetingPriorities.xml) and R-09 on the fleet.
    for (const auto& set : tables.priority_sets) {
        bool categories = !set.attack_priorities.empty();
        for (const auto& entry : set.attack_priorities) {
            categories = categories && entry.match == eawr::units::PriorityMatch::category && entry.bits != 0;
        }
        expect(categories, "every FoC space entry is a category: " + set.id);
        expect(set.property_exclusions == std::vector<std::string>{"NotOpportunityTarget"} &&
                   set.property_exclusion_bits == 0x100 && set.category_exclusion_bits == 0 &&
                   set.unit_exclusions == std::vector<std::string>{"Destroyable_Asteroid_Small",
                       "Destroyable_Asteroid_Medium", "Destroyable_Asteroid_Large", "Destroyable_Asteroid_Huge"} &&
                   set.hard_point_priorities.empty() && set.hard_point_exclusions.empty(),
               "every FoC space set excludes NotOpportunityTarget and the destroyable asteroids: " + set.id);
    }
    const auto set_of = [&](const std::string_view id) -> const eawr::units::TargetingPrioritySet* {
        for (const auto& set : tables.priority_sets) {
            if (set.id == id) return &set;
        }
        return nullptr;
    };
    const auto* fighter_set = set_of("Fighter");
    const auto* x_wing = unit("X-Wing");
    const auto* y_wing = unit("Y-Wing");
    const auto* corellian = unit("Corellian_Corvette");
    const auto* frigate = unit("Nebulon_B_Frigate");
    expect(fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr,
           "the Fighter set and the ranked fleet types load");
    if (fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr) {
        const auto& entries = fighter_set->attack_priorities;
        expect(entries.size() == 3 && entries[0].name == "Transport" && entries[0].weight.raw() == raw(1) &&
                   entries[1].name == "Bomber" && entries[1].weight.raw() == raw(2) && entries[2].name == "Fighter" &&
                   entries[2].weight.raw() == raw(3),
               "FoC Fighter set: Transport 1, Bomber 2, Fighter 3");
        const auto y_score = eawr::units::attack_priority(*fighter_set, *y_wing);
        const auto x_score = eawr::units::attack_priority(*fighter_set, *x_wing);
        expect(y_score && x_score && *y_score < *x_score && x_score->raw() == raw(3),
               "S-01: the Fighter set ranks a Y-Wing (Bomber) above an X-Wing (Fighter)");
        expect(eawr::units::attack_priority(*fighter_set, *corellian) == eawr::units::unlisted_priority,
               "the Fighter set does not list Corvette: ranked last, not excluded");
        auto flagged = *frigate;
        flagged.property_bits |= 0x100;
        expect(!eawr::units::attack_priority(*fighter_set, flagged), "a NotOpportunityTarget type has no priority");
        auto asteroid = *frigate;
        asteroid.id = "Destroyable_Asteroid_Large";
        expect(!eawr::units::attack_priority(*fighter_set, asteroid), "a destroyable asteroid has no priority");
    }
    expect(tables.categories.size() == 30 && tables.properties.size() == 14, "FoC category and property enums");
    for (const auto& item : fleet) {
        // Squadrons are containers; only Rebel_X-Wing_Squadron authors a CategoryMask.
        expect((item.production.upgrade_object || item.kind == UnitKind::squadron || item.category_bits != 0) && (item.property_bits & 0x100) == 0,
               "fleet type has categories and is an opportunity target: " + item.id);
    }
    expect(tables.constants.scalars.size() == 55, "combat, hazard and multiplayer tech scalars");
    for (const auto& [tag, expected] : {
            std::pair{std::string_view{"Asteroid_Field_Damage"}, Fixed::from_raw(raw(20))},
            std::pair{std::string_view{"Asteroid_Field_Damage_Rate"}, Fixed::from_decimal("0.20").value()}}) {
        const auto constant = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
            [&](const auto& value) { return value.tag == tag; });
        expect(constant != tables.constants.scalars.end() && constant->value == expected,
            "WHZ-11/12: stock asteroid damage and per-frame probability are retained");
    }
    // #70: the five roster ships move; Max_Speed, OverrideAcceleration/Deceleration and
    // Max_Rate_Of_Turn x 1.2 (docs/behaviour/space-movement.md MV-01).
    const auto motion = eawr::units::motion_table(tables);
    expect(motion && motion.value().profiles.size() >= 5 && motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "FoC motion table: five ships, 15 degree arcs, 300 unit expansion distance");
    // OR-06, OR-14: the stock gameconstants.xml authors 10 frames and 750 units.
    expect(motion && motion.value().rules.reevaluation_frames == 10
               && motion.value().rules.guard_range == Fixed::from_raw(raw(750)) && motion.value().rules.max_waypoints == 6,
           "FoC motion rules: MovementReevaluationFrameCount 10, Space_Guard_Range 750");
    // #599 (space-fighters FO-11): the group move's lane steer reads FormationMinimumSideError
    // and FormationMaximumSideError.
    expect(motion && motion.value().squadrons.side_error_max == Fixed::from_raw(raw(30))
               && motion.value().squadrons.side_error_min.raw() > 0
               && motion.value().squadrons.side_error_min < Fixed::from_raw(raw(1)),
           "FoC squadron table: side errors 0.1 and 30");
    if (motion) {
        const auto* corvette = motion.value().find(eawr::assets::object_type_crc("Corellian_Corvette"));
        const auto* assault = motion.value().find(eawr::assets::object_type_crc("Acclamator_Assault_Ship"));
        const auto near = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -2 && difference <= 2;
        };
        expect(corvette != nullptr && near(corvette->max_speed, "3.72") && near(corvette->acceleration, "0.06") &&
                   near(corvette->deceleration, "0.06") && corvette->rate_of_turn == Fixed::from_decimal("1.5").value() &&
                   corvette->turn_in_place_slowdown == Fixed::from_raw(raw(2)),
               "Corellian corvette: 3.72 per frame, 0.06 per frame squared, 1.5 degrees per frame, slowdown 2");
        expect(assault != nullptr && near(assault->max_speed, "2.64") && near(assault->rate_of_turn, "0.6") &&
                   assault->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
               "Acclamator: 2.64 per frame, 0.6 degrees per frame, frigate slowdown 3");
        // #351 BK-01: Max_Rate_Of_Roll 0.2 x 1.2 for all four; Bank_Turn_Angle as authored.
        const auto* nebulon_motion = motion.value().find(eawr::assets::object_type_crc("Nebulon_B_Frigate"));
        const auto* tartan = motion.value().find(eawr::assets::object_type_crc("Tartan_Patrol_Cruiser"));
        const auto banks = [&near](const eawr::sim::tactical::MotionProfile* profile, const std::int64_t angle) {
            return profile != nullptr && near(profile->roll_rate, "0.24") && profile->bank_angle == Fixed::from_raw(raw(angle));
        };
        expect(banks(corvette, 15) && banks(tartan, 15) && banks(nebulon_motion, 5) && banks(assault, 20),
               "banking: roll rate 0.24 per frame; bank 15 (corvette, Tartan), 5 (Nebulon-B), 20 (Acclamator)");
        // #447 SP-01: the M2 craft spin away on death, rebel 0.2 and imperial 0.4, for 2 s.
        const auto spins = [&](const char* id, const char* chance) {
            const auto* craft = motion.value().squadrons.find_craft(eawr::assets::object_type_crc(id));
            return craft != nullptr && craft->spin_away && near(craft->spin_away->chance, chance)
                && craft->spin_away->time == Fixed::from_raw(raw(2));
        };
        expect(spins("X-Wing", "0.2") && spins("Y-Wing", "0.2") && spins("TIE_Fighter", "0.4")
                   && spins("TIE_Interceptor", "0.4") && spins("TIE_Bomber", "0.4"),
               "spin-away: X-wing and Y-wing 0.2, TIE fighter, interceptor and bomber 0.4, all 2 s");
        // #71 footprints (docs/behaviour/space-movement.md AV-05, docs/unit-data.md).
        using eawr::sim::tactical::SpaceLayer;
        const auto close = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -(std::int64_t{1} << 14) && difference <= (std::int64_t{1} << 14);
        };
        const auto print = [&](const char* id) {
            const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc(id));
            if (footprint == nullptr) {
                std::cout << "footprint " << id << ": none" << std::endl;
                return footprint;
            }
            std::cout << "footprint " << id << ": layer " << static_cast<int>(footprint->layer) << " x "
                      << footprint->x_extent.raw() << " y " << footprint->y_extent.raw() << " radius "
                      << footprint->radius.raw() << (footprint->obstacle ? " obstacle" : "") << std::endl;
            return footprint;
        };
        const auto* corvette_print = print("Corellian_Corvette");
        const auto* nebulon_print = print("Nebulon_B_Frigate");
        print("Tartan_Patrol_Cruiser");
        print("Acclamator_Assault_Ship");
        print("Skirmish_Rebel_Star_Base_1");
        print("Skirmish_Empire_Star_Base_1");
        const auto* pad = print("Mineral_Extractor_Pad");
        print("Defense_Satellite_Laser_Pad");
        print("N_Gravity_Well_Station");
        print("Skirmish_Merchant_Dock");
        const auto* container = print("Orbital_Resource_Container");
        expect(corvette_print != nullptr && corvette_print->layer == SpaceLayer::corvette
                   && close(corvette_print->x_extent, "17.755") && close(corvette_print->y_extent, "41.822")
                   && corvette_print->radius == corvette_print->y_extent,
               "corvette footprint: the RV_CORVETTE.ALO collision box x 0.5");
        expect(nebulon_print != nullptr && nebulon_print->layer == SpaceLayer::frigate && close(nebulon_print->x_extent, "23.48")
                   && close(nebulon_print->y_extent, "109.949"),
               "Nebulon-B footprint: the RV_NEBULONB.ALO collision box x 0.7");
        expect(pad != nullptr && pad->layer == SpaceLayer::static_object && pad->obstacle
                   && pad->radius == Fixed::from_decimal("206.25").value(),
               "the mining pad is a static obstacle of 275 x 0.75");
        expect(container == nullptr, "a type without Space_Layer is not tracked");
        // #372 review: validate_motion rejects a radius whose outer destination search ring holds
        // more than 2^13 points; the original M2 fleet's corvette puts 245 there.
        const eawr::sim::tactical::Footprint* smallest = nullptr;
        const eawr::sim::tactical::Footprint* fixture_smallest = nullptr;
        for (const auto& footprint : motion.value().footprints) {
            if (footprint.radius.raw() > 0 && (smallest == nullptr || footprint.radius < smallest->radius)) smallest = &footprint;
            const bool fixture_type = std::any_of(fleet.begin(), fleet.end(), [&](const auto& type) {
                return eawr::assets::object_type_crc(type.id) == footprint.type_id;
            });
            if (fixture_type && footprint.radius.raw() > 0
                && (fixture_smallest == nullptr || footprint.radius < fixture_smallest->radius)) fixture_smallest = &footprint;
        }
        if (smallest != nullptr) {
            std::string name;
            for (const auto& type : tables.units) {
                if (eawr::assets::object_type_crc(type.id) == smallest->type_id) name = type.id;
            }
            for (const auto& obstacle : tables.obstacles) {
                if (eawr::assets::object_type_crc(obstacle.id) == smallest->type_id) name = obstacle.id;
            }
            std::cout << "smallest soft radius: " << name << " radius " << smallest->radius.raw() << std::endl;
        }
        expect(fixture_smallest != nullptr && fixture_smallest == corvette_print,
               "the corvette has the original M2 fixture's smallest soft radius");
        expect(smallest != nullptr && smallest->type_id == eawr::assets::object_type_crc("Underworld_Pirate_IPV")
                   && close(smallest->radius, "35.81762"),
               "expanded production closure retains the pirate IPV's smaller soft radius");
        expect(motion.value().avoidance.has_value(), "FoC avoidance rules load");
    }

    // #76 abilities from FoC data (docs/behaviour/space-abilities.md AB-01 to AB-04): the four
    // modelled kinds on six types, squadron types and the cut HUNT left out; #561 adds the Y-wing
    // squadron's ION_CANNON_SHOT (AB-60) on the squadron and its craft. The MC80 (SK-22) carries the
    // Nebulon-B's DEFEND multipliers and script, with a 40 s recharge.
    const std::vector<eawr::sim::tactical::PlayerId> humans{1};
    const auto abilities = eawr::units::ability_table(tables, humans);
    expect(static_cast<bool>(abilities), "FoC ability table builds");
    if (!abilities) std::cerr << abilities.error().message << "\n";
    if (abilities) {
        using eawr::sim::tactical::AbilityKind;
        const auto& table = abilities.value();
        expect(table.profiles.size() >= 8 && table.humans == humans, "eight types carry a modelled ability");
        expect(table.autofire_defaults == humans, "AB-45: human profiles default to supported autofire");
        const auto manual = eawr::units::ability_table(tables, humans, nullptr, false);
        expect(manual && manual.value().autofire_defaults.empty(), "AB-45: the profile default can be disabled");
        const auto decimal = [](const char* text) { return Fixed::from_decimal(text).value(); };
        struct Expected {
            const char* id;
            AbilityKind kind;
            std::uint32_t expiration;
            std::uint32_t recharge;
            const char* weapon_delay;
            const char* shield_regen;
            const char* shield_interval;
            const char* energy_regen;
            const char* energy_interval;
            const char* speed;
            bool autofire;
            bool script;
        };
        const Expected rows[] = {
            {"Nebulon_B_Frigate", AbilityKind::defend, 450, 1800, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Calamari_Cruiser", AbilityKind::defend, 450, 1200, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Corellian_Corvette", AbilityKind::turbo, 600, 1500, "3", "0", "1", "1", "1", "2", false, false},
            {"Tartan_Patrol_Cruiser", AbilityKind::power_to_weapons, 210, 1800, "0.2", "-25", "1", "1", "1", "0.5", false, false},
            {"Acclamator_Assault_Ship", AbilityKind::power_to_weapons, 600, 1800, "0.5", "-3", "1", "1", "1", "0.5", false, false},
            {"X-Wing", AbilityKind::spoiler_lock, 0, 0, "3", "3", "1", "3", "1", "1.3", false, false},
        };
        for (const auto& row : rows) {
            const auto* found = table.find(eawr::assets::object_type_crc(row.id));
            const bool shaped = found != nullptr && found->abilities.size() == 1;
            expect(shaped, std::string("one modelled ability: ") + row.id);
            if (!shaped) continue;
            const auto& ability = found->abilities[0];
            const auto& m = ability.modifiers;
            expect(ability.kind == row.kind && ability.expiration_frames == row.expiration && ability.recharge_frames == row.recharge
                       && m.weapon_delay == decimal(row.weapon_delay) && m.shield_regen == decimal(row.shield_regen)
                       && m.shield_regen_interval == decimal(row.shield_interval) && m.energy_regen == decimal(row.energy_regen)
                       && m.energy_regen_interval == decimal(row.energy_interval) && m.speed == decimal(row.speed)
                       && ability.supports_autofire == row.autofire && found->defend_script == row.script,
                std::string("FoC ability values: ") + row.id);
        }
        // WHE-22/23: each craft retains its own inherited modes and timers.
        for (const char* id : {"Millennium_Falcon", "Admonitor_Star_Destroyer", "Wedge_XWing_Rogue",
                "Rogue_2_XWing", "Rogue_4_XWing", "Rogue_7_XWing", "Rogue_10_XWing", "Rogue_11_XWing"}) {
            const auto* hero = table.find(eawr::assets::object_type_crc(id));
            const auto kind = std::string_view(id) == "Millennium_Falcon" ? AbilityKind::invulnerability : AbilityKind::power_to_weapons;
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, kind).value_or(hero->abilities.size()) : 0;
            const bool bound = hero && slot < hero->abilities.size();
            expect(bound, std::string("WHE-22: loaded hero damage mode: ") + id);
            if (!bound) continue;
            const auto& mode = hero->abilities[slot];
            if (kind == AbilityKind::invulnerability) {
                expect(mode.expiration_frames == 180 && mode.recharge_frames == 1800 && mode.modifiers.take_damage == Fixed{},
                    "WHE-22: Falcon zero take damage, six seconds, sixty-second recharge");
            } else if (std::string_view(id) == "Admonitor_Star_Destroyer") {
                expect(mode.expiration_frames == 210 && mode.recharge_frames == 1500 && mode.modifiers.cause_damage == decimal("2"),
                    "WHE-22: Assault twice damage, seven seconds, fifty-second recharge");
            } else {
                expect(mode.expiration_frames == 300 && mode.recharge_frames == 1800 && mode.modifiers.cause_damage == decimal("2.5"),
                    "WHE-23: Rogue leader and inherited escorts use craft ten-second/sixty-second timers");
            }
        }
        for (const auto& [id, range, decrease, recharge] : {
                std::tuple{"Accuser_Star_Destroyer", 1100, "0.98", 600U},
                std::tuple{"Admonitor_Star_Destroyer", 800, "0.9", 750U}}) {
            const auto* hero = table.find(eawr::assets::object_type_crc(id));
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, AbilityKind::tractor_beam).value_or(hero->abilities.size()) : 0;
            const bool bound = hero && slot < hero->abilities.size();
            expect(bound, std::string("WHE-27: stock tractor is bound: ") + id);
            if (!bound) continue;
            const auto nested = std::find_if(hero->special.begin(), hero->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::tractor_beam;
            });
            expect(nested != hero->special.end() && nested->beam_max_range == Fixed::from_raw(raw(range))
                && nested->target_speed_decrease == decimal(decrease) && hero->abilities[slot].recharge_frames == recharge,
                "WHE-27/60: stock tractor range, reduction and recharge stay distinct");
        }
        const auto* accuser = table.find(eawr::assets::object_type_crc("Accuser_Star_Destroyer"));
        const auto energy = accuser ? eawr::sim::tactical::ability_slot(*accuser, AbilityKind::energy_weapon).value_or(accuser->abilities.size()) : 0;
        const bool energy_bound = accuser && energy < accuser->abilities.size();
        expect(energy_bound, "WHE-26: Accuser energy weapon is bound");
        if (energy_bound) {
            const auto nested = std::find_if(accuser->special.begin(), accuser->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::energy_weapon;
            });
            expect(nested != accuser->special.end() && nested->beam_max_range == Fixed::from_raw(raw(800))
                && nested->damage_per_frame == Fixed::from_raw(raw(50)) && accuser->abilities[energy].expiration_frames == 150
                && accuser->abilities[energy].recharge_frames == 3600 && nested->beam_owner_bone == "HP_trac_bone",
                "WHE-26/59: stock beam damage per frame, duration, recharge and owner attachment bind from XML");
        }
        const auto* slave = table.find(eawr::assets::object_type_crc("Slave_I"));
        auto ring_input = input;
        ring_input.types = {"MC30_Frigate", "TIE_Scout_Squadron"};
        ring_input.obstacles.clear();
        const auto ring_loaded = eawr::units::load_unit_tables(ring_input);
        expect(static_cast<bool>(ring_loaded), "WCB/WFP: stock MC30 and scout closure loads");
        if (ring_loaded) {
            const auto built = eawr::units::ability_table(ring_loaded.value(), humans);
            expect(static_cast<bool>(built), "WCB/WFP: stock ability profiles bind");
            if (built) {
                const auto* mc30 = built.value().find(eawr::assets::object_type_crc("MC30_Frigate"));
                const auto slot = mc30 ? eawr::sim::tactical::ability_slot(*mc30, AbilityKind::cluster_bomb) : std::nullopt;
                expect(slot && mc30->abilities[*slot].spawned, "WCB-01: MC30 has a cluster button");
                if (slot && mc30->abilities[*slot].spawned) {
                    const auto& ability = mc30->abilities[*slot];
                    const auto& spawn = *ability.spawned;
                    expect(ability.recharge_frames == 1500 && ability.supports_autofire && spawn.ring_count == 12
                        && spawn.countdown_frames == 45 && spawn.speed == Fixed::from_raw(raw(12))
                        && spawn.max_travel == Fixed::from_raw(raw(500)) && spawn.blast.damage == Fixed::from_raw(raw(150))
                        && spawn.blast.radius == Fixed::from_raw(raw(150)), "WCB-01: stock ring, flight, countdown and recharge bind");
                    expect(mc30->mc30_script, "FS-29: stock Lua_Script attaches the MC30 self-trigger");
                    const auto health = eawr::units::durability_table(ring_loaded.value());
                    const auto combat = eawr::units::combat_table(ring_loaded.value());
                    expect(health && combat, "WCB-02: stock bomb combat tables bind");
                    if (health && combat) {
                        eawr::sim::tactical::TacticalSetup initial;
                        initial.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                        initial.units = {{1, mc30->type_id, 1, {}, eawr::sim::math::identity_quat(), {}},
                            {2, mc30->type_id, 2, {Fixed::from_raw(raw(10000)), {}, {}}, eawr::sim::math::identity_quat(), {}}};
                        // FS-29/FD-47: no activation command; an AI MC30 consumes the preceding stock sample.
                        auto ai_initial = initial;
                        ai_initial.units[0].entity_id = 30;
                        ai_initial.units[0].owner = 2;
                        ai_initial.units[1].entity_id = 31;
                        ai_initial.units[1].owner = 1;
                        ai_initial.units[1].position = {Fixed::from_raw(raw(300)), {}, {}};
                        auto ai_session = eawr::sim::tactical::TacticalSession::create(ai_initial, {}, health.value(), {}, {}, combat.value(), {}, built.value());
                        expect(static_cast<bool>(ai_session), "FS-29: installed AI MC30 session creates");
                        if (ai_session) {
                            const eawr::platform::ThreadWorkerAdapter ai_executor(4);
                            for (unsigned frame = 0; frame <= 31; ++frame) {
                                const auto step = ai_session.value().step(ai_executor);
                                expect(static_cast<bool>(step), "FS-29: installed self-trigger step succeeds");
                                if (!step) break;
                                if (frame == 0 || frame == 30) expect(ai_session.value().projectiles().empty(),
                                    "FC-57: installed first empty update and equality skip do not fire");
                                if (frame == 31) expect(ai_session.value().projectiles().size() == 12
                                    && ai_session.value().ability_state(30)->slots[*slot].ready_tick == 1531,
                                    "FD-47: AI stock MC30 directly launches twelve bombs and starts recharge");
                            }
                        }
                        auto session = eawr::sim::tactical::TacticalSession::create(initial, {}, health.value(), {}, {}, combat.value(), {}, built.value());
                        expect(static_cast<bool>(session), "WCB-02: stock bomb session creates");
                        if (session) {
                            expect(static_cast<bool>(session.value().submit({{0, 1, 1}, {1},
                                eawr::sim::tactical::AbilityPayload{AbilityKind::cluster_bomb, eawr::sim::tactical::AbilityAction::activate}})),
                                "WCB-02: stock ring activates");
                            std::set<std::uint64_t> detonated;
                            const eawr::platform::ThreadWorkerAdapter executor(4);
                            for (unsigned tick = 0; tick < 48; ++tick) {
                                const auto step = session.value().step(executor);
                                expect(static_cast<bool>(step), "WCB-02: stock flight succeeds");
                                if (!step) break;
                                if (tick == 0) expect(session.value().projectiles().size() == 12, "WCB-02: twelve stock bombs launch");
                                for (const auto& event : step.value().snapshot->combat_events())
                                    if (event.kind == eawr::sim::tactical::CombatEventKind::projectile_expired && event.shooter == 1)
                                        detonated.insert(event.target);
                            }
                            expect(detonated.size() == 12 && session.value().projectiles().empty(),
                                "WCB-02: all twelve stock projectiles detonate, including travel expiry");
                        }
                    }
                }
                const auto* scout = built.value().find(eawr::assets::object_type_crc("TIE_Scout"));
                const auto ping = scout ? eawr::sim::tactical::ability_slot(*scout, AbilityKind::fow_reveal_ping) : std::nullopt;
                expect(ping && scout->abilities[*ping].spawned, "WFP-01: scout craft has the acting ping slot (AB-15)");
                if (ping && scout->abilities[*ping].spawned) {
                    const auto& ability = scout->abilities[*ping];
                    expect(ability.recharge_frames == 900 && ability.spawned->reveal_range == Fixed::from_raw(raw(1000))
                        && ability.spawned->speed.raw() == 0 && ability.spawned->max_travel.raw() == 0
                        && ability.spawned->dense_reveal_multiplier == Fixed::from_raw(raw(1)), "WFP-01: projectile radius replaces decal distance");
                    expect(!ability.spawned->lifetime, "WFP-03: stock ping has no injected or authored lifetime");
                    const auto health = eawr::units::durability_table(ring_loaded.value());
                    const auto combat = eawr::units::combat_table(ring_loaded.value());
                    expect(health && combat, "WFP-03: stock ping combat tables bind");
                    if (health && combat) {
                        namespace tactical = eawr::sim::tactical;
                        const eawr::sim::math::Vec3 click{Fixed::from_raw(raw(3000)), {}, {}};
                        tactical::TacticalSetup initial;
                        initial.players = {{1, 1, 1, tactical::player_flag_commandable},
                            {2, 2, 2, tactical::player_flag_commandable}};
                        initial.units = {{1, scout->type_id, 1, {}, eawr::sim::math::identity_quat(), {}}};
                        tactical::FogRules fog;
                        fog.map_left = Fixed::from_raw(raw(-2000)); fog.map_top = Fixed::from_raw(raw(2000));
                        fog.cell_size = Fixed::from_raw(raw(100)); fog.cells_wide = 80; fog.cells_tall = 40;
                        // Fast ordinary regrowth isolates the stock object's lifetime from FC-15 cadence.
                        fog.service_period = 1; fog.ramp_down_step = 238;
                        std::vector<std::string> reference;
                        for (const auto workers : {1U, 2U, 4U, 8U}) {
                            auto session = tactical::TacticalSession::create(initial, {}, health.value(), {}, fog,
                                combat.value(), {}, built.value());
                            expect(static_cast<bool>(session), "WFP-03: loaded stock ping session creates");
                            if (!session) continue;
                            expect(static_cast<bool>(session.value().submit({{0, 1, 1}, {1},
                                tactical::AbilityPayload{AbilityKind::fow_reveal_ping, tactical::AbilityAction::activate,
                                    eawr::sim::invalid_entity_id, tactical::no_hardpoint, click}})), "WFP-03: loaded stock ping activates");
                            const eawr::platform::ThreadWorkerAdapter executor(workers);
                            std::vector<std::string> hashes;
                            for (unsigned tick = 0; tick < 200; ++tick) {
                                const auto step = session.value().step(executor);
                                expect(static_cast<bool>(step), "WFP-03: loaded stock ping steps");
                                if (!step) break;
                                hashes.push_back(step.value().state_sha256);
                                if (tick == 0) expect(session.value().projectiles().size() == 1
                                    && session.value().fog_cells()->revealed(0, click)
                                    && !session.value().fog_cells()->revealed(1, click),
                                    "WFP-01/03: stock spawn reveals only for owner");
                                if (tick >= 1) expect(session.value().projectiles().empty(),
                                    "WFP-03: stock ping ends on first flight and never returns");
                                if (tick >= 8) expect(!session.value().fog_cells()->revealed(0, click),
                                    "WFP-03: ordinary fog regrows after stock ping expires");
                            }
                            if (reference.empty()) reference = hashes;
                            else expect(hashes == reference, "WFP-03: loaded stock expiry equals on 1/2/4/8 workers");
                        }
                    }
                }
            }
        }
        const auto bomb = slave ? eawr::sim::tactical::ability_slot(*slave, AbilityKind::harmonic_bomb) : std::nullopt;
        expect(bomb && slave->abilities[*bomb].spawned && slave->abilities[*bomb].spawned->countdown_frames == 45
            && slave->abilities[*bomb].recharge_frames == 600, "stock harmonic bomb binds the authored countdown and recharge");
        // WAB-70..74: the StarViper squadron's team container holds BUZZ_DROIDS with the nested
        // data of its team type and the Buzz_Droids object's own speed and valid targets. The pinned
        // M2 roster has no Underworld units, so the squadron loads on its own.
        auto viper_input = input;
        viper_input.types = {"StarViper_Squadron"};
        viper_input.obstacles.clear();
        const auto viper_loaded = eawr::units::load_unit_tables(viper_input);
        expect(static_cast<bool>(viper_loaded), "WAB-70: the StarViper squadron's tables load");
        std::optional<eawr::sim::tactical::AbilityTable> viper_abilities;
        if (viper_loaded) {
            if (auto built = eawr::units::ability_table(viper_loaded.value(), humans)) viper_abilities = std::move(built).value();
        }
        expect(viper_abilities.has_value(), "WAB-70: the StarViper squadron's ability table builds");
        const auto* viper = viper_abilities ? viper_abilities->find(eawr::assets::object_type_crc("StarViper_Squadron")) : nullptr;
        const auto buzz = viper ? eawr::sim::tactical::ability_slot(*viper, AbilityKind::buzz_droids) : std::nullopt;
        expect(buzz.has_value(), "WAB-70: the StarViper squadron's team holds buzz droids");
        if (buzz) {
            const auto& ability = viper->abilities[*buzz];
            const auto per_frame = [](const std::int64_t per_second) {
                return Fixed::from_raw((raw(per_second) + 15) / 30);
            };
            expect(ability.team && ability.recharge_frames == 1800 && ability.buzz
                && ability.buzz->droid_type == eawr::assets::object_type_crc("Buzz_Droids")
                && ability.buzz->min_range == Fixed::from_raw(raw(5)) && ability.buzz->max_range == Fixed::from_raw(raw(400))
                && ability.buzz->chase_radius == Fixed::from_raw(raw(300)) && ability.buzz->damage_radius == Fixed::from_raw(raw(175))
                && ability.buzz->enemy_damage == per_frame(23) && ability.buzz->own_damage == per_frame(3)
                && ability.buzz->activation_frames == 30 && ability.buzz->victim_categories != 0
                && ability.buzz->speed.raw() - decimal("4.2").raw() <= 2 && decimal("4.2").raw() - ability.buzz->speed.raw() <= 2,
                "WAB-70..74: stock buzz-droid reach, radii, damage, arming, speed and recharge bind from XML");
        }
        const auto* viper_craft = viper_abilities ? viper_abilities->find(eawr::assets::object_type_crc("Starviper_Fighter")) : nullptr;
        expect(viper_craft == nullptr || !eawr::sim::tactical::ability_slot(*viper_craft, AbilityKind::buzz_droids),
            "WAB-70: the StarViper craft carry no buzz-droid slot of their own (AB-60)");
        const auto* antilles = table.find(eawr::assets::object_type_crc("Sundered_Heart"));
        const auto weak = antilles ? eawr::sim::tactical::ability_slot(*antilles, AbilityKind::weaken_enemy) : std::nullopt;
        expect(weak && antilles->abilities[*weak].spawned && antilles->abilities[*weak].spawned->weaken.on_detonation
            && antilles->abilities[*weak].spawned->weaken.duration_frames == 450
            && antilles->abilities[*weak].spawned->weaken.radius == Fixed::from_raw(raw(300))
            && antilles->abilities[*weak].spawned->weaken.cause_damage_reduction == Fixed::from_decimal("0.2").value(),
            "stock weaken projectile binds radius, timed damage reduction and detonation gate");
        const auto* vader = table.find(eawr::assets::object_type_crc("TIE_Prototype"));
        const auto refill = vader ? eawr::sim::tactical::ability_slot(*vader, AbilityKind::replenish_wingmen) : std::nullopt;
        expect(refill && vader->abilities[*refill].replenish_team == eawr::assets::object_type_crc("Darth_Vader_TIE_Fighter_Squadron")
            && vader->abilities[*refill].recharge_frames == 1800 && !vader->abilities[*refill].replenish_particle.empty(),
            "WHE-63 Vader uses authored creation team, sixty-second recharge and particle");
        // AB-60: the team ability from Y_Wing_Squadron_Container (Recharge_Seconds 20, autofire),
        // and each craft's own slot of the kind.
        const auto* team = table.find(eawr::assets::object_type_crc("Y-Wing_Squadron"));
        expect(team != nullptr && team->abilities.size() == 1 && team->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && team->abilities[0].team && team->abilities[0].recharge_frames == 600
                   && team->abilities[0].supports_autofire,
            "AB-60: the Y-wing squadron holds the team ION_CANNON_SHOT");
        const auto* craft = table.find(eawr::assets::object_type_crc("Y-Wing"));
        expect(craft != nullptr && craft->abilities.size() == 1 && craft->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && !craft->abilities[0].team,
            "AB-60: each Y-wing has its own ION_CANNON_SHOT slot");
        const auto* hunter = table.find(eawr::assets::object_type_crc("TIE_Fighter"));
        expect(hunter != nullptr && hunter->abilities.size() == 1
                   && hunter->abilities[0].kind == AbilityKind::hunt && !hunter->abilities[0].team
                   && hunter->abilities[0].expiration_frames == 0 && hunter->abilities[0].recharge_frames == 0
                   && !hunter->abilities[0].supports_autofire
                   && hunter->hunt_reveal_range == Fixed::from_raw(raw(500)),
            "WAB-50/54: each stock TIE Fighter binds untimed Hunt and its authored reveal range");
        for (const char* id : {"Rebel_X-Wing_Squadron", "TIE_Fighter_Squadron"}) {
            expect(table.find(eawr::assets::object_type_crc(id)) == nullptr, std::string("no modelled ability profile: ") + id);
        }
    }

    // #72 durability from FoC data (docs/behaviour/space-hardpoints.md HD-01 to HD-05).
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "FoC durability table builds");
    if (durability) {
        using eawr::sim::tactical::HardpointRole;
        const auto& table = durability.value();
        expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
                   table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
                   table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
               "FoC durability rules 0.2, 0.4 and 0.33");
        expect(table.profiles.size() > 12, "original fleet hulls plus capture points, UC and completed structures");
        const auto profile = [&](const std::string_view id) { return table.find(eawr::assets::object_type_crc(id)); };
        for (const auto& [id, count] : members) {
            static_cast<void>(count);
            expect(profile(id) == nullptr, "WSQ-60: craft alone carry combat durability: " + id);
        }
        const auto roles = [](const eawr::sim::tactical::DurabilityProfile& item) {
            std::string text;
            for (const auto& hardpoint : item.hardpoints) {
                text += std::string(eawr::sim::tactical::to_string(hardpoint.role)) + (hardpoint.destroyable ? "+" : "-") +
                        std::to_string(hardpoint.max_health.raw() / Fixed::scale) + " ";
            }
            return text;
        };
        const auto* nebulon_profile = profile("Nebulon_B_Frigate");
        expect(nebulon_profile != nullptr && nebulon_profile->max_hull.raw() == raw(5400) &&
                   roles(*nebulon_profile) == "weapon+390 weapon+390 weapon+390 weapon+390 engine+390 ",
               "Nebulon-B: 3600 x 1.5 hull, 4 lasers and the engines at 260 x 1.5");
        const auto* acclamator_profile = profile("Acclamator_Assault_Ship");
        expect(acclamator_profile != nullptr && acclamator_profile->max_hull.raw() == raw(3000) &&
                   roles(*acclamator_profile) ==
                       "weapon+210 weapon+210 weapon+240 weapon+210 weapon+210 weapon+240 engine+255 fighter_bay+150 ",
               "Acclamator: 6 weapons, engines and the fighter bay");
        const auto* mc80_profile = profile("Calamari_Cruiser");
        expect(mc80_profile != nullptr && mc80_profile->max_hull.raw() == raw(12750) &&
                   roles(*mc80_profile) == "weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 engine+735 ",
               "MC80: 8500 x 1.5 hull, 2 ion cannons and 4 lasers at 420 x 1.5, the engines at 490 x 1.5: "
                   + (mc80_profile == nullptr ? std::string("none")
                                              : std::to_string(mc80_profile->max_hull.raw() / Fixed::scale) + " " + roles(*mc80_profile)));
        for (const auto* id : {"Corellian_Corvette", "Tartan_Patrol_Cruiser"}) {
            const auto* corvette = profile(id);
            bool none_destroyable = corvette != nullptr && !corvette->hardpoints.empty();
            if (corvette != nullptr) {
                for (const auto& hardpoint : corvette->hardpoints) none_destroyable = none_destroyable && !hardpoint.destroyable;
            }
            expect(none_destroyable, std::string("corvette weapons are not destroyable: ") + id);
        }
        for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
            const auto* station_profile = profile(id);
            std::size_t shield_generators = 0;
            bool repairable = station_profile != nullptr;
            if (station_profile != nullptr) {
                for (const auto& hardpoint : station_profile->hardpoints) {
                    if (hardpoint.role == HardpointRole::shield_generator) {
                        ++shield_generators;
                        repairable = repairable && hardpoint.max_health.raw() == raw(975) &&
                                     hardpoint.repair_amount_per_frame.raw() == Fixed::from_decimal(".50").value().raw() &&
                                     hardpoint.repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw();
                    }
                }
            }
            expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && shield_generators == 1 &&
                       repairable,
                   std::string("station: 1600 x 1.5 hull, one repairable shield generator at 650 x 1.5: ") + id);
        }
        for (const auto& item : fleet) {
            if (item.kind == UnitKind::squadron) continue;
            if (item.kind != UnitKind::station) {
                for (const auto& hardpoint : item.hardpoints) {
                    expect(!hardpoint.repair_amount_per_frame && !hardpoint.repair_cost_per_frame,
                           "no ship or craft hardpoint authors repair: " + hardpoint.id);
                }
            }
            expect(item.destroyed_with_hardpoints, "FoC fleet type defaults to death with its hardpoints: " + item.id);
        }
    }

    const auto identity = eawr::units::content_identity(tables);
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : identity) {
        hex.push_back(digits[byte >> 4U]);
        hex.push_back(digits[byte & 15U]);
    }
    std::cout << "FoC fleet: " << tables.units.size() << " units, " << tables.projectiles.size() << " projectiles, "
              << tables.constants.damage_to_armor.size() << " damage/armor rows, " << tables.unresolved.size()
              << " unresolved, " << tables.notes.size() << " notes, identity " << hex << std::endl;
    print_rows("unresolved", tables.unresolved);
    print_rows("note", tables.notes);
    expect(tables.projectiles.size() >= 13 && tables.constants.damage_to_armor.size() >= 168,
           "production closure retains the pinned projectile and damage rows");
    expect(tables.unresolved.size() == 4 && std::all_of(tables.unresolved.begin(), tables.unresolved.end(),
        [](const auto& row) {
            return (row.owner == "Empire_Defense_Satellite_Repair" || row.owner == "Rebel_Defense_Satellite_Repair")
                ? row.field == "Space_FOW_Reveal_Range"
                : ((row.owner == "Slave_I" || row.owner == "TIE_Prototype") && row.field == "Targeting_Priority_Set");
        }), "only authored repair reveal and hero targeting gaps remain unresolved");
    expect(std::count_if(tables.notes.begin(), tables.notes.end(), [](const auto& row) {
        return row.owner == "Darth_Vader_TIE_Fighter_Container" && row.reason.find("WHE-70") != std::string::npos;
    }) == 1, "WHE-70: stock container replenishment is one member-dispatch note");
    // ML-53: the loader tolerates an unknown restriction name, so this is the stock-data guard against typos.
    expect(std::none_of(tables.unresolved.begin(), tables.unresolved.end(), [](const auto& row) {
        return row.field == "Fire_Category_Restrictions";
    }), "no stock hardpoint names an unknown Fire_Category_Restrictions category");
    expect(std::none_of(tables.unresolved.begin(), tables.unresolved.end(), [](const auto& row) {
        return row.owner == "UL_Extort_Cash_L1_Upgrade" || row.owner == "UL_Extort_Cash_L2_Upgrade";
    }), "upgrade objects never pass through the required ship-body fields");
    const auto pin_path = eawr::testing::environment("EAWR_UNIT_PINNED_REPLAY").value_or(EAWR_UNIT_PINNED_REPLAY);
    std::ifstream pinned(pin_path, std::ios::binary);
    const std::vector<std::uint8_t> pinned_bytes((std::istreambuf_iterator<char>(pinned)), std::istreambuf_iterator<char>());
    const auto replay_pin = eawr::sim::tactical::parse_replay(pinned_bytes);
    expect(replay_pin && replay_pin.value().setup.content_identity == identity,
           "FoC content identity matches the generated M2 replay pin");
    // WPR-50/52: every faction/level chain loads its menu's types through the same closure.
    for (const auto* faction : {"Rebel", "Empire"}) for (unsigned level = 1; level <= 5; ++level) {
        const auto name = std::string("Skirmish_") + faction + "_Star_Base_" + std::to_string(level);
        const auto* station = tables.find(name);
        expect(station != nullptr && !station->production.buildable.empty(), "station menu closure: " + name);
        if (station == nullptr) continue;
        expect(level == 5 || tables.find(station->production.next_level) != nullptr, "next station level resolves: " + name);
        for (const auto& list : station->production.buildable) for (const auto& type : list.types) {
            expect(tables.find(type) != nullptr, "buildable type resolves: " + type);
        }
    }

    // The Q24 root-to-bone composition agrees with a double-precision
    // composition of the same stored binary32 transforms.
    double worst = 0;
    std::size_t bones = 0;
    for (const auto& item : fleet) {
        if (item.model_path.empty()) continue;
        const auto* model = access.model(item.model_path);
        if (model == nullptr) continue;
        const auto frames = eawr::units::bind_frames(*model);
        expect(static_cast<bool>(frames), "bind frames: " + item.model_path);
        if (!frames) continue;
        std::vector<std::array<double, 12>> reference(model->bones.size());
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            std::array<double, 12> local{};
            for (std::size_t element = 0; element < 12; ++element) local[element] = model->bones[index].relative_transform[element];
            const auto parent = model->bones[index].parent;
            if (parent < 0) {
                reference[index] = local;
                continue;
            }
            const auto& p = reference[static_cast<std::size_t>(parent)];
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    double value = column == 3 ? p[row * 4 + 3] : 0.0;
                    for (std::size_t k = 0; k < 3; ++k) value += p[row * 4 + k] * local[k * 4 + column];
                    reference[index][row * 4 + column] = value;
                }
            }
        }
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            for (std::size_t row = 0; row < 3; ++row) {
                const double fixed = static_cast<double>(frames.value()[index].rows[row][3].raw()) / Fixed::scale;
                worst = std::max(worst, std::abs(fixed - reference[index][row * 4 + 3]));
            }
            ++bones;
        }
    }
    std::cout << "FoC fleet: " << bones << " bone translations, worst Q24 vs double difference " << worst << std::endl;
    expect(bones > 0 && worst < 1.0e-4, "Q24 bone composition matches the double reference");
    foc_gunboat_pole(input);
    foc_object_weapon_defaults(input);
    foc_fighter_muzzles(input);
    foc_living_collision(input);
    foc_resource_container(input);
    foc_container_not_opportunity(input);
}

} // namespace unit_tables_test_support

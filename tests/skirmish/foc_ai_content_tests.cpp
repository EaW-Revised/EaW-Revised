#include "../support/env.hpp"
#include "../support/check.hpp"
#include "foc_ai_fixtures.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/setup.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace foc_ai_test_support {

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(bool condition, const std::string& message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

std::optional<std::string> environment(const char* name) {
    return eawr::testing::environment(name);
}

std::optional<Content> load(const std::filesystem::path& root, bool rebel_ai, bool underworld,
                            std::vector<std::string> underworld_fleet, bool crusader,
                            std::optional<std::uint64_t> battle_seed,
                            std::vector<std::string> player_fleet,
                            std::vector<std::string> ai_fleet, bool three_vs_three,
                            bool serial_profile) {
    // The Crusader battle's Rebel player fields only torpedo bombers, so its deadly enemy fires missiles.
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return std::nullopt;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return std::nullopt;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return std::nullopt;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    if (crusader) {
        // The pinned M2 list and obstacles, plus the Underworld gunship.
        for (const auto id : eawr::units::pinned_m2_types()) input.types.emplace_back(id);
        input.types.emplace_back("Crusader_Gunship");
        for (const auto id : eawr::units::pinned_m2_obstacles()) input.obstacles.emplace_back(id);
    }
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    auto fixture = skirmish::m2_fixture();
    if (serial_profile) {
        skirmish::FixtureOptions options;
        options.map = "data/art/maps/_mp_space_coruscant.ted";
        options.seed = battle_seed.value_or(67);
        options.slots = std::vector<skirmish::LobbySlot>{};
        for (std::uint32_t index = 0; index < 6; ++index) {
            const bool rebel = index < 3;
            options.slots->push_back({index + 1, rebel ? "Rebel" : "Empire", rebel ? 0U : 1U,
                index == 0, rebel
                    ? std::vector<std::string>{"Y-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser"}
                    : std::vector<std::string>{"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Victory_Destroyer", "Imperial_Star_Destroyer"},
                std::nullopt});
        }
        auto chosen = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        expect(static_cast<bool>(chosen), "serial profile's stock Coruscant fixture resolves");
        if (!chosen) return std::nullopt;
        fixture = std::move(chosen).value();
    } else if (three_vs_three) {
        skirmish::SetupMapQuery query;
        query.minimum_capacity = 6;
        const auto maps = skirmish::setup_maps(filesystem.value(), catalog.value().catalog, query);
        expect(static_cast<bool>(maps), "stock diagnostic maps load");
        if (!maps) return std::nullopt;
        bool selected = false;
        for (const auto& map : maps.value()) {
            if (!map.unavailable.empty()) continue;
            for (const auto& rebel : map.teams) {
                if (rebel.spawn_capacity.value_or(0) < 3
                    || std::find(rebel.factions.begin(), rebel.factions.end(), "Rebel") == rebel.factions.end()) continue;
                for (const auto& empire : map.teams) {
                    if (empire.team == rebel.team || empire.spawn_capacity.value_or(0) < 3
                        || std::find(empire.factions.begin(), empire.factions.end(), "Empire") == empire.factions.end()) continue;
                    skirmish::FixtureOptions options;
                    options.map = map.path;
                    options.slots = std::vector<skirmish::LobbySlot>{};
                    for (std::uint32_t index = 0; index < 3; ++index)
                        options.slots->push_back({index + 1, "Rebel", rebel.team, index == 0, {}, std::nullopt});
                    for (std::uint32_t index = 0; index < 3; ++index)
                        options.slots->push_back({index + 4, "Empire", empire.team, false, {}, std::nullopt});
                    const auto chosen = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
                    if (!chosen) continue;
                    fixture = chosen.value();
                    selected = true;
                    break;
                }
                if (selected) break;
            }
            if (selected) break;
        }
        expect(selected, "installed content supplies an authored 3v3 space start");
        if (!selected) return std::nullopt;
        std::cout << "SQUADRON-TRACE map=" << fixture.map << " players=" << fixture.slots.size() << '\n';
    }
    if (battle_seed) fixture.seed = *battle_seed;
    if (rebel_ai) {
        fixture.slots[0].faction = "Empire";
        fixture.slots[0].fleet = {"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship"};
        fixture.slots[1].faction = "Rebel";
        fixture.slots[1].fleet = {"Rebel_X-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate"};
    }
    if (underworld) {
        fixture.slots[0].fleet = {"Rebel_X-Wing_Squadron", "Corellian_Corvette", "Corellian_Corvette"};
        if (std::find(underworld_fleet.begin(), underworld_fleet.end(), "IG-2000") != underworld_fleet.end())
            fixture.slots[0].fleet = {"Nebulon_B_Frigate"};
        if (!player_fleet.empty()) fixture.slots[0].fleet = player_fleet; // WST-16: the idle player's own fleet
        fixture.slots[1].faction = "Underworld";
        fixture.slots[1].fleet = underworld_fleet.empty()
            ? std::vector<std::string>{"StarViper_Squadron", "StarViper_Squadron", "StarViper_Squadron", "Skipray_Squadron"}
            : underworld_fleet;
    }
    // WLD-10: the Empire AI's fleet carries an Underworld Crusader against the idle Rebel torpedo bombers.
    if (crusader) {
        fixture.slots[1].fleet.push_back("Crusader_Gunship");
        fixture.slots[0].fleet = {"Y-Wing_Squadron", "Y-Wing_Squadron"};
    }
    // WAB-36 (legacy #2997): the AI (slots[1]) fields the given fleet: the Empire against idle Rebel
    // corvettes, or with rebel_ai the Rebels against the idle Empire group above.
    if (!ai_fleet.empty()) {
        fixture.slots[1].fleet = ai_fleet;
        if (!rebel_ai) fixture.slots[0].fleet = {"Corellian_Corvette", "Corellian_Corvette"};
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    if (underworld || !ai_fleet.empty() || serial_profile) {
        // As the Underworld plan fixture: keep the pinned M2 types and add the Underworld
        // fleet, the factions' authored forces and the station marker types.
        auto extended = input;
        for (const auto type : eawr::units::pinned_m2_types()) extended.types.emplace_back(type);
        for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended.obstacles.emplace_back(obstacle);
        for (const auto& slot : fixture.slots) extended.types.insert(extended.types.end(), slot.fleet.begin(), slot.fleet.end());
        for (const auto& forces : inputs.value().faction_forces)
            for (const auto& type : forces.space_skirmish_default_forces) extended.types.push_back(type);
        for (const auto& placement : inputs.value().placements)
            for (const auto& candidate : placement.marker_for) extended.types.push_back(candidate.type);
        tables = eawr::units::load_unit_tables(extended);
        expect(static_cast<bool>(tables), "selected fixture's unit tables load");
        if (!tables) return std::nullopt;
        inputs.value().tables = &tables.value();
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds" + (start ? std::string() : ": " + start.error().message));
    if (!start) return std::nullopt;
    if (three_vs_three || serial_profile) start.value().setup = skirmish::recording_setup(fixture, start.value());
    if (!rebel_ai && !three_vs_three && !serial_profile) {
        // FH-20/WHZ-50: this controlled two-faction engagement omits Hutt map fixtures.
        // Their authored Structure category is a valid nearer target in the stock scene.
        std::set<eawr::sim::EntityId> omitted;
        for (const auto& unit : start.value().units) {
            const auto owner = std::find_if(start.value().players.begin(), start.value().players.end(),
                [&](const auto& player) { return player.player.player_id == unit.state.owner; });
            if (unit.role == skirmish::UnitRole::map_object && owner != start.value().players.end()
                && owner->faction == "Hutts") omitted.insert(unit.state.entity_id);
        }
        std::erase_if(start.value().units, [&](const auto& unit) { return omitted.contains(unit.state.entity_id); });
        std::erase_if(start.value().setup.units, [&](const auto& unit) { return omitted.contains(unit.entity_id); });
    }
    auto economy = skirmish::economy_rules(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(economy), "skirmish economy loads: " + (economy ? std::string() : economy.error().message));
    if (!economy) return std::nullopt;
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    // #495: the AI battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    content.value().map_box = skirmish::map_box(inputs.value());
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}, {}, std::move(economy).value()};
    // FT-02/WNO-11: the freestore's regional fallback needs the perception/goal host
    // when its forced nearest query finds no object. Match the production live AI setup.
    auto enabled = skirmish::enable_goal_system(filesystem.value(), out.ai);
    expect(static_cast<bool>(enabled), "the tactical goal system loads");
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load: " + (modules ? std::string() : modules.error().message));
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    auto plan_modules = skirmish::ai_modules(filesystem.value(), out.ai, true);
    expect(static_cast<bool>(plan_modules), "the selected plans load from the FoC view");
    if (!plan_modules) return std::nullopt;
    out.plan_modules = std::move(plan_modules).value();
    for (const auto& type : tables.value().units) out.type_names.emplace(skirmish::type_id(type.id), type.id);
    return out;
}

} // namespace foc_ai_test_support

#include "unit_tables_support.hpp"
// P2-02 (#65) unit-table contracts. The synthetic catalog, priority sets,
// constants and bone hierarchies are invented here. With EAWR_EAW_GAME_ROOT
// set, the pinned FoC fleet is also loaded read-only from the installation and
// its counts, references and content identity are pinned; nothing from the
// installation is written.

#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/platform/live_ai.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/roster_gate.hpp"

#include "../../src/units/unit_internal.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unit_tables_test_support {


int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

std::int64_t raw(const std::int64_t whole) { return whole * Fixed::scale; }

Vec3 point(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {Fixed::from_raw(x), Fixed::from_raw(y), Fixed::from_raw(z)};
}


void collision_extents() {
    auto wide = model("data/art/models/wide.alo", {bone("Root", -1, {0, 0, 0})});
    eawr::assets::Mesh hull;
    hull.collidable = true;
    hull.bone = 0;
    hull.bounds_min = {-100.0F, -10.0F, -5.0F};
    hull.bounds_max = {100.0F, 10.0F, 5.0F};
    wide.meshes.push_back(hull);
    const auto frames = eawr::units::bind_frames(wide);
    expect(static_cast<bool>(frames), "the wide model's frames bind");
    if (!frames) return;
    const auto one = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(one && one->first.raw() == raw(100) && one->second.raw() == raw(10), "an X-wide mesh: half extents 100 and 10");
    // A second, Y-long mesh off to the side widens only the axes it reaches past.
    eawr::assets::Mesh mast;
    mast.collidable = true;
    mast.bone = 0;
    mast.bounds_min = {-20.0F, -30.0F, -5.0F};
    mast.bounds_max = {-10.0F, 50.0F, 5.0F};
    wide.meshes.push_back(mast);
    const auto two = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(two && two->first.raw() == raw(100) && two->second.raw() == raw(40), "two meshes: the union box's half extents");
    auto strict = eawr::units::prepare_galactic_collision_bounds(wide);
    expect(strict && strict.value().min.z.raw() == raw(-5) && strict.value().max.z.raw() == raw(5),
        "GC collision bounds reuse all collision mesh corners");
    wide.bones[0].relative_transform[11] = 7.0F;
    strict = eawr::units::prepare_galactic_collision_bounds(wide);
    expect(strict && strict.value().min.z.raw() == raw(2) && strict.value().max.z.raw() == raw(12),
        "GC collision bounds retain reference bone translation");
    wide.meshes[0].collidable = wide.meshes[1].collidable = false;
    wide.meshes[0].bounds_min.z = -1000.0F;
    strict = eawr::units::prepare_galactic_collision_bounds(wide);
    expect(strict && strict.value().min == point(raw(-1), raw(-1), raw(-1)) && strict.value().max == point(raw(1), raw(1), raw(1)),
        "no collision-enabled meshes use the sourced unit box regardless of render bounds");
    wide.meshes[0].collidable = true; wide.meshes[0].bone = 999;
    expect(!eawr::units::prepare_galactic_collision_bounds(wide), "missing collision mesh bone refuses");
    wide.meshes[0].bone = 0; wide.meshes[0].bounds_min.z = std::numeric_limits<float>::quiet_NaN();
    expect(!eawr::units::prepare_galactic_collision_bounds(wide), "malformed collision bounds cannot become the no-mesh fallback");
    wide.meshes[0].bounds_min.z = 6.0F;
    expect(!eawr::units::prepare_galactic_collision_bounds(wide), "reversed collision bounds refuse");
}


void engine_disable_durations() {
    for (const auto duration : {"", "0", "-1", "-0.25", "20", "3601"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Engine_Disable_Test");
        const auto start = xml.find("<Projectile Name=\"Proj_Test_Small\">")
            + std::string_view{"<Projectile Name=\"Proj_Test_Small\">"}.size();
        xml.insert(start, "<Projectile_Disables_Engines_When_Power_Drained>Yes</Projectile_Disables_Engines_When_Power_Drained>"
            + (std::string_view(duration).empty() ? std::string{} : "<Projectile_Disable_Engines_Duration>"
                + std::string(duration) + "</Projectile_Disable_Engines_Duration>"));
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "EN-08: duration fixture loads");
        if (!loaded.tables) continue;
        expect(!has_row(loaded.tables->unresolved, "Proj_Test_Small", "Projectile_Disable_Engines_Duration"),
            "EN-08: an absent duration is a supported default");
        const auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "EN-08: absent, negative, stock and long durations bind");
        if (!combat) continue;
        const auto expected = std::string_view(duration).starts_with('-')
            ? eawr::sim::tactical::indefinite_engine_disable_frames
            : std::string_view(duration) == "20" ? 600U : std::string_view(duration) == "3601" ? 108030U : 0U;
        bool found = false;
        for (const auto& profile : combat.value().profiles) {
            for (const auto& weapon : profile.weapons) {
                if (!weapon.shot || !weapon.shot->disable_engines_frames) continue;
                found = true;
                expect(*weapon.shot->disable_engines_frames == expected, "EN-08: duration maps to the sourced frame rule");
            }
        }
        expect(found, "EN-08: enabled projectile reaches a weapon profile");
        expect(static_cast<bool>(eawr::sim::tactical::validate_combat(combat.value())),
            "EN-08: bound duration passes simulation validation");
    }
}

} // namespace

using namespace unit_tables_test_support;

int main(const int argc, char** argv) {
    // A selected installed-mod battle, using the same faction dependency closure as the viewer.
    // Arguments supply all installed paths, the playable map and both faction names.
    if (argc == 7 && std::string_view(argv[1]) == "--battle-mod") {
        const auto fail = [](const eawr::core::Diagnostic& diagnostic) {
            std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
            return 1;
        };
        auto roots = eawr::vfs::mod_chain_roots(std::filesystem::path(argv[3]));
        roots.emplace_back("expansion", std::filesystem::path(argv[2]) / "corruption" / "Data");
        roots.emplace_back("base", std::filesystem::path(argv[2]) / "GameData" / "Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        if (!chain) return fail(chain.error());
        std::vector<eawr::vfs::MountSpec> specs;
        for (auto& layer : chain.value()) specs.push_back(std::move(layer.mount));
        auto filesystem = eawr::vfs::Vfs::mount(specs);
        if (!filesystem) return fail(filesystem.error());
        auto catalog = eawr::data::load_catalog_strict(filesystem.value(), eawr::data::Profile::remake);
        if (!catalog) return fail(catalog.error());
        eawr::skirmish::FixtureOptions options;
        options.map = argv[4];
        options.slots = std::vector<eawr::skirmish::LobbySlot>{
            {1, argv[5], 0, false, {}, {}}, {2, argv[6], 1, false, {}, {}}};
        auto fixture = eawr::skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        if (!fixture) return fail(fixture.error());
        eawr::scene::VfsAssetCache cache(filesystem.value());
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog;
        input.filesystem = &filesystem.value();
        input.model = cache.access().model;
        input.space_map = fixture.value().map;
        auto tables = eawr::units::load_unit_tables(input);
        if (!tables) return fail(tables.error());
        auto inputs = eawr::skirmish::read_start_inputs(fixture.value(), filesystem.value(),
            catalog.value().catalog, tables.value());
        if (!inputs) return fail(inputs.error());
        std::set<std::string> missing;
        const auto need = [&](const std::string& type) {
            if (!eawr::skirmish::roster_disabled_types().contains(eawr::skirmish::type_id(type))
                && !tables.value().find(type)) missing.insert(type);
        };
        for (const auto& slot : fixture.value().slots) {
            for (const auto& type : slot.fleet) need(type);
            for (const auto& forces : inputs.value().faction_forces) {
                if (!eawr::units::detail::iequals(forces.faction, slot.faction)) continue;
                for (const auto& type : forces.space_skirmish_default_forces) need(type);
            }
            const auto marker = "team_" + std::string(slot.team < 10 ? "0" : "")
                + std::to_string(slot.team) + "_space_station";
            for (const auto& placement : inputs.value().placements) {
                if (!placement.marker || !eawr::units::detail::iequals(placement.type, marker)) continue;
                for (const auto& candidate : placement.marker_for) {
                    for (const auto& affiliation : eawr::units::detail::tokens(candidate.affiliation))
                        if (eawr::units::detail::iequals(affiliation, slot.faction)) need(candidate.type);
                }
            }
        }
        if (!missing.empty()) {
            for (const auto type : eawr::units::pinned_m2_types()) input.types.emplace_back(type);
            input.types.insert(input.types.end(), missing.begin(), missing.end());
            for (const auto type : eawr::units::pinned_m2_obstacles()) input.obstacles.emplace_back(type);
            tables = eawr::units::load_unit_tables(input);
            if (!tables) return fail(tables.error());
            inputs = eawr::skirmish::read_start_inputs(fixture.value(), filesystem.value(),
                catalog.value().catalog, tables.value());
            if (!inputs) return fail(inputs.error());
        }
        print_rows("notes", tables.value().notes);
        print_rows("unresolved", tables.value().unresolved);
        auto start = eawr::skirmish::build_start(fixture.value(), inputs.value());
        if (!start) return fail(start.error());
        auto content = eawr::skirmish::session_content(tables.value(), eawr::skirmish::human_slots(fixture.value()));
        if (!content) return fail(content.error());
        auto fog = eawr::skirmish::fog_rules(inputs.value());
        if (!fog) return fail(fog.error());
        auto economy = eawr::skirmish::economy_rules(start.value(), inputs.value(), tables.value());
        if (!economy) return fail(economy.error());
        std::map<eawr::sim::tactical::PlayerId, std::string> factions;
        for (const auto& player : start.value().players) factions.emplace(player.player.player_id, player.faction);
        auto scoring = eawr::platform::BattleScoring::create(filesystem.value(), catalog.value().catalog,
            tables.value(), std::move(factions), fixture.value().map);
        if (!scoring) return fail(scoring.error());
        std::shared_ptr<const eawr::platform::BattleScoring> shared_scoring(std::move(scoring).value());
        auto journal = std::make_shared<eawr::script::foc::AiJournal>();
        auto ai = eawr::platform::live_ai(start.value(), inputs.value(), tables.value(), filesystem.value(), shared_scoring, journal);
        if (!ai) return fail(ai.error());
        if (ai.value().players.size() != 2) { std::cerr << "Expected two initialized AI players\n"; return 1; }
        const auto traits = eawr::skirmish::ai_setup(start.value(), inputs.value(), tables.value());
        std::map<eawr::sim::tactical::TypeId, const eawr::script::foc::AiType*> type_traits;
        for (const auto& type : traits.content.types) type_traits.emplace(type.type_id, &type);
        for (const auto& unit : start.value().setup.units) {
            if (std::find(ai.value().players.begin(), ai.value().players.end(), unit.owner) == ai.value().players.end()) continue;
            const auto type = type_traits.find(unit.type_id);
            if (type == type_traits.end()) continue;
            std::cout << "AI initial unit: player=" << unit.owner << ", unit=" << unit.entity_id
                      << ", type=" << type->second->name << ", locomotor=" << type->second->locomotor
                      << ", squadron=" << type->second->squadron << ", craft=" << type->second->craft
                      << ", ship_motion=" << (content.value().motion.find(unit.type_id) != nullptr)
                      << ", formation_motion=" << (content.value().motion.squadrons.find_squadron(unit.type_id) != nullptr) << '\n';
        }
        eawr::platform::LiveSession::Options live_options;
        live_options.workers = 2;
        live_options.pacing = eawr::platform::LiveSession::Pacing::driven;
        live_options.scripts = ai.value().scripts;
        const auto& bound = content.value();
        auto session = eawr::platform::LiveSession::start(
            eawr::skirmish::recording_setup(fixture.value(), start.value()), bound.sensors, bound.durability,
            bound.motion, bound.combat, live_options, eawr::skirmish::victory_rules(start.value(), tables.value()),
            fog.value(), bound.abilities, economy.value(), eawr::skirmish::map_box(inputs.value()));
        if (!session) return fail(session.error());
        auto& live = *session.value();
        live.advance_to(1);
        if (!live.wait_for(1, std::chrono::seconds(60))) {
            live.stop();
            if (const auto failure = live.failure()) return fail(*failure);
            std::cerr << "First simulation frame timed out\n";
            return 1;
        }
        std::cout << "battle first simulation frame reached with AI\n";
        live.advance_to(1800);
        const bool finished = live.wait_for(1800, std::chrono::seconds(600));
        live.stop();
        if (const auto failure = live.failure()) return fail(*failure);
        if (!finished) { std::cerr << "Battle did not complete 1800 ticks\n"; return 1; }
        const auto report = live.script_report();
        for (const auto& diagnostic : report.diagnostics) std::cerr << "AI diagnostic: " << diagnostic << '\n';
        const auto hashes = live.tick_hashes();
        std::map<eawr::sim::tactical::PlayerId, std::uint64_t> production, attacks, moves, guards;
        for (const auto& command : live.record().commands) {
            const auto player = command.key.player_id;
            if (std::holds_alternative<eawr::sim::tactical::BuyPayload>(command.payload)
                || std::holds_alternative<eawr::sim::tactical::PadBuildPayload>(command.payload)) ++production[player];
            if (std::holds_alternative<eawr::sim::tactical::AttackPayload>(command.payload)
                || std::holds_alternative<eawr::sim::tactical::AttackMovePayload>(command.payload)) ++attacks[player];
            if (std::holds_alternative<eawr::sim::tactical::MovePayload>(command.payload)) ++moves[player];
            if (std::holds_alternative<eawr::sim::tactical::GuardPayload>(command.payload)) ++guards[player];
            if (const auto* purchase = std::get_if<eawr::sim::tactical::BuyPayload>(&command.payload)) {
                const auto type = type_traits.find(purchase->type);
                std::cout << "AI production request: player=" << player << ", type="
                          << (type == type_traits.end() ? std::to_string(purchase->type) : type->second->name) << '\n';
            }
        }
        std::ofstream plan_trace("ai-plan-journal.tsv");
        plan_trace << "tick\tplayer\tplan\tgoal\ttarget\tevent\tdetail\n";
        std::map<std::pair<eawr::sim::tactical::PlayerId, std::string>, std::uint64_t> plan_events;
        for (const auto& row : journal->plans) {
            auto detail = row.detail;
            for (auto& character : detail) if (character == '\n' || character == '\r' || character == '\t') character = ' ';
            plan_trace << row.tick << '\t' << row.player << '\t' << row.plan << '\t' << row.goal << '\t'
                       << row.target << '\t' << row.event << '\t' << detail << '\n';
            ++plan_events[{row.player, row.event}];
        }
        plan_trace.close();
        if (!plan_trace) { std::cerr << "Cannot write AI plan diagnostic journal\n"; return 1; }
        for (const auto& [key, count] : plan_events)
            std::cout << "AI plan events: player=" << key.first << ", event=" << key.second << ", count=" << count << '\n';
        std::uint64_t free_runs{}, goals_scored{}, plans_attached{};
        for (const auto& row : journal->costs) {
            free_runs += row.freestore_runs; goals_scored += row.goals_evaluated; plans_attached += row.plans_attached;
        }
        std::cout << "AI services: freestore=" << free_runs << ", goals=" << goals_scored
                  << ", plans=" << plans_attached << ", dropped plan diagnostics=" << journal->plans_dropped << '\n';
        bool active = true;
        for (const auto player : ai.value().players) {
            std::cout << "AI activity: player=" << player << ", production=" << production[player]
                      << ", attacks=" << attacks[player] << ", moves=" << moves[player] << ", guards=" << guards[player] << '\n';
            active = active && production[player] != 0 && attacks[player] != 0;
        }
        std::cout << "battle acceptance: ticks=" << live.completed_tick()
                  << ", types=" << tables.value().units.size() << ", notes=" << tables.value().notes.size()
                  << ", AI players=" << ai.value().players.size() << ", script ticks=" << report.ticks
                  << ", AI diagnostics=" << report.diagnostics.size()
                  << ", rejected orders=" << live.rejected_orders().size()
                  << ", hash=" << (hashes.empty() ? std::string{} : hashes.back()) << '\n';
        if (!active) { std::cerr << "Each AI must issue production and attack orders\n"; return 1; }
        return 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--installed"
        || std::string_view(argv[1]) == "--formation-deceleration-installed")) {
        const auto root = environment("EAWR_EAW_GAME_ROOT");
        if (!root || root->empty()) return 77;
        foc_fleet(std::string_view(argv[1]) == "--formation-deceleration-installed");
        return failures == 0 ? 0 : 1;
    }
    // Installed compatibility checks use the real manifest chain and model cache.
    // Scan mode overlays the extracted effective XML above the installed assets.
    if (argc == 5 && (std::string_view(argv[1]) == "--engine-disable-mod"
        || std::string_view(argv[1]) == "--mod-loader-scan")) {
        const bool scan = std::string_view(argv[1]) == "--mod-loader-scan";
        auto roots = eawr::vfs::mod_chain_roots(std::filesystem::path(argv[3]));
        roots.emplace_back("expansion", std::filesystem::path(argv[2]) / "corruption" / "Data");
        roots.emplace_back("base", std::filesystem::path(argv[2]) / "GameData" / "Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        if (!chain) { std::cerr << chain.error().message << '\n'; return 1; }
        std::vector<eawr::vfs::MountSpec> specs;
        if (scan) specs.push_back({"effective", std::filesystem::path(argv[4]) / "data", "data", {}});
        for (auto& layer : chain.value()) specs.push_back(std::move(layer.mount));
        auto filesystem = eawr::vfs::Vfs::mount(specs);
        if (!filesystem) { std::cerr << filesystem.error().message << '\n'; return 1; }
        auto catalog = eawr::data::load_catalog_strict(filesystem.value(), eawr::data::Profile::remake);
        if (!catalog) { std::cerr << catalog.error().message << '\n'; return 1; }
        eawr::scene::VfsAssetCache cache(filesystem.value());
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog;
        input.filesystem = &filesystem.value();
        input.model = cache.access().model;
        std::vector<std::string> types;
        std::size_t resolved = 0;
        if (scan) {
            for (const auto& definition : catalog.value().catalog.definitions()) {
                if (!definition.namespace_winner) continue;
                const auto object = catalog.value().catalog.resolve(definition.id, definition.category);
                if (!object) {
                    std::cout << "ERROR\tcatalog\t" << definition.id << '\t' << object.error().message << '\n';
                    ++failures;
                    continue;
                }
                ++resolved;
                if (definition.category == eawr::data::Category::game_object
                    && (object.value().value("Space_Model_Name") || definition.type_name == "Squadron"
                        || definition.type_name == "StarBase" || definition.type_name == "SpaceUnit"))
                    types.push_back(definition.id);
            }
        } else types.emplace_back(argv[4]);
        std::size_t loaded_count = 0;
        for (const auto& type : types) {
            input.types = {type};
            const auto tables = eawr::units::load_unit_tables(input);
            if (!tables) {
                std::cout << "ERROR\tunits\t" << type << '\t' << tables.error().message << '\n';
                ++failures;
                continue;
            }
            ++loaded_count;
            const auto check = [&](const std::string_view stage, const auto& result) {
                if (!result) {
                    std::cout << "ERROR\t" << stage << '\t' << type << '\t' << result.error().message << '\n';
                    ++failures;
                }
            };
            const auto combat = eawr::units::combat_table(tables.value());
            check("combat", combat);
            if (scan) {
                check("durability", eawr::units::durability_table(tables.value()));
                check("motion", eawr::units::motion_table(tables.value()));
                check("abilities", eawr::units::ability_table(tables.value()));
            } else {
                bool found = false;
                for (const auto& projectile : tables.value().projectiles) {
                    if (!projectile.disables_engines_when_power_drained || projectile.disable_engines_duration) continue;
                    found = true;
                    std::cout << "default-duration projectile\t" << projectile.id << '\n';
                }
                expect(found, "EN-08: installed type reaches an enabled projectile with no authored duration");
                if (combat) expect(static_cast<bool>(eawr::sim::tactical::validate_combat(combat.value())),
                    "EN-08: installed combat profile passes simulation validation");
            }
        }
        std::cout << "installed loader: resolved " << resolved << ", tactical types " << types.size()
                  << ", loaded " << loaded_count << ", failures " << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--engine-disable") {
        engine_disable_durations();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--space-population") {
        space_population();
        return failures == 0 ? 0 : 1;
    }
    // Targeted installed-mod regression: game root, mod chain, craft type. This uses
    // real XML inheritance and model resolution without starting a renderer.
    if (argc == 5 && std::string_view(argv[1]) == "--bank-angle-mod") {
        auto roots = eawr::vfs::mod_chain_roots(std::filesystem::path(argv[3]));
        roots.emplace_back("expansion", std::filesystem::path(argv[2]) / "corruption" / "Data");
        roots.emplace_back("base", std::filesystem::path(argv[2]) / "GameData" / "Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        if (!chain) { std::cerr << chain.error().message << '\n'; return 1; }
        std::vector<eawr::vfs::MountSpec> specs;
        for (auto& layer : chain.value()) specs.push_back(std::move(layer.mount));
        auto filesystem = eawr::vfs::Vfs::mount(specs);
        if (!filesystem) { std::cerr << filesystem.error().message << '\n'; return 1; }
        auto catalog = eawr::data::load_catalog_strict(filesystem.value(), eawr::data::Profile::remake);
        if (!catalog) { std::cerr << catalog.error().message << '\n'; return 1; }
        eawr::scene::VfsAssetCache cache(filesystem.value());
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog;
        input.filesystem = &filesystem.value();
        input.model = cache.access().model;
        input.types = {argv[4]};
        auto tables = eawr::units::load_unit_tables(input);
        if (!tables) { std::cerr << tables.error().message << '\n'; return 1; }
        auto motion = eawr::units::motion_table(tables.value());
        if (!motion) { std::cerr << motion.error().message << '\n'; return 1; }
        const auto* craft = motion.value().squadrons.find_craft(eawr::assets::object_type_crc(argv[4]));
        expect(craft && craft->bank_angle == Fixed::from_raw(raw(360)),
            "FM-03: installed mod craft binds with its authored bank of 360");
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable}};
        expect(static_cast<bool>(eawr::sim::tactical::TacticalSession::create(setup, {}, {}, motion.value())),
            "FM-03: installed mod motion tables pass session creation");
        std::cout << "installed bank-angle loader: " << argv[4] << ", craft "
                  << motion.value().squadrons.craft.size() << ", failures " << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--object-scripts") {
        object_script_inheritance();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--formation-deceleration") {
        synthetic_tables();
        return failures == 0 ? 0 : 1;
    }
    if (argc != 1) return 1;
    {
        for (const bool authored : {false, true}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const auto begin = xml.find("<Unit_Abilities_Data");
            const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
            const std::string fields = authored
                ? "<Charge_Up_Seconds>5</Charge_Up_Seconds><Damage_Multiplier>5</Damage_Multiplier>" : "";
            xml.replace(begin, end - begin, "<Unit_Abilities_Data><Unit_Ability><Type>BLAST</Type>"
                "<Recharge_Seconds>90</Recharge_Seconds><GUI_Activated_Ability_Name>Test_Blast</GUI_Activated_Ability_Name>"
                "</Unit_Ability></Unit_Abilities_Data><Abilities><Blast_Ability Name=\"Test_Blast\">"
                "<Activation_Style>User_Input</Activation_Style>" + fields + "</Blast_Ability></Abilities>");
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            expect(loaded.tables.has_value(), "BLAST required-field fixture loads");
            if (!loaded.tables) continue;
            for (const auto tag : {"Charge_Up_Seconds", "Damage_Multiplier"}) {
                expect(has_row(loaded.tables->unresolved, "Test_Frigate", tag) == !authored,
                    "WHE-35/36: absent required BLAST field is reported; authored field has no gap");
            }
        }
    }
    {
        // FW-25: validated inherited flags apply independently of XML class.
        for (const auto& class_name : {"StarBase", "SpaceBuildable", "SpecialStructure", "SecondaryStructure", "Marker"}) {
            TempTree tree;
            auto xml = units_xml("3600", "Fog_Tag_Test");
            xml.insert(xml.rfind("</Objects>"), std::string("<") + class_name + " Name=\"Fog_Test\">"
                "<Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
                "<Last_State_Visible_Under_FOW>Yes</Last_State_Visible_Under_FOW>"
                "<Initial_State_Visible_Under_FOW>True</Initial_State_Visible_Under_FOW>"
                "<Faction_Anim_Subindex>Neutral, 2</Faction_Anim_Subindex>"
                "<Faction_Anim_Subindex>neutral, 3</Faction_Anim_Subindex></" + class_name + ">");
            write_fixture(tree.root, xml);
            const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
            auto filesystem = eawr::vfs::Vfs::mount(mounts);
            if (!filesystem) { expect(false, "fog flag fixture mounts"); continue; }
            auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
            if (!catalog) { expect(false, "fog flag catalog loads"); continue; }
            eawr::units::LoadInput input;
            input.catalog = &catalog.value().catalog;
            input.filesystem = &filesystem.value();
            input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
                static const auto assets = models(0);
                const auto found = assets.find(std::string(path));
                return found == assets.end() ? nullptr : &found->second;
            };
            input.types = {"Fog_Test"};
            auto loaded = eawr::units::load_unit_tables(input);
            expect(loaded && loaded.value().find("Fog_Test")
                && loaded.value().find("Fog_Test")->last_state_visible_under_fow
                && loaded.value().find("Fog_Test")->initial_state_visible_under_fow, "inherited fog flags load for every structure class");
            if (loaded) {
                expect(loaded.value().find("Fog_Test")->neutral_fog_animation_index == 2U,
                    "neutral faction animation uses the first matching validated row");
                auto changed = loaded.value();
                for (auto& type : changed.units) {
                    type.last_state_visible_under_fow = false;
                    type.initial_state_visible_under_fow = false;
                    type.neutral_fog_animation_index = 1U;
                }
                expect(eawr::units::content_identity(changed) == eawr::units::content_identity(loaded.value()),
                    "fog memory flags leave the replay content identity unchanged");
            }
        }
    }
    synthetic_tables();
    for (const std::string value : {"", "Yes", "true", "1", "no", "0"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string behavior = "TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR";
        xml.replace(xml.find(behavior), behavior.size(), behavior + ", TURRET");
        const std::string fields = "<Turret_Bone_Name>HP_F-L_BONE</Turret_Bone_Name><Barrel_Bone_Name>HP_F-L_BONE</Barrel_Bone_Name>"
            "<Turret_Rotate_Speed>3.5</Turret_Rotate_Speed><Projectile_Types>Proj_Test_Small</Projectile_Types>"
            + (value.empty() ? std::string{} : "<Turret_XY_Only>" + value + "</Turret_XY_Only>");
        const auto start = xml.find("<SpaceUnit Name=\"Test_Frigate\">") + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size();
        xml.insert(start, fields);
        write_fixture(tree.root, xml);
        auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        auto* unit = const_cast<eawr::units::UnitType*>(loaded.tables->find("Test_Frigate"));
        const bool expected = value == "Yes" || value == "true" || value == "1";
        expect(unit && unit->object_turret && unit->object_turret->xy_only == expected
            && unit->object_turret->rest_delay == eawr::sim::tactical::default_object_turret_rest_delay,
            "UA-05/FC-61: one retail boolean parser and absent-tag rest default feed ordinary turrets");
        if (!unit || !unit->object_turret) continue;
        const auto object_weapon = [](const auto& combat) -> const eawr::sim::tactical::WeaponProfile* {
            if (!combat) return nullptr;
            const auto* profile = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
            if (!profile) return nullptr;
            for (const auto& weapon : profile->weapons)
                if (weapon.hardpoint == eawr::sim::tactical::object_weapon) return &weapon;
            return nullptr;
        };
        const auto combat = eawr::units::combat_table(*loaded.tables);
        const auto* weapon = object_weapon(combat);
        expect(weapon && weapon->object_turret && weapon->object_turret->xy_only == expected
            && weapon->object_turret->barrel, "UA-05: usable barrel and validated XY-only reach the sim profile together");
        unit->object_turret->barrel.axes.reset();
        const auto no_axes = eawr::units::combat_table(*loaded.tables);
        weapon = object_weapon(no_axes);
        expect(weapon && weapon->object_turret && !weapon->object_turret->barrel,
            "UA-05: a barrel bone without axes uses the no-barrel profile for both consumers");
        unit->object_turret->turret.axes = std::array<Vec3, 3>{};
        const auto invalid = eawr::units::combat_table(*loaded.tables);
        expect(!invalid && invalid.error().message.find("unusable bone axes") != std::string::npos,
            "UA-05: unusable required turret axes report a contract failure instead of silently disabling fire");
    }
    engine_disable_durations();
    object_weapon_defaults();
    squadron_container_health();
    retail_value_rules();
    living_collision_admission();
    ship_suitability_content();
    height_adjusted_aim();
    object_weapon_muzzles();
    presentation_admission();
    priority_rules();
    content_identity();
    {
        TempTree tree;
        write_fixture(tree.root, units_xml("250", "Select_Test"));
        const auto assets = models(1.0F);
        const auto legacy = load(tree.root, assets);
        write(tree.root / "XML" / "DifficultyAdjustments.xml", R"xml(<Difficulty_Adjustments>
          <Difficulty_Adjustment Name="Easy_Default"><Credit_Multiplier>0.5</Credit_Multiplier></Difficulty_Adjustment>
          <Difficulty_Adjustment Name="Normal_Default"><Credit_Multiplier>1</Credit_Multiplier></Difficulty_Adjustment>
          <Difficulty_Adjustment Name="Hard_Default"><Credit_Multiplier>1.2</Credit_Multiplier></Difficulty_Adjustment>
        </Difficulty_Adjustments>)xml");
        const auto normal = load(tree.root, assets);
        expect(legacy.tables && normal.tables && eawr::units::content_identity(*legacy.tables)
            == eawr::units::content_identity(*normal.tables), "WPR-12 neutral credit rules retain the Normal replay identity");
        for (const auto& [difficulty, value] : {std::pair{"Easy_Default", "0.5"}, std::pair{"Hard_Default", "1.2"}}) {
            const auto alternate = load(tree.root, assets, difficulty);
            expect(alternate.tables && alternate.tables->ai_credit_multiplier == Fixed::from_decimal(value).value(),
                "WPR-12 selected difficulty reads its credit multiplier");
            expect(alternate.tables && normal.tables && eawr::units::content_identity(*alternate.tables)
                != eawr::units::content_identity(*normal.tables), "WPR-12 alternate credit rules bind a different replay identity");
        }
        const auto unknown = load(tree.root, assets, "Unknown_Default");
        expect(unknown.tables && !unknown.tables->ai_credit_multiplier
            && has_row(unknown.tables->unresolved, "Unknown_Default", "Credit_Multiplier"),
            "WPR-12 missing difficulty stays an explicit data gap");
    }
    {
        eawr::units::UnitTables before;
        before.units.emplace_back();
        auto after = before;
        expect(after == before, "structural table equality of a copy");
        after.units.front().tech_level = 1;
        expect(after != before, "structural comparison includes AI tech level");
        expect(eawr::units::content_identity(after) == eawr::units::content_identity(before),
               "structural comparison leaves replay identity unchanged");
        after = before;
        after.units.front().spin_away_time = Fixed::from_raw(Fixed::scale);
        expect(after != before, "structural comparison includes spin-away fields");
        after = before;
        after.units.front().abilities.emplace_back();
        before.units.front().abilities.emplace_back();
        after.units.front().abilities.front().projectile_override = "another_projectile";
        expect(after != before, "structural comparison includes ability projectile override");
    }
    {
        eawr::units::UnitTables defaults;
        defaults.units.emplace_back().capture_point = true;
        defaults.obstacles.emplace_back().id = "capture_candidate";
        auto opted_out = defaults;
        opted_out.obstacles.front().influences_capture = false;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "an obstacle capture opt-out changes the pad rules identity");
        opted_out = defaults;
        opted_out.obstacles.front().construction_blocker = false;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "an obstacle construction opt-out changes the pad rules identity");
        opted_out = defaults;
        opted_out.obstacles.front().living_projectile_collision = true;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "WBP-50: obstacle admission to capture/build queries changes the pad rules identity");
    }
    bind_frame_errors();
    hunt_tables();
    {
        namespace t = eawr::sim::tactical;
        for (const auto kind : {t::AbilityKind::tractor_beam, t::AbilityKind::leech_shields, t::AbilityKind::blast,
                t::AbilityKind::energy_weapon}) {
            eawr::units::UnitTables tables;
            auto& unit = tables.units.emplace_back(); unit.id = "Dependency_Fixture"; unit.kind = eawr::units::UnitKind::ship;
            auto& authored = unit.abilities.emplace_back();
            authored.type = kind == t::AbilityKind::tractor_beam ? "TRACTOR_BEAM" : kind == t::AbilityKind::leech_shields
                ? "LEECH_SHIELDS" : kind == t::AbilityKind::blast ? "BLAST" : "ENERGY_WEAPON";
            authored.gui_activated_ability_name = "dependency";
            auto& nested = unit.special_abilities.emplace_back(); nested.name = "dependency";
            nested.kind = kind == t::AbilityKind::blast ? t::SpecialAbilityKind::blast
                : kind == t::AbilityKind::leech_shields ? t::SpecialAbilityKind::leech_shields
                : kind == t::AbilityKind::tractor_beam ? t::SpecialAbilityKind::tractor_beam : t::SpecialAbilityKind::energy_weapon;
            nested.style = t::SpecialActivationStyle::user_input;
            nested.leech_duration_frames = 30; nested.leech_damage_bonus = Fixed::from_raw(Fixed::scale);
            if (kind == t::AbilityKind::blast) nested.charged_blast = t::ChargedBlastProfile{};
            unit.hardpoints.resize(4);
            unit.hardpoints[0].type = eawr::units::HardpointType::weapon_laser;
            unit.hardpoints[1].type = unit.hardpoints[2].type = eawr::units::HardpointType::weapon_special;
            unit.hardpoints[3].type = eawr::units::HardpointType::tractor_beam;
            const auto compiled = eawr::units::ability_table(tables);
            expect(static_cast<bool>(compiled), "WHD-01: synthetic hardpoint dependency content compiles");
            expect(compiled && !compiled.value().profiles.empty() && !compiled.value().profiles.front().abilities.empty(),
                "WHD-01: synthetic dependency ability is exposed");
            if (!compiled || compiled.value().profiles.empty() || compiled.value().profiles.front().abilities.empty()) continue;
            const auto& ability = compiled.value().profiles.front().abilities.front();
            const auto expected = kind == t::AbilityKind::tractor_beam ? std::vector<std::uint32_t>{3}
                : kind == t::AbilityKind::energy_weapon ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{1, 2};
            expect(ability.required_hardpoints == expected && ability.requires_hardpoint == (kind != t::AbilityKind::energy_weapon),
                "WHD-01: dependency resolves by type, includes both SPECIALs, excludes unrelated weapons and visual bones");
        }
    }
    mass_driver_type();
    collision_extents();
    if (failures != 0) {
        std::cerr << failures << " unit table contract(s) failed\n";
        return 1;
    }
    std::cout << "unit table contracts passed\n";
    return 0;
}

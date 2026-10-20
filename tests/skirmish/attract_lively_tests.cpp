// The demo attract battle keeps going (docs/behaviour/main-menu-attract.md MAT-15, MAT-16; legacy #3021):
// the stock demo map's placed forces fight under the AI, and a destroyed force's type returns to its
// owner's pool and arrives again. Needs EAWR_EAW_GAME_ROOT (skipped otherwise).
//
//   attract_lively_tests [ticks [workers...]]
//
// The default is 9000 ticks (five minutes) on 1, 2, 4 and 8 workers: the state hashes and the unit
// counts must be equal. With one worker count and a long run (the map cycle is 20 minutes, 36000
// ticks) it also checks that the battle does not run dry. Each run is paired with a control that
// has no pool return, which verifies that recycling remains attract-only. Recycling also feeds
// more combat, so the two battles' final live-unit counts need not be ordered.

#include "../support/env.hpp"
#include "../support/check.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/attract.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

std::optional<std::string> environment(const char* name) {
    return eawr::testing::environment(name);
}

struct Battle final {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    tactical::EconomyRules economy;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::string map;
};

// The viewer's attract start (apps/viewer battle_source_local.cpp), headless: the first map the
// game constants list, its placed forces, the AI player types of MAT-09.
std::optional<Battle> load(const std::filesystem::path& root, const bool pool_return) {
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
    auto settings = skirmish::read_attract_settings(filesystem.value());
    expect(static_cast<bool>(settings) && !settings.value().maps.empty(), "the game constants list a demo map");
    if (!settings || settings.value().maps.empty()) return std::nullopt;
    Battle battle;
    battle.map = skirmish::attract_map_path(settings.value().maps.front());
    skirmish::PlacedForces forces;
    forces.ai = skirmish::attract_ai_players();
    auto fixture = skirmish::placed_fixture(battle.map, std::move(forces), 0, filesystem.value(), catalog.value().catalog);
    expect(static_cast<bool>(fixture),
        "the demo map's placed fixture builds: " + (fixture ? std::string() : fixture.error().message));
    if (!fixture) return std::nullopt;

    eawr::scene::VfsAssetCache cache(filesystem.value());
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.space_map = battle.map;
    input.model = cache.access().model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "unit tables load");
    if (!tables) return std::nullopt;
    auto inputs = skirmish::read_start_inputs(fixture.value(), filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "start inputs read");
    if (!inputs) return std::nullopt;
    std::set<std::string> missing;
    for (const auto& type : skirmish::placed_force_types(inputs.value().placements)) {
        if (!tables.value().find(type)) missing.insert(type);
    }
    if (!missing.empty()) {
        auto extended = input;
        for (const auto type : eawr::units::pinned_m2_types()) extended.types.emplace_back(type);
        extended.types.insert(extended.types.end(), missing.begin(), missing.end());
        for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended.obstacles.emplace_back(obstacle);
        tables = eawr::units::load_unit_tables(extended);
        expect(static_cast<bool>(tables), "the placed types load");
        if (!tables) return std::nullopt;
        inputs.value().tables = &tables.value();
    }
    auto start = skirmish::build_start(fixture.value(), inputs.value());
    expect(static_cast<bool>(start), "the placed start builds: " + (start ? std::string() : start.error().message));
    if (!start) return std::nullopt;
    expect(start.value().setup.attract_pool.has_value(), "MAT-15: the placed start carries the pool-return record");
    start.value().setup.queue_identities = true; // as the live session records (PU-17/63)
    if (!pool_return) start.value().setup.attract_pool.reset(); // the control: nothing returns
    auto content = skirmish::session_content(tables.value(), {});
    expect(static_cast<bool>(content), "session content builds");
    if (!content) return std::nullopt;
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    content.value().map_box = skirmish::map_box(inputs.value());
    auto economy = skirmish::economy_rules(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(economy), "the economy builds: " + (economy ? std::string() : economy.error().message));
    if (!economy) return std::nullopt;
    battle.victory = skirmish::victory_rules(start.value(), tables.value());
    battle.ai = skirmish::ai_setup(start.value(), inputs.value(), tables.value());
    auto enabled = skirmish::enable_goal_system(filesystem.value(), battle.ai);
    expect(static_cast<bool>(enabled), "the goal system loads");
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), battle.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load");
    if (!modules) return std::nullopt;
    battle.modules = std::move(modules).value();
    battle.start = std::move(start).value();
    battle.content = std::move(content).value();
    battle.economy = std::move(economy).value();
    return battle;
}

struct Outcome final {
    std::vector<std::string> hashes;       // the world's state hash at every 600th tick
    std::vector<std::size_t> alive;        // units in the snapshot at every 1800th tick (a minute)
    std::uint64_t destroyed{};
    std::uint64_t returned_reinforcements{}; // accepted reinforce orders
    std::uint64_t rejected_reinforcements{};
    std::uint64_t reinforcements_before_tail{}; // accepted before the last quarter of a full cycle
    std::size_t pool_end{};                // units waiting in the pools at the end
    std::size_t start_units{};
    bool ok{};
};

// A failed step leaves the last committed world: print what could have fed a zero vector (an order
// whose destination is where the unit stands, reinforced units and their orders) and the latest events.
void diagnose(const tactical::TacticalSession& world, const std::vector<tactical::Event>& recent) {
    std::cout << "failed after completed tick " << world.completed_tick() << "; recent events:\n";
    for (const auto& event : recent) {
        std::cout << "  tick " << event.tick << " " << tactical::to_string(event.kind) << " player " << event.player
                  << " unit " << event.unit << " order " << tactical::to_string(event.order) << " reason "
                  << tactical::to_string(event.reason) << '\n';
    }
    const auto planar = [](const eawr::sim::math::Vec3& a, const eawr::sim::math::Vec3& b) {
        const double dx = static_cast<double>(a.x.raw() - b.x.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        const double dy = static_cast<double>(a.y.raw() - b.y.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        return std::sqrt(dx * dx + dy * dy);
    };
    std::size_t shown = 0;
    for (const auto& unit : world.units()) {
        const bool ordered = unit.order.kind != tactical::OrderKind::none;
        const bool reinforced = unit.purchase_token != 0;
        const double gap = planar(unit.order.destination, unit.position);
        // A heading with no planar part: the unit points straight up or down.
        const auto matrix = eawr::sim::math::to_matrix(unit.rotation, eawr::sim::math::Vec3{});
        const bool vertical = matrix && matrix.value().rows[0][0].raw() == 0 && matrix.value().rows[1][0].raw() == 0;
        if (!(reinforced || vertical || (ordered && gap < 2.0)) || ++shown > 40) continue;
        std::cout << "  unit " << unit.entity_id << " type " << unit.type_id << " owner " << unit.owner << " at ("
                  << static_cast<double>(unit.position.x.raw()) / eawr::sim::math::Fixed::scale << ", "
                  << static_cast<double>(unit.position.y.raw()) / eawr::sim::math::Fixed::scale << ") order "
                  << tactical::to_string(unit.order.kind) << " to ("
                  << static_cast<double>(unit.order.destination.x.raw()) / eawr::sim::math::Fixed::scale << ", "
                  << static_cast<double>(unit.order.destination.y.raw()) / eawr::sim::math::Fixed::scale << ") issued "
                  << unit.order.issued_tick << " target " << unit.order.target
                  << (reinforced ? " reinforced" : "") << (vertical ? " vertical" : "") << '\n';
    }
    std::cout << "  arrivals " << world.arrivals().size() << '\n';
}

Outcome run(const Battle& battle, const std::size_t workers, const std::uint64_t ticks) {
    Outcome out;
    auto world = tactical::TacticalSession::create(battle.start.setup, battle.content.sensors, battle.content.durability,
        battle.content.motion, battle.content.fog, battle.content.combat, battle.victory, battle.content.abilities,
        battle.economy, battle.content.map_box);
    expect(static_cast<bool>(world), "the world is created: " + (world ? std::string() : world.error().message));
    if (!world) return out;
    auto session = foc::create_session(std::move(world).value(), battle.ai, battle.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().message));
    if (!session) return out;
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::vector<tactical::Event> recent; // the last few ticks' events, printed when a step fails
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.value().step(executor);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) {
            diagnose(session.value().world(), recent);
            return out;
        }
        for (const auto& event : stepped.value().world.snapshot->events()) {
            if (event.kind == tactical::EventKind::order_accepted || event.kind == tactical::EventKind::order_rejected
                || event.kind == tactical::EventKind::unit_destroyed || event.kind == tactical::EventKind::reinforcement_unloaded)
                recent.push_back(event);
        }
        if (recent.size() > 48) recent.erase(recent.begin(), recent.end() - 48);
        const auto& snapshot = *stepped.value().world.snapshot;
        if (tick == 0) out.start_units = snapshot.instances().size();
        for (const auto& event : snapshot.events()) {
            if (event.kind == tactical::EventKind::unit_destroyed) ++out.destroyed;
            if (event.order != tactical::OrderKind::reinforce) continue;
            if (event.kind == tactical::EventKind::order_accepted) ++out.returned_reinforcements;
            if (event.kind == tactical::EventKind::order_rejected) ++out.rejected_reinforcements;
        }
        if (tick + 1 == ticks - ticks / 4) out.reinforcements_before_tail = out.returned_reinforcements;
        if ((tick + 1) % 600 == 0) out.hashes.push_back(stepped.value().world.state_sha256);
        if ((tick + 1) % 1800 == 0) out.alive.push_back(snapshot.instances().size());
    }
    for (const auto& ledger : session.value().world().ledgers()) out.pool_end += ledger.pool.size();
    out.ok = true;
    return out;
}

void print(const char* label, const Outcome& outcome) {
    std::cout << label << ": " << outcome.start_units << " units at the start, destroyed " << outcome.destroyed
              << ", reinforce orders accepted " << outcome.returned_reinforcements << " rejected "
              << outcome.rejected_reinforcements << ", pooled at the end " << outcome.pool_end << ", per minute:";
    for (const auto count : outcome.alive) std::cout << ' ' << count;
    std::cout << "; reinforcements in the last quarter "
              << outcome.returned_reinforcements - outcome.reinforcements_before_tail << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the attract battle\n";
        return 0;
    }
    std::uint64_t ticks = 9000;
    if (argc > 1) ticks = std::strtoull(argv[1], nullptr, 10);
    std::vector<std::size_t> worker_counts;
    for (int index = 2; index < argc; ++index) worker_counts.push_back(std::strtoull(argv[index], nullptr, 10));
    if (worker_counts.empty()) worker_counts = {1, 2, 4, 8};

    // The control first: a failure there is not the pool return's.
    const auto control = load(*root, false);
    if (!control) return 1;
    const auto dry = run(*control, worker_counts.front(), ticks);
    if (dry.ok) print("control", dry);
    expect(dry.ok, "the control battle (no pool return) runs the whole way");

    const auto lively = load(*root, true);
    if (!lively) return 1;
    std::optional<Outcome> reference;
    for (const auto workers : worker_counts) {
        auto outcome = run(*lively, workers, ticks);
        if (!outcome.ok) {
            expect(false, "the pool-return battle runs the whole way on " + std::to_string(workers) + " workers");
            break;
        }
        if (!reference) {
            print("pool return", outcome);
            reference = outcome;
        } else {
            expect(outcome.hashes == reference->hashes && outcome.alive == reference->alive
                    && outcome.destroyed == reference->destroyed
                    && outcome.returned_reinforcements == reference->returned_reinforcements
                    && outcome.rejected_reinforcements == reference->rejected_reinforcements
                    && outcome.reinforcements_before_tail == reference->reinforcements_before_tail
                    && outcome.pool_end == reference->pool_end && outcome.start_units == reference->start_units,
                "MAT-15: the battle is equal on " + std::to_string(workers) + " workers");
        }
    }
    if (reference) {
        expect(reference->destroyed > 0, "the placed forces fight and lose units");
        expect(reference->returned_reinforcements > 0,
            "MAT-15/16: the AI reinforces from the returned pool with no allied star base required");
    }
    if (dry.ok) {
        expect(dry.returned_reinforcements == 0 && dry.pool_end == 0, "WPR-23: outside the attract nothing is returned");
        if (reference && !reference->alive.empty() && !dry.alive.empty()) {
            // MAT-15/16 promises returned forces and continued reinforcement, not a larger endpoint
            // population than a different battle. Reinforcements can enable additional fighter launches
            // and combat losses in either battle. Check that recycling is still active late in the cycle.
            if (ticks >= 36000) {
                expect(reference->returned_reinforcements > reference->reinforcements_before_tail,
                    "MAT-15/16: returned forces still reinforce in the last quarter of the cycle");
            }
            // The floor: the battle that refills still has, after the run, at least a quarter of what
            // started it.
            expect(reference->alive.back() * 4 >= reference->start_units,
                "MAT-15: the unit count stays above a quarter of the start after " + std::to_string(ticks / 1800) + " minutes");
        }
    }
    if (failures != 0) {
        std::cerr << failures << " attract battle failure(s)\n";
        return 1;
    }
    std::cout << "attract battle: PASS\n";
    return 0;
}

// The Empire AI's attack on the starbase after it has destroyed the Rebel fleet (#532,
// docs/behaviour/foc-tactical-ai.md AI-24, FH-20, PL-45). The Rebel player sends every mobile
// unit of the M2 fleet without the MC80 at the Empire station and loses them all. Once the game is 180 s old and the Rebel
// fighter, bomber, corvette and frigate force is below 500 (the last craft may still be alive),
// the retail burn plan starts: it abandons the other plans (PL-45), sleeps 1 s, collects the free
// units and attack-moves them at Find_Nearest's "Structure | Capital" (a category mask, FH-20),
// which is the Rebel starbase, and they hit it after the Rebel fleet is gone. The same journal,
// diagnostics and hashes come out on 1, 2, 4 and 8 workers (ADR-009).
//
//   foc_burn_tests [seed [ticks [timeline.csv]]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//
// timeline.csv is the one-worker run's plan journal (tick, player, plan, goal, target, event, detail).

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
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

struct Battle {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
};

eawr::sim::EntityId rebel_station_id(const Battle& battle) {
    for (const auto& unit : battle.start.units) {
        if (unit.role == skirmish::UnitRole::station && unit.type == "Skirmish_Rebel_Star_Base_1") return unit.state.entity_id;
    }
    return {};
}

struct Run {
    std::vector<std::string> hashes;
    std::vector<std::string> journal;
    std::set<std::string> diagnostics;
    std::uint64_t last_loss{};            // the last Rebel ship or craft destroyed
    std::uint64_t three_near{};           // first tick after the wipe with three Empire units near the Rebel station
    std::uint64_t first_hit{};            // first hit on the Rebel station after the wipe
    std::optional<std::int64_t> power_at_burn;
    std::set<eawr::sim::EntityId> empire_ships_at_order; // Empire ships (not squadrons) alive at the first burn order
    // #669 (space-damage DG-39): each death of a unit with destroyable hardpoints as it stood the
    // tick before, and how many died with over 0.2 of their hardpoint health left.
    std::vector<std::string> hardpoint_deaths;
    std::size_t deaths_with_hardpoints_standing{};
    foc::AiJournal plans;
};

// The retail stage's "near" (docs/foc-original-capture.md, pause): within 3000 units of the station.
constexpr std::int64_t near_range = 3000;

std::optional<Run> run(const Battle& battle, std::size_t workers, std::uint64_t ticks) {
    auto world = tactical::TacticalSession::create(battle.start.setup, battle.content.sensors, battle.content.durability,
        battle.content.motion, battle.content.fog, battle.content.combat, battle.victory);
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    foc::AiSetup setup = battle.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, battle.modules);
    expect(static_cast<bool>(session), "the AI session is created");
    if (!session) return std::nullopt;

    eawr::sim::EntityId rebel_station{};
    eawr::sim::EntityId empire_station{};
    eawr::sim::math::Vec3 rebel_station_at{};
    tactical::PlayerId rebel{};
    tactical::PlayerId empire{};
    for (const auto& unit : battle.start.units) {
        if (unit.role != skirmish::UnitRole::station) continue;
        if (unit.type == "Skirmish_Rebel_Star_Base_1") {
            rebel_station = unit.state.entity_id;
            rebel_station_at = unit.state.position;
            rebel = unit.state.owner;
        } else {
            empire_station = unit.state.entity_id;
            empire = unit.state.owner;
        }
    }
    expect(rebel_station != 0 && empire_station != 0, "both stations start");
    const auto& squadrons = battle.content.motion.squadrons;
    std::map<eawr::sim::EntityId, std::pair<std::string, double>> standing; // #669: last tick's units with hardpoints
    // AI-24: the Rebel fighter, bomber, corvette and frigate force (AI_Combat_Power, before the
    // perception's health attenuation) when the burn plan starts.
    std::uint64_t mobile_bits = 0;
    for (const char* name : {"FIGHTER", "BOMBER", "CORVETTE", "FRIGATE"}) {
        if (const auto bit = setup.content.categories.find(name); bit != setup.content.categories.end()) mobile_bits |= bit->second;
    }
    std::uint64_t structure_bits = 0;
    if (const auto bit = setup.content.categories.find("STRUCTURE"); bit != setup.content.categories.end()) structure_bits = bit->second;
    std::map<tactical::TypeId, std::int64_t> mobile_power;
    std::set<tactical::TypeId> structures;
    for (const auto& type : setup.content.types) {
        if ((type.category_bits & mobile_bits) != 0) mobile_power[type.type_id] = static_cast<std::int64_t>(type.combat_power);
        if ((type.category_bits & structure_bits) != 0) structures.insert(type.type_id);
    }

    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run out;
    std::uint64_t sequence = 1;
    std::set<eawr::sim::EntityId> ordered;
    std::set<eawr::sim::EntityId> rebel_alive; // ships and craft
    std::size_t journal_seen = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        // The Rebel player attack-moves every new ship and squadron at the Empire station.
        std::vector<tactical::PlayerCommand> commands;
        const auto snapshot = session.value().world().snapshot();
        std::vector<eawr::sim::EntityId> fresh;
        for (const auto& instance : snapshot->instances()) {
            if (instance.owner != rebel || instance.entity_id == rebel_station) continue;
            if (squadrons.find_squadron(instance.type_id) == nullptr) rebel_alive.insert(instance.entity_id);
            if (squadrons.find_craft(instance.type_id) != nullptr) continue;
            if (tick % 30 == 1 && !ordered.contains(instance.entity_id)) fresh.push_back(instance.entity_id);
        }
        if (!fresh.empty()) {
            tactical::PlayerCommand command;
            command.key = {tick + 1, rebel, sequence++};
            command.units = fresh;
            command.payload = tactical::AttackMovePayload{{}, empire_station};
            commands.push_back(command);
            ordered.insert(fresh.begin(), fresh.end());
        }
        auto stepped = session.value().step(executor, commands);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1));
        if (!stepped) return std::nullopt;
        const auto& result = stepped.value();
        out.hashes.push_back(result.state_sha256);
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind != tactical::EventKind::unit_destroyed) continue;
            if (const auto found = standing.find(event.unit); found != standing.end()) {
                out.hardpoint_deaths.push_back(std::to_string(event.tick) + " unit " + std::to_string(event.unit) + ' ' + found->second.first);
                out.deaths_with_hardpoints_standing += found->second.second > 0.2 ? 1 : 0;
            }
            if (rebel_alive.erase(event.unit) != 0) out.last_loss = event.tick;
        }
        standing.clear();
        for (const auto& instance : result.world.snapshot->instances()) {
            const auto* profile = battle.content.durability.find(instance.type_id);
            if (!instance.durability || profile == nullptr) continue;
            std::int64_t total = 0;
            std::int64_t left = 0;
            std::size_t alive = 0;
            std::size_t count = 0;
            for (std::size_t index = 0; index < profile->hardpoints.size() && index < instance.durability->hardpoints.size(); ++index) {
                if (!profile->hardpoints[index].destroyable) continue;
                ++count;
                total += profile->hardpoints[index].max_health.raw();
                left += instance.durability->hardpoints[index].health.raw();
                alive += instance.durability->hardpoints[index].health.raw() > 0 ? 1 : 0;
            }
            if (count == 0 || total <= 0) continue;
            const double share = static_cast<double>(left) / static_cast<double>(total);
            const auto hull = instance.durability->hull.raw() >> 24;
            standing[instance.entity_id] = {"type " + std::to_string(instance.type_id) + ": hull " + std::to_string(hull) + ", "
                    + std::to_string(alive) + " of " + std::to_string(count) + " hardpoints alive, "
                    + std::to_string(static_cast<int>(share * 100)) + " % of their health",
                share};
        }
        const bool wiped = rebel_alive.empty() && out.last_loss != 0;
        for (; journal_seen < setup.journal->plans.size(); ++journal_seen) {
            const auto& event = setup.journal->plans[journal_seen];
            if (event.plan == "burnunits" && event.event == "order" && out.empire_ships_at_order.empty()) {
                for (const auto& instance : result.world.snapshot->instances()) {
                    if (instance.owner != empire || instance.entity_id == empire_station || structures.contains(instance.type_id)) continue;
                    if (squadrons.find_squadron(instance.type_id) != nullptr || squadrons.find_craft(instance.type_id) != nullptr) continue;
                    out.empire_ships_at_order.insert(instance.entity_id);
                }
            }
            if (event.plan != "burnunits" || event.event != "started" || out.power_at_burn) continue;
            std::int64_t power = 0;
            for (const auto& instance : result.world.snapshot->instances()) {
                if (instance.owner != rebel) continue;
                if (const auto found = mobile_power.find(instance.type_id); found != mobile_power.end()) power += found->second;
            }
            out.power_at_burn = power;
        }
        if (!wiped) continue;
        if (out.three_near == 0) {
            std::size_t near = 0;
            for (const auto& instance : result.world.snapshot->instances()) {
                if (instance.owner != empire || structures.contains(instance.type_id)) continue;
                if (squadrons.find_squadron(instance.type_id) != nullptr) continue;
                const std::int64_t dx = (instance.fixed_transform.rows[0][3].raw() - rebel_station_at.x.raw()) >> 24;
                const std::int64_t dy = (instance.fixed_transform.rows[1][3].raw() - rebel_station_at.y.raw()) >> 24;
                if (dx * dx + dy * dy <= near_range * near_range) ++near;
            }
            if (near >= 3) out.three_near = tick + 1;
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::projectile_hit && event.target == rebel_station && out.first_hit == 0) {
                out.first_hit = event.tick;
            }
        }
    }
    out.plans = *setup.journal;
    for (const auto& event : setup.journal->plans) {
        out.journal.push_back(std::to_string(event.tick) + ',' + std::to_string(event.player) + ',' + event.plan + ',' + event.goal + ',' +
            event.target + ',' + event.event + ",\"" + event.detail + '"');
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC burn battle\n";
        return 0;
    }
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2;
    const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10200;

    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return 1;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return 1;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return 1;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return 1;
    auto fixture = skirmish::m2_fixture();
    fixture.seed = seed;
    // The battle needs a Rebel fleet the Empire destroys: without the MC80 (#537). With it, the MC80
    // is a Capital, so AI-24's fighter, bomber, corvette and frigate force ignores it and the burn plan's
    // "Structure | Capital" search may answer it instead of the starbase (FH-20), as in FoC; in seed 6
    // it and the frigate outlive the Empire station instead. With the ships at their Layer_Z_Adjust
    // heights (#666) and the idle grid (#687) seeds 6 and 1 no longer bring three Empire units to the
    // starbase together; seed 2 plays the scenario.
    for (auto& slot : fixture.slots) {
        if (slot.faction == "Rebel") std::erase(slot.fleet, std::string("Calamari_Cruiser"));
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return 1;
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds");
    if (!start) return 1;
    auto content = skirmish::session_content(tables.value());
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return 1;
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return 1;
    content.value().fog = fog.value();
    auto ai = skirmish::ai_setup(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(skirmish::enable_goal_system(filesystem.value(), ai)), "the goal system's XML loads");
    auto modules = skirmish::ai_modules(filesystem.value(), ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load");
    if (!modules || failures != 0) return 1;
    const Battle battle{start.value(), content.value(), skirmish::victory_rules(start.value(), tables.value()), ai, modules.value()};

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(battle, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): final " << result->hashes.back() << '\n';
        runs.push_back(std::move(result).value());
    }
    const Run& first = runs.front();
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].journal == first.journal, "the plan journal matches worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }
    if (argc > 3) {
        std::ofstream out(argv[3], std::ios::binary);
        out << "tick,player,plan,goal,target,event,detail\n";
        for (const auto& line : first.journal) out << line << '\n';
    }

    std::optional<std::uint64_t> burn_start;
    std::optional<std::uint64_t> burn_order;
    std::string burn_order_detail;
    std::size_t abandoned = 0;
    for (const auto& event : first.plans.plans) {
        if (event.plan == "burnunits" && event.event == "started" && !burn_start) burn_start = event.tick;
        if (event.plan == "burnunits" && event.event == "order" && !burn_order) {
            burn_order = event.tick;
            burn_order_detail = event.detail;
        }
        if (burn_start && event.event == "abandoned" && event.tick < *burn_start + 30) ++abandoned;
    }
    const auto seconds = [](std::uint64_t tick) { return std::to_string(tick / 30) + '.' + std::to_string(tick % 30 * 10 / 30) + " s"; };
    std::cout << "seed " << seed << ": last Rebel loss " << seconds(first.last_loss) << ", burn plan " << (burn_start ? seconds(*burn_start) : "-")
              << " (" << abandoned << " plans abandoned), first order " << (burn_order ? seconds(*burn_order) : "-") << " ("
              << burn_order_detail << "), Rebel mobile force then " << (first.power_at_burn ? std::to_string(*first.power_at_burn) : "-")
              << "; after the wipe: three Empire units near the starbase " << (first.three_near ? seconds(first.three_near) : "-")
              << ", first hit " << (first.first_hit ? seconds(first.first_hit) : "-") << '\n';
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    for (const auto& death : first.hardpoint_deaths) std::cout << "destroyed: " << death << '\n';

    expect(first.last_loss != 0, "the Empire destroys every Rebel ship and craft");
    // AI-24: the burn trigger needs a game age above 180 s (5,400 ticks) and a Rebel fighter, bomber,
    // corvette and frigate force below 500 after the health attenuation; before it, the fleet
    // (5,325 with every launch) must be mostly gone.
    expect(burn_start.has_value() && *burn_start > 5400, "the burn plan starts after 180 s");
    expect(first.power_at_burn.has_value() && *first.power_at_burn < 1000, "the burn plan starts once the Rebel fleet is nearly gone");
    // PL-45: Purge_Goals abandons the other running plans.
    expect(abandoned > 0, "the burn plan abandons the other plans");
    // FH-20: Find_Nearest(MainForce, "Structure | Capital", ...) finds the Rebel starbase; the plan
    // attack-moves every collected unit there after its one-second sleep.
    const std::string station = "attack_move object " + std::to_string(rebel_station_id(battle)) + " [";
    expect(burn_order && burn_start && *burn_order <= *burn_start + 60 && burn_order_detail.find(station) != std::string::npos,
        "the burn plan attack-moves its units at the Rebel starbase within 2 s");
    // Collect_All_Free_Units after Purge_Goals: every Empire ship alive then, and squadrons. A
    // squadron may stay with a plan the purge keeps (PL-45): the retail bombing run makes itself
    // unremovable while it attacks, and in seed 6 its bombers and escort are on the starbase.
    const auto bracketed = [](const std::string& detail) {
        std::set<eawr::sim::EntityId> ids;
        if (const auto open = detail.find('['); open != std::string::npos) {
            for (std::size_t at = open + 1; at < detail.size() && detail[at] != ']';) {
                char* end = nullptr;
                ids.insert(static_cast<eawr::sim::EntityId>(std::strtoull(detail.c_str() + at, &end, 10)));
                at = static_cast<std::size_t>(end - detail.c_str()) + 1;
            }
        }
        return ids;
    };
    const auto ordered_units = bracketed(burn_order_detail);
    // A ship too may stay with a plan the purge keeps: the retail turbo attack on a location makes
    // its force unremovable (PL-45). The units such a plan produced, while it still runs at the
    // burn plan's order, are not free.
    std::map<std::string, std::set<eawr::sim::EntityId>> held_by_running_plan;
    for (const auto& event : first.plans.plans) {
        if (!burn_order || event.tick > *burn_order || event.plan == "burnunits") continue;
        const auto key = std::to_string(event.player) + '/' + event.plan + '/' + event.goal + '/' + event.target;
        if (event.event == "started") held_by_running_plan[key].clear();
        if (event.event == "produced") held_by_running_plan[key] = bracketed(event.detail);
        if (event.event == "finished" || event.event == "failed" || event.event == "abandoned") held_by_running_plan.erase(key);
    }
    std::set<eawr::sim::EntityId> kept;
    for (const auto& [key, ids] : held_by_running_plan) kept.insert(ids.begin(), ids.end());
    std::string missing;
    for (const auto id : first.empire_ships_at_order) {
        if (!ordered_units.contains(id) && !kept.contains(id)) missing += ' ' + std::to_string(id);
    }
    std::size_t free_ships = 0;
    for (const auto id : first.empire_ships_at_order) free_ships += kept.contains(id) ? 0 : 1;
    expect(!first.empire_ships_at_order.empty() && missing.empty() && ordered_units.size() > free_ships,
        "the burn plan collects every Empire ship and free squadrons; ships not ordered:" + missing);
    for (const auto& diagnostic : first.diagnostics) {
        expect(diagnostic.find("Find_Nearest") == std::string::npos, "no Find_Nearest filter is rejected: " + diagnostic);
    }
    expect(first.three_near > first.last_loss, "three Empire units come near the Rebel starbase after the wipe");
    expect(first.first_hit > first.last_loss, "the Empire hits the Rebel starbase after the wipe");
    if (failures != 0) {
        std::cerr << failures << " FoC burn check(s) failed\n";
        return 1;
    }
    std::cout << "FoC burn contracts passed\n";
    return 0;
}

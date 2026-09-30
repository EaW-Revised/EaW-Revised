// The FoC goal system in the M2 battle (#449, docs/behaviour/foc-tactical-ai.md "Goal system",
// "Plans and TaskForces"): with the retail space AI XML loaded, the Empire AI's goal system
// proposes goals, draws plans and runs them beside the freestore. The run must start named
// plans that issue orders, give the same state hashes, commands and plan journal on 1, 2, 4 and
// 8 workers (ADR-009), replay headless from its record, and keep the Lua cost per tick in
// budget. The live session hosts the same AI. The bombing run's squadrons fly to their fogged
// target and fire at it (#633); their trace (target, distance, whether the Empire sees the
// object, every 240 ticks from the order) is printed.
//
//   foc_plan_tests [ticks [timeline.csv [costs.csv]]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//
// timeline.csv is the one-worker run's plan journal (tick, player, plan, goal, target, event,
// detail); costs.csv its Lua cost per tick. Both feed the eye-check material.

#include "eawr/data/xml.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace auth = eawr::script::authoritative;
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

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<tactical::TypeId, std::string> names; // #498: unit type names for the report
};

// #498: what each unit type of the battle did: the orders it accepted by issuing player, and its
// hits, the first one's tick, where its shot left and the type it struck, and how close any unit of
// the type came to an enemy start station (whether it advanced).
struct TypeActivity {
    std::map<std::uint32_t, std::size_t> orders;
    std::size_t hits{};
    std::uint64_t first_hit{};
    eawr::sim::math::Vec3 first_from{};
    tactical::TypeId first_target{};
    std::optional<double> closest{};
};

struct Run {
    std::vector<std::string> hashes;
    std::vector<std::string> world_hashes;
    std::vector<std::string> commands;
    std::set<std::string> diagnostics;
    foc::AiJournal journal;
    double seconds{};
    tactical::TacticalReplay record;
    // #452: whether the plans close and fight: projectile hits by the start units' owners, the
    // tick of the first, and the units destroyed.
    std::map<std::uint32_t, std::uint64_t> hits;
    std::uint64_t first_hit{};
    std::uint64_t destroyed{};
    std::map<tactical::TypeId, TypeActivity> by_type;
    // #633: the squadrons a plan ordered to attack an object (bombingrun's BomberForce and
    // FighterForce), their target and team position per tick from the order, and the first hit a
    // craft of each landed on the ordered object.
    std::map<eawr::sim::EntityId, eawr::sim::EntityId> ordered_squadrons; // container -> ordered target
    std::map<eawr::sim::EntityId, std::uint64_t> ordered_tick;
    std::map<eawr::sim::EntityId, std::vector<std::string>> squadron_trace;
    std::map<eawr::sim::EntityId, std::uint64_t> squadron_first_hit;
    std::map<eawr::sim::EntityId, std::uint64_t> squadron_first_shot; // first weapon fired at the ordered object
    std::map<eawr::sim::EntityId, double> order_distance;             // planar, team to object, at the order
    // Each tick while it lives: the team's planar distance to the ordered object; then the tick it
    // is gone. Whether the plan ordered it as the BomberForce.
    std::map<eawr::sim::EntityId, double> last_distance;
    std::map<eawr::sim::EntityId, double> closest_seen; // nearest while its owner saw the object
    std::map<eawr::sim::EntityId, std::uint64_t> gone_tick;
    std::map<eawr::sim::EntityId, bool> bomber_force;
    std::map<eawr::sim::EntityId, std::map<eawr::sim::EntityId, std::size_t>> squadron_hits; // container -> target -> hits
};

// #633: "BomberForce attack object 5 [73,78,97]" -> (5, {73, 78, 97}).
std::optional<std::pair<eawr::sim::EntityId, std::vector<eawr::sim::EntityId>>> attack_order(const std::string& detail) {
    const std::string marker = " attack object ";
    const auto at = detail.find(marker);
    const auto open = detail.find('[');
    if (at == std::string::npos || open == std::string::npos) return std::nullopt;
    std::pair<eawr::sim::EntityId, std::vector<eawr::sim::EntityId>> out;
    out.first = std::strtoull(detail.c_str() + at + marker.size(), nullptr, 10);
    for (std::size_t index = open + 1; index < detail.size() && detail[index] != ']';) {
        char* end = nullptr;
        out.second.push_back(std::strtoull(detail.c_str() + index, &end, 10));
        index = static_cast<std::size_t>(end - detail.c_str()) + 1;
    }
    return out;
}

std::string line_of(const foc::PlanEvent& event) {
    std::string detail;
    for (const char item : event.detail) detail += item == '"' ? std::string("\"\"") : std::string(1, item);
    return std::to_string(event.tick) + "," + std::to_string(event.player) + "," + event.plan + "," + event.goal + "," +
        event.target + "," + event.event + ",\"" + detail + "\"";
}

std::optional<Content> load(const std::filesystem::path& root) {
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
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    expect(inputs.value().map_extents.has_value(), "PG-01: the M2 map declares its extents");
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds");
    if (!start) return std::nullopt;
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    // #495: the goal-system battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}};
    for (const auto& type : tables.value().units) out.names.emplace(skirmish::type_id(type.id), type.id);
    expect(out.ai.bounds.has_value(), "the AI setup has the map bounds");
    // PL-13: a squadron type takes its craft's categories and the sum of their power.
    const auto bomber = out.ai.content.categories.find("BOMBER");
    const auto squadron = std::find_if(out.ai.content.types.begin(), out.ai.content.types.end(),
        [](const foc::AiType& type) { return type.name == "TIE_BOMBER_SQUADRON"; });
    expect(bomber != out.ai.content.categories.end() && squadron != out.ai.content.types.end()
            && (squadron->category_bits & bomber->second) != 0 && squadron->combat_power == eawr::script::numeric::LuaNumber(240),
        "PL-13: a TIE bomber squadron is a Bomber with its 4 craft's power (240)");
    auto enabled = skirmish::enable_goal_system(filesystem.value(), out.ai);
    expect(static_cast<bool>(enabled), "the goal system's XML loads: " + (enabled ? std::string() : enabled.error().message));
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load: " + (modules ? std::string() : modules.error().message));
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    return out;
}

std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities);
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    foc::AiSetup setup = content.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().code + " " + session.error().message));
    if (!session) return std::nullopt;
    std::map<eawr::sim::EntityId, std::uint32_t> owners;
    for (const auto& unit : content.start.units) owners.emplace(unit.state.entity_id, unit.state.owner);
    std::map<eawr::sim::EntityId, tactical::TypeId> types;
    std::vector<std::pair<std::uint32_t, eawr::sim::math::Vec3>> stations;
    for (const auto& unit : content.start.units) {
        if (unit.role == skirmish::UnitRole::station) stations.emplace_back(unit.state.owner, unit.state.position);
    }
    const auto planar = [](const eawr::sim::math::Vec3& a, const eawr::sim::math::Vec3& b) {
        const double dx = static_cast<double>(a.x.raw() - b.x.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        const double dy = static_cast<double>(a.y.raw() - b.y.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        return std::sqrt(dx * dx + dy * dy);
    };
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::size_t journal_seen = 0; // #633: the plan journal read so far
    Run out;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.value().step(executor);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        const auto& result = stepped.value();
        out.hashes.push_back(result.state_sha256);
        out.world_hashes.push_back(result.world.state_sha256);
        for (const auto& instance : result.world.snapshot->instances()) {
            types.emplace(instance.entity_id, instance.type_id);
            const eawr::sim::math::Vec3 at{instance.fixed_transform.rows[0][3], instance.fixed_transform.rows[1][3],
                instance.fixed_transform.rows[2][3]};
            for (const auto& [station_owner, station] : stations) {
                if (station_owner == instance.owner) continue;
                auto& closest = out.by_type[instance.type_id].closest;
                const double distance = planar(at, station);
                if (!closest || distance < *closest) closest = distance;
            }
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind != tactical::EventKind::order_accepted) continue;
            if (const auto type = types.find(event.unit); type != types.end()) ++out.by_type[type->second].orders[event.player];
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            if (const auto type = types.find(event.shooter); type != types.end()) {
                auto& activity = out.by_type[type->second];
                ++activity.hits;
                if (activity.first_hit == 0) {
                    activity.first_hit = event.tick;
                    activity.first_from = event.origin;
                    if (const auto target = types.find(event.target); target != types.end()) activity.first_target = target->second;
                }
            }
            const auto owner = owners.find(event.shooter);
            if (owner == owners.end()) continue;
            ++out.hits[owner->second];
            if (out.first_hit == 0) out.first_hit = event.tick;
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind == tactical::EventKind::unit_destroyed) ++out.destroyed;
        }
        // #633: the bombing run's ordered squadrons, traced from the plan's order.
        for (; journal_seen < setup.journal->plans.size(); ++journal_seen) {
            const auto& event = setup.journal->plans[journal_seen];
            if (event.plan != "bombingrun" || event.event != "order") continue;
            if (const auto order = attack_order(event.detail)) {
                for (const auto unit : order->second) {
                    out.ordered_squadrons[unit] = order->first;
                    out.ordered_tick.emplace(unit, event.tick);
                    out.bomber_force.emplace(unit, event.detail.starts_with("BomberForce"));
                }
            }
        }
        if (!out.ordered_squadrons.empty()) {
            const auto& snapshot = *result.world.snapshot;
            std::map<eawr::sim::EntityId, eawr::sim::EntityId> craft_squadron;
            for (const auto& squadron : snapshot.squadrons()) {
                for (const auto member : squadron.members) craft_squadron.emplace(member, squadron.container);
            }
            const auto find = [&snapshot](const eawr::sim::EntityId id) -> const tactical::TacticalInstance* {
                for (const auto& instance : snapshot.instances()) {
                    if (instance.entity_id == id) return &instance;
                }
                return nullptr;
            };
            for (const auto& [container, target] : out.ordered_squadrons) {
                const auto now = snapshot.completed_tick();
                if (now < out.ordered_tick.at(container)) continue;
                const auto* self = find(container);
                const auto* goal = find(target);
                if (self != nullptr) {
                    if (goal != nullptr) {
                        const double distance = planar(
                            eawr::sim::math::Vec3{self->fixed_transform.rows[0][3], self->fixed_transform.rows[1][3], {}},
                            eawr::sim::math::Vec3{goal->fixed_transform.rows[0][3], goal->fixed_transform.rows[1][3], {}});
                        out.last_distance[container] = distance;
                        std::size_t index = 0;
                        for (; index < snapshot.players().size() && snapshot.players()[index].player_id != self->owner; ++index) {}
                        if (((goal->visible_to >> index) & 1U) != 0U) {
                            const auto seen = out.closest_seen.find(container);
                            if (seen == out.closest_seen.end() || distance < seen->second) out.closest_seen[container] = distance;
                        }
                    }
                } else {
                    out.gone_tick.emplace(container, now);
                }
                if ((now - out.ordered_tick.at(container)) % 240 != 0) continue;
                eawr::sim::EntityId held = eawr::sim::invalid_entity_id;
                for (const auto& entry : snapshot.squadron_targets()) {
                    if (entry.squadron == container) held = entry.target;
                }
                std::string line = "t" + std::to_string(now) + " target " + std::to_string(held);
                if (self != nullptr && goal != nullptr) {
                    const eawr::sim::math::Vec3 at{self->fixed_transform.rows[0][3], self->fixed_transform.rows[1][3], {}};
                    const eawr::sim::math::Vec3 there{goal->fixed_transform.rows[0][3], goal->fixed_transform.rows[1][3], {}};
                    std::size_t index = 0;
                    for (; index < snapshot.players().size() && snapshot.players()[index].player_id != self->owner; ++index) {}
                    const double distance = planar(at, there);
                    out.order_distance.emplace(container, distance);
                    line += " distance " + std::to_string(static_cast<long long>(distance)) + " seen "
                        + (((goal->visible_to >> index) & 1U) != 0U ? "yes" : "no");
                } else {
                    line += self == nullptr ? " (squadron gone)" : " (target gone)";
                }
                out.squadron_trace[container].push_back(line);
            }
            for (const auto& event : snapshot.combat_events()) {
                if (event.kind == tactical::CombatEventKind::weapon_fired) {
                    const auto squadron = craft_squadron.find(event.shooter);
                    if (squadron == craft_squadron.end()) continue;
                    const auto ordered = out.ordered_squadrons.find(squadron->second);
                    if (ordered != out.ordered_squadrons.end() && ordered->second == event.target) {
                        out.squadron_first_shot.emplace(squadron->second, event.tick);
                    }
                    continue;
                }
                if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
                const auto squadron = craft_squadron.find(event.shooter);
                if (squadron == craft_squadron.end()) continue;
                const auto ordered = out.ordered_squadrons.find(squadron->second);
                if (ordered != out.ordered_squadrons.end()) ++out.squadron_hits[squadron->second][event.target];
                if (ordered != out.ordered_squadrons.end() && ordered->second == event.target) {
                    out.squadron_first_hit.emplace(squadron->second, event.tick);
                }
            }
        }
        for (const auto& routed : result.script_input) {
            out.commands.push_back(routed.command.verb + " i" + std::to_string(routed.command.issuer) + " " +
                (routed.submitted ? "ok" : "dropped " + routed.dropped.message));
        }
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    out.record = session.value().record();
    out.journal = *setup.journal;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC plan battle\n";
        return 0;
    }
    const std::uint64_t ticks = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5400;
    auto content = load(*root);
    if (!content) return 1;

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): " << ticks << " ticks in " << result->seconds << " s, final "
                  << result->hashes.back() << '\n';
        runs.push_back(std::move(*result));
    }
    const Run& first = runs.front();
    std::vector<std::string> timeline;
    for (const auto& event : first.journal.plans) timeline.push_back(line_of(event));
    for (std::size_t index = 1; index < runs.size(); ++index) {
        std::vector<std::string> other;
        for (const auto& event : runs[index].journal.plans) other.push_back(line_of(event));
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].commands == first.commands, "the script commands match worker count 1");
        expect(other == timeline, "the plan journal matches worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }
    if (argc > 2) {
        std::ofstream out(argv[2], std::ios::binary);
        out << "tick,player,plan,goal,target,event,detail\n";
        for (const auto& line : timeline) out << line << '\n';
    }
    if (argc > 3) {
        std::ofstream out(argv[3], std::ios::binary);
        out << "tick,freestore_instructions,plan_instructions,plan_instances\n";
        for (const auto& cost : first.journal.costs) {
            out << cost.tick << ',' << cost.freestore_instructions << ',' << cost.plan_instructions << ',' << cost.plan_instances << '\n';
        }
    }

    // Named plans start and issue orders.
    std::map<std::string, std::size_t> started;
    std::map<std::string, std::size_t> ordered;
    std::map<std::string, std::size_t> finished;
    for (const auto& event : first.journal.plans) {
        if (event.event == "started") ++started[event.plan];
        if (event.event == "order") ++ordered[event.plan];
        if (event.event == "finished") ++finished[event.plan];
    }
    for (const auto& [plan, count] : started) {
        std::cout << "plan " << plan << ": started " << count << ", orders " << ordered[plan] << ", finished " << finished[plan] << '\n';
    }
    expect(!started.empty(), "the goal system starts plans");
    // #495: on the real map bounds (PG-01) the Empire's first attack is TURBO_ATTACK_LOCATION on
    // the Rebel corner's threat cell (tick 378); DESTROY_UNIT follows at tick 503.
    expect(started.contains("destroyunit") || started.contains("flankplan") || started.contains("destroyunitminimal")
            || started.contains("turboattacklocation"),
        "an attack plan (destroyunit, destroyunitminimal, flankplan or turboattacklocation) starts");
    // #529 (PL-11, PL-13): TIE squadrons fill Fighter and Bomber teams, so the Empire escorts a
    // ship with fighters. The area sweep's "Fighter | Bomber | Corvette" search is a category
    // mask (FH-20, #532): no Find_Nearest filter is rejected, and the sweep orders its squadrons.
    expect(started.contains("escortplan"), "an escort (escortplan) starts with TIE squadrons");
    expect(started.contains("areasweep") && ordered["areasweep"] > 0, "an area sweep (areasweep) starts and orders its squadrons");
    for (const auto& diagnostic : first.diagnostics) {
        expect(diagnostic.find("Find_Nearest") == std::string::npos, "no Find_Nearest filter is rejected: " + diagnostic);
    }
    std::size_t orders = 0;
    for (const auto& [plan, count] : ordered) orders += count;
    expect(orders > 0, "the started plans order their TaskForces");
    std::size_t plan_commands = 0;
    for (const auto& command : first.commands) plan_commands += command.find(" i10") != std::string::npos ? 1 : 0;
    std::cout << "plan unit orders routed: " << plan_commands << '\n';
    expect(plan_commands > 0, "the plans' unit orders reach the world");
    // #452 (FH-40): the orders by type, all and from plans, and whether the battle is fought.
    std::map<std::string, std::pair<std::size_t, std::size_t>> verbs;
    for (const auto& command : first.commands) {
        auto& [all, plans] = verbs[command.substr(0, command.find(' '))];
        ++all;
        plans += command.find(" i10") != std::string::npos ? 1 : 0;
        if (command.find(" dropped") != std::string::npos && (command.rfind(std::string(foc::verb_attack_move), 0) == 0
                || command.rfind(std::string(foc::verb_guard), 0) == 0)) {
            expect(false, "an attack-move or guard is dropped: " + command);
        }
    }
    for (const auto& [verb, count] : verbs) {
        std::cout << "  " << verb << ": " << count.first << " (plans " << count.second << ")\n";
    }
    for (const auto& [owner, count] : first.hits) std::cout << "hits by player " << owner << ": " << count << '\n';
    std::cout << "first hit at tick " << first.first_hit << ", units destroyed: " << first.destroyed << '\n';
    // #498: orders and hits by unit type, the capital ships and structures apart from the fighters
    // and bombers (a squadron's orders go to its team container, its hits come from its craft).
    const auto& squadron_table = content->content.motion.squadrons;
    const auto units = [](const eawr::sim::math::Fixed value) {
        return static_cast<double>(value.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
    };
    for (const auto& [type, activity] : first.by_type) {
        const auto name = content->names.find(type);
        if (name == content->names.end()) continue; // a map object the unit tables do not list
        const char* kind = squadron_table.find_squadron(type) != nullptr ? "squadron"
            : squadron_table.find_craft(type) != nullptr                 ? "craft"
                                                                         : "ship";
        std::cout << "  type " << name->second << " (" << kind << "):";
        for (const auto& [player, count] : activity.orders) std::cout << " orders by player " << player << " " << count << ",";
        if (activity.closest) std::cout << " closest to an enemy station " << *activity.closest << ",";
        std::cout << " hits " << activity.hits;
        if (activity.hits != 0) {
            const auto target = content->names.find(activity.first_target);
            std::cout << ", first hit at tick " << activity.first_hit << " from (" << units(activity.first_from.x) << ", "
                      << units(activity.first_from.y) << ") on "
                      << (target != content->names.end() ? target->second : std::to_string(activity.first_target));
        }
        std::cout << '\n';
    }

    // #633 (FT-01, FA-07): the bombing run's squadrons fly to the ordered object although it is
    // fogged to the Empire at the order, and fire at it. The pace: FoC's craft fly at most
    // `Max_Speed` times `Object_Max_Speed_Multiplier_Space` (TIE bomber 3.0 x 1.2 = 3.6 units a
    // frame), so FoC needs at least (distance - strafe reach) / 3.6 frames; each squadron must
    // close at 3.0 or more (within 1.2 times that bound). FoC's own time is not measured
    // (unverified: no retail capture of this run).
    expect(started.contains("bombingrun") && ordered["bombingrun"] > 0, "a bombing run (bombingrun) starts and orders its squadrons");
    expect(first.ordered_squadrons.size() == 6, "the bombing run orders 3 bomber and 3 fighter squadrons");
    constexpr double strafe_reach = 500.0; // TIE Strafe_Distance
    constexpr double least_pace = 3.0;
    // A squadron destroyed before its first shot at the object is excused. With the idle grid (#687,
    // walk 1 WSQ-08 to WSQ-11) the Rebel squadrons stand spread over their own cells rather than on
    // one point, and in this battle two of the three bomber squadrons are shot down on the approach
    // (one 1070 units out, one within sight of the corvette a frame before its first shot). Each
    // excused squadron is printed with its gone tick and its last and closest in-sight distances,
    // so a change that kills the bombers early shows in the output. At least one bomber and one
    // fighter squadron must still fire, every one that fires at the FA-07 pace, and fewer than all
    // the ordered squadrons may be excused.
    std::size_t bombers_fired = 0;
    std::size_t fighters_fired = 0;
    std::size_t excused = 0;
    const auto distance_text = [](const std::map<eawr::sim::EntityId, double>& values, const eawr::sim::EntityId container) {
        const auto found = values.find(container);
        return found != values.end() ? std::to_string(static_cast<long long>(found->second)) : std::string("-");
    };
    for (const auto& [container, target] : first.ordered_squadrons) {
        const auto shot = first.squadron_first_shot.find(container);
        const auto from = first.order_distance.find(container);
        if (shot == first.squadron_first_shot.end()) {
            if (const auto gone = first.gone_tick.find(container); gone != first.gone_tick.end()) {
                std::cout << "excused: bombing run squadron " << container << " destroyed at tick " << gone->second
                          << " before its first shot at object " << target << "; last " << distance_text(first.last_distance, container)
                          << " units from it, closest in sight " << distance_text(first.closest_seen, container) << '\n';
                ++excused;
                continue;
            }
        }
        expect(shot != first.squadron_first_shot.end(), "FT-01: bombing run squadron " + std::to_string(container) + " fires at object "
                + std::to_string(target));
        if (shot != first.squadron_first_shot.end()) ++(first.bomber_force.at(container) ? bombers_fired : fighters_fired);
        if (shot != first.squadron_first_shot.end() && from != first.order_distance.end()) {
            const double pace = (from->second - strafe_reach) / static_cast<double>(shot->second - first.ordered_tick.at(container));
            expect(pace >= least_pace, "FA-07: bombing run squadron " + std::to_string(container) + " closes at " + std::to_string(pace)
                    + " units a tick (at least " + std::to_string(least_pace) + ")");
        }
    }
    expect(bombers_fired >= 1 && fighters_fired >= 1, "FT-01: at least one bomber and one fighter squadron of the bombing run fire (bombers "
            + std::to_string(bombers_fired) + ", fighters " + std::to_string(fighters_fired) + ")");
    expect(excused < first.ordered_squadrons.size(), "FT-01: not every bombing run squadron is destroyed before it fires");
    for (const auto& [container, target] : first.ordered_squadrons) {
        const auto hit = first.squadron_first_hit.find(container);
        std::string target_type;
        for (const auto& unit : content->start.units) {
            if (unit.state.entity_id != target) continue;
            if (const auto name = content->names.find(unit.state.type_id); name != content->names.end()) target_type = " (" + name->second + ")";
        }
        std::cout << "bombingrun squadron " << container << " ordered at tick " << first.ordered_tick.at(container)
                  << " on object " << target << target_type << ": "
                  << (hit != first.squadron_first_hit.end() ? "first hit at tick " + std::to_string(hit->second) : std::string("no hit"))
                  << '\n';
        if (const auto shot = first.squadron_first_shot.find(container); shot != first.squadron_first_shot.end()) {
            std::cout << "    first shot at the object at tick " << shot->second << '\n';
        }
        if (const auto hits = first.squadron_hits.find(container); hits != first.squadron_hits.end()) {
            std::cout << "    hits:";
            for (const auto& [struck, count] : hits->second) std::cout << ' ' << struck << " x" << count;
            std::cout << '\n';
        }
        if (const auto trace = first.squadron_trace.find(container); trace != first.squadron_trace.end()) {
            for (const auto& line : trace->second) std::cout << "    " << line << '\n';
        }
    }

    // The Lua cost per tick (#449 budget).
    std::int64_t max_plans = 0;
    std::int64_t max_total = 0;
    std::int64_t sum_total = 0;
    std::uint32_t max_instances = 0;
    for (const auto& cost : first.journal.costs) {
        max_plans = std::max(max_plans, cost.plan_instructions);
        max_total = std::max(max_total, cost.plan_instructions + cost.freestore_instructions);
        sum_total += cost.plan_instructions + cost.freestore_instructions;
        max_instances = std::max(max_instances, cost.plan_instances);
    }
    std::cout << "Lua cost: max " << max_total << " instructions in a tick (plans " << max_plans << ", " << max_instances
              << " plan instances at most), mean " << (sum_total / static_cast<std::int64_t>(std::max<std::uint64_t>(1, ticks)))
              << " (budget " << auth::Quotas{}.instructions_per_service << " per instance)\n";
    expect(first.journal.costs.size() == ticks, "a Lua cost for every tick");
    expect(max_total < auth::Quotas{}.instructions_per_service, "the AI's Lua cost per tick stays in the service budget");
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    // FT-03 finds the station's special-weapon target; FH-27 answers nil there, so the freestore's
    // player pass keeps running instead of ending on a missing API.
    expect(std::none_of(first.diagnostics.begin(), first.diagnostics.end(),
               [](const std::string& diagnostic) { return diagnostic.find("Fire_Special_Weapon") != std::string::npos; }),
        "the station's special weapon call does not end the freestore");

    // The record replays without scripts.
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities);
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");

    // The live session hosts the same AI and goal system (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), content->ai, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities);
    expect(static_cast<bool>(live), "the live session starts with the goal system");
    if (live) {
        live.value()->advance_to(ticks);
        expect(live.value()->wait_for(ticks, std::chrono::minutes(10)), "the live session reaches the last tick");
        live.value()->stop();
        expect(live.value()->tick_hashes() == first.world_hashes, "the live session's hashes are the plan run's world hashes");
    }

    if (failures != 0) {
        std::cerr << failures << " FoC plan check(s) failed\n";
        return 1;
    }
    std::cout << "FoC plan contracts passed\n";
    return 0;
}

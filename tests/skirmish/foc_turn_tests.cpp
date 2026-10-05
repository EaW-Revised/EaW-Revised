// An AI ship with an enemy astern (#668, docs/behaviour/space-weapon-fire.md A-05, A-08;
// docs/behaviour/foc-tactical-ai.md FH-23, FH-31). The M2 battle with the FoC AI on the Empire,
// staged like the retail rig staging (tools/validation/p1_capture/staging_probe.lua, aiturn): the
// Empire's Acclamator rests at the midpoint between the stations facing the Empire station, and
// the Rebel Nebulon-B rests 1000 units dead astern, inside the Acclamator's attack distance (1400).
//
// - Idle: the Acclamator takes the frigate as its (not ordered) target and keeps its heading
//   until the AI orders it; the AI's first order, its first turn and its first shot are printed.
// - Ordered: the Empire player orders it to attack the frigate at tick 30, as the AI's Lua
//   Attack_Target does (FH-40): it turns in place toward the frigate within a few frames (A-04).
// Both run on 1, 2, 4 and 8 workers with the same hashes (ADR-009).
//
//   foc_turn_tests [ticks]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
namespace math = eawr::sim::math;

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

double real(const math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(math::Fixed::scale);
}

double yaw_of(const math::Quat& q) {
    const double x = real(q.x), y = real(q.y), z = real(q.z), w = real(q.w);
    return std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z)) * 180.0 / 3.14159265358979323846;
}

double wrap(double angle) {
    while (angle >= 180.0) angle -= 360.0;
    while (angle < -180.0) angle += 360.0;
    return angle;
}

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    eawr::sim::EntityId acclamator{};
    eawr::sim::EntityId nebulon{};
};

// The staging: whole-unit positions, and a yaw in whole degrees rotated as the start builds its
// markers' (a rotation about +Z by half the yaw in turns), so every platform stages the same bits.
bool stage(skirmish::SkirmishStart& start, Content& out) {
    double sx[3]{}, sy[3]{};
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::station && unit.state.owner < 3) {
            sx[unit.state.owner] = real(unit.state.position.x);
            sy[unit.state.owner] = real(unit.state.position.y);
        }
    }
    const auto mx = static_cast<std::int64_t>(std::llround((sx[1] + sx[2]) / 2));
    const auto my = static_cast<std::int64_t>(std::llround((sy[1] + sy[2]) / 2));
    const auto yaw = static_cast<std::int64_t>(
        std::llround(std::atan2(sy[2] - static_cast<double>(my), sx[2] - static_cast<double>(mx)) * 180.0 / 3.14159265358979323846));
    const double radians = static_cast<double>(yaw) * 3.14159265358979323846 / 180.0;
    const auto nx = static_cast<std::int64_t>(std::llround(static_cast<double>(mx) - 1000.0 * std::cos(radians)));
    const auto ny = static_cast<std::int64_t>(std::llround(static_cast<double>(my) - 1000.0 * std::sin(radians)));
    std::cout << "staging: Acclamator at (" << mx << ", " << my << ") yaw " << yaw << ", Nebulon-B at (" << nx << ", " << ny
              << ")\n";
    auto turns = math::divide(math::Fixed::from_integer(yaw).value(), math::Fixed::from_integer(720).value());
    if (!turns) return false;
    const auto half = math::wrap_turn(turns.value());
    auto rotation = math::normalize(math::Quat{math::Fixed{}, math::Fixed{}, math::sin_turn(half), math::cos_turn(half)});
    if (!rotation) return false;
    const auto place = [&](const std::string& type, std::int64_t x, std::int64_t y) -> eawr::sim::EntityId {
        for (auto& unit : start.units) {
            if (unit.type != type) continue;
            unit.state.position = math::Vec3{math::Fixed::from_integer(x).value(), math::Fixed::from_integer(y).value(),
                unit.state.position.z};
            unit.state.rotation = rotation.value();
            unit.yaw_degrees = math::Fixed::from_integer(yaw).value();
            for (auto& setup_unit : start.setup.units) {
                if (setup_unit.entity_id != unit.state.entity_id) continue;
                setup_unit.position = unit.state.position;
                setup_unit.rotation = unit.state.rotation;
            }
            return unit.state.entity_id;
        }
        return 0;
    };
    out.acclamator = place("Acclamator_Assault_Ship", mx, my);
    out.nebulon = place("Nebulon_B_Frigate", nx, ny);
    return out.acclamator != 0 && out.nebulon != 0;
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
    input.space_map = skirmish::m2_fixture().map;
    // Keep the production viewer's M2 closure and table order. Victory is
    // already loaded through station production; seeding it separately changes
    // the replay content identity despite retaining the same unit values.
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds");
    if (!start) return std::nullopt;
    Content out;
    expect(stage(start.value(), out), "the Acclamator and the Nebulon-B are staged");
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    out.start = start.value();
    out.content = std::move(content).value();
    out.victory = skirmish::victory_rules(out.start, tables.value());
    out.ai = skirmish::ai_setup(out.start, inputs.value(), tables.value());
    auto enabled = skirmish::enable_goal_system(filesystem.value(), out.ai);
    expect(static_cast<bool>(enabled), "the goal system's XML loads");
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load");
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    return out;
}

struct Run {
    std::vector<std::string> hashes;
    std::vector<double> yaw;         // the Acclamator's yaw after each tick
    std::vector<bool> direct;        // whether its target is an ordered one
    std::vector<eawr::sim::EntityId> target;
    std::optional<std::uint64_t> first_order; // the first order it accepted (no ability switch), and from whom
    tactical::PlayerId first_order_player{};
    tactical::OrderKind first_order_kind{};
    std::optional<std::uint64_t> first_shot;  // the frame of its first weapon fired
};

std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks, bool attack_at_30) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory, content.content.abilities);
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    auto session = foc::create_session(std::move(world).value(), content.ai, content.modules);
    expect(static_cast<bool>(session), "the AI session is created");
    if (!session) return std::nullopt;
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run out;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        std::vector<tactical::PlayerCommand> input;
        if (attack_at_30 && tick == 30) {
            // The Empire player's attack order, as the AI's Lua Attack_Target issues it (FH-40).
            input.push_back(tactical::PlayerCommand{eawr::sim::CommandKey{tick, 2, 0}, {content.acclamator},
                tactical::AttackPayload{content.nebulon}});
        }
        auto stepped = session.value().step(executor, input);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1));
        if (!stepped) return std::nullopt;
        out.hashes.push_back(stepped.value().state_sha256);
        const auto& snapshot = *stepped.value().world.snapshot;
        for (const auto& event : snapshot.events()) {
            // Orders only: an ability switch or scripted damage never becomes a unit's order.
            if (event.kind == tactical::EventKind::order_accepted && event.unit == content.acclamator && !out.first_order &&
                event.order != tactical::OrderKind::ability && event.order != tactical::OrderKind::damage) {
                out.first_order = event.tick;
                out.first_order_player = event.player;
                out.first_order_kind = event.order;
            }
        }
        for (const auto& event : snapshot.combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == content.acclamator && !out.first_shot) {
                out.first_shot = event.tick;
            }
        }
        const auto& units = session.value().world().units();
        double yaw = out.yaw.empty() ? 0.0 : out.yaw.back();
        for (const auto& unit : units) {
            if (unit.entity_id == content.acclamator) yaw = yaw_of(unit.rotation);
        }
        const auto combat = session.value().world().combat_state(content.acclamator);
        out.yaw.push_back(yaw);
        out.direct.push_back(combat && combat->direct);
        out.target.push_back(combat ? combat->attack_target : 0);
    }
    return out;
}

// The first tick after which the yaw is more than `degrees` off its staged value.
std::optional<std::uint64_t> turned(const Run& run, double staged, double degrees) {
    for (std::size_t tick = 0; tick < run.yaw.size(); ++tick) {
        if (std::abs(wrap(run.yaw[tick] - staged)) > degrees) return tick;
    }
    return std::nullopt;
}

std::string text(const std::optional<std::uint64_t>& value) {
    return value ? std::to_string(*value) : std::string("none");
}

// WMV-20 / AT-10: loaded Victory and Rebel station, isolated from other units.
// The AI path uses a test freestore through the real Lua object order binding.
// Fog is off. Profiles, health, weapon frames and cones retain their loaded values.
void capital_station_test(const Content& content, const std::filesystem::path& output = {}) {
    const auto victory_type = skirmish::type_id("Victory_Destroyer");
    const auto station_type = skirmish::type_id("Skirmish_Rebel_Star_Base_1");
    const auto* profile = content.content.combat.find(victory_type);
    const auto* target_profile = content.content.combat.find(station_type);
    expect(profile != nullptr && target_profile != nullptr, "capital facing: loaded Victory and station profiles exist");
    if (profile == nullptr || target_profile == nullptr) return;
    if (!output.empty()) std::filesystem::create_directories(output);
    for (const bool scripted : {false, true}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto setup = skirmish::recording_setup(skirmish::m2_fixture(), content.start);
            for (auto& slot : setup.skirmish->slots)
                if (slot.player == 2) slot.fleet.push_back("Victory_Destroyer");
            tactical::UnitState ship{}, station{};
            for (const auto& unit : setup.units) {
                if (unit.entity_id == content.acclamator) ship = unit;
                if (unit.type_id == station_type) station = unit;
            }
            ship.entity_id = 1;
            ship.type_id = victory_type;
            ship.owner = 2;
            ship.position.x = math::Fixed::from_integer(-1000).value();
            ship.position.y = {};
            const auto eighth = math::Fixed::from_raw(math::Fixed::scale / 8);
            ship.rotation = math::normalize(math::Quat{{}, {}, math::sin_turn(eighth), math::cos_turn(eighth)}).value();
            ship.order = {};
            station.entity_id = 2;
            station.type_id = station_type;
            station.owner = 1;
            station.position.x = {};
            station.position.y = {};
            station.rotation = math::Quat{{}, {}, {}, math::Fixed::from_raw(math::Fixed::scale)};
            station.order = {};
            setup.units = {ship, station};
            setup.squadrons.clear();
            auto world = tactical::TacticalSession::create(setup, content.content.sensors, content.content.durability,
                content.content.motion, std::nullopt, content.content.combat, {}, content.content.abilities);
            expect(static_cast<bool>(world), "capital facing: isolated world builds");
            if (!world) continue;
            auto ai = content.ai;
            ai.bounds.reset();
            ai.xml.clear();
            for (auto& player : ai.players) {
                player.ai = scripted && player.player == 2;
                player.human = !player.ai && !player.neutral;
            }
            ai.freestore_module = "Data/Scripts/Test/CapitalFacing.lua";
            auto modules = content.modules;
            modules[ai.freestore_module] =
                "function Base_Definitions() ServiceRate = 1; UnitServiceRate = 1 end\n"
                "function main() end\n"
                "function On_Unit_Service(object)\n"
                "  if not ordered and object.Get_Type() == Find_Object_Type('Victory_Destroyer') then\n"
                "    local stations = Find_All_Objects_Of_Type('Skirmish_Rebel_Star_Base_1')\n"
                "    if table.getn(stations) > 0 then object.Attack_Move(stations[1]); ordered = true end\n"
                "  end\n"
                "end\n";
            auto session = foc::create_session(std::move(world).value(), ai, modules);
            expect(static_cast<bool>(session), "capital facing: scripted session builds");
            if (!session) continue;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::map<std::uint32_t, std::uint64_t> shots;
            bool ordered = false;
            std::ofstream trace;
            const auto label = scripted ? "ai-attack-move" : "human-attack";
            if (!output.empty() && workers == 1) {
                trace.open(output / (std::string(label) + ".csv"));
                trace << "tick,yaw,centre_distance,weapon,aim_distance,cone_yaw,cone_pitch,cone_admitted,raw_range,extended_range,shots\n";
            }
            for (std::uint64_t tick = 0; tick < 720; ++tick) {
                std::vector<tactical::PlayerCommand> input;
                if (!scripted && tick == 0) input.push_back({{0, 2, 0}, {1}, tactical::AttackPayload{2}});
                auto stepped = session.value().step(executor, input);
                expect(static_cast<bool>(stepped), "capital facing: world and Lua step succeed");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto& snapshot = *stepped.value().world.snapshot;
                for (const auto& event : snapshot.events()) {
                    ordered = ordered || (event.kind == tactical::EventKind::order_accepted && event.unit == 1
                        && event.order == (scripted ? tactical::OrderKind::attack_move : tactical::OrderKind::attack));
                }
                for (const auto& event : snapshot.combat_events()) {
                    if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1 && event.target == 2)
                        ++shots[event.weapon];
                }
                const auto units = session.value().world().units();
                const auto source = std::find_if(units.begin(), units.end(), [](const auto& value) { return value.entity_id == 1; });
                const auto target = std::find_if(units.begin(), units.end(), [](const auto& value) { return value.entity_id == 2; });
                if (source == units.end() || target == units.end()) break;
                if (trace) {
                    const auto source_matrix = math::to_matrix(source->rotation, source->position).value();
                    const auto target_matrix = math::to_matrix(target->rotation, target->position).value();
                    auto aim = target->position;
                    auto best = std::numeric_limits<double>::infinity();
                    const auto durability = session.value().world().durability_state(2);
                    const auto* health = content.content.durability.find(station_type);
                    for (const auto& hardpoint : target_profile->hardpoints) {
                        if (!hardpoint.targetable || (durability && health
                            && tactical::hardpoint_destroyed(*health, *durability, hardpoint.hardpoint))) continue;
                        const auto point = math::transform_point(target_matrix, hardpoint.position).value();
                        const auto distance = std::hypot(real(point.x) - real(source->position.x), real(point.y) - real(source->position.y),
                            real(point.z) - real(source->position.z));
                        if (distance < best) { best = distance; aim = point; }
                    }
                    const auto* footprint = content.content.motion.footprint(station_type);
                    const auto radius = footprint != nullptr ? real(footprint->radius) : 0.0;
                    for (const auto& weapon : profile->weapons) {
                        math::Vec3 middle{math::Fixed::from_raw((weapon.fire_a.x.raw() + weapon.fire_b.x.raw()) / 2),
                            math::Fixed::from_raw((weapon.fire_a.y.raw() + weapon.fire_b.y.raw()) / 2),
                            math::Fixed::from_raw((weapon.fire_a.z.raw() + weapon.fire_b.z.raw()) / 2)};
                        if (!weapon.has_fire_b) middle = weapon.fire_a;
                        const auto muzzle = math::transform_point(source_matrix, middle).value();
                        const std::array<double, 3> delta{real(aim.x) - real(muzzle.x), real(aim.y) - real(muzzle.y), real(aim.z) - real(muzzle.z)};
                        std::array<double, 3> local{};
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            for (std::size_t row = 0; row < 3; ++row) local[axis] += real(source_matrix.rows[row][axis]) * delta[row];
                        if (weapon.fire_axes) {
                            const auto prior = local;
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                const auto& basis = (*weapon.fire_axes)[axis];
                                local[axis] = real(basis.x) * prior[0] + real(basis.y) * prior[1] + real(basis.z) * prior[2];
                            }
                        }
                        const auto yaw = std::atan2(local[1], local[0]) * 180.0 / 3.14159265358979323846;
                        const auto pitch = std::atan2(local[2], std::hypot(local[0], local[1])) * 180.0 / 3.14159265358979323846;
                        const auto distance = std::hypot(delta[0], delta[1]);
                        trace << tick << ',' << yaw_of(source->rotation) << ','
                            << std::hypot(real(target->position.x) - real(source->position.x), real(target->position.y) - real(source->position.y))
                            << ',' << weapon.hardpoint << ',' << distance << ',' << yaw << ',' << pitch << ','
                            << (std::abs(yaw) <= real(weapon.cone_width) / 2 && std::abs(pitch) <= real(weapon.cone_height) / 2)
                            << ',' << (distance <= real(weapon.range)) << ',' << (distance <= real(weapon.range) + radius)
                            << ',' << shots[weapon.hardpoint] << '\n';
                    }
                }
            }
            expect(ordered, "capital facing: the human or real Lua binding issues the expected unit order");
            expect(shots[0] > 0, "capital facing: the Victory forward ion cannon fires at the station");
            if (!output.empty() && workers == 1) {
                auto bytes = tactical::write_replay(session.value().world().record());
                expect(static_cast<bool>(bytes), "capital facing: observation replay encodes");
                if (bytes) {
                    std::ofstream replay(output / (std::string(label) + ".eawr-replay"), std::ios::binary);
                    replay.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
                    expect(static_cast<bool>(replay), "capital facing: observation replay writes");
                }
            }
            if (workers == 1) {
                reference = hashes;
                std::cout << label << ": ion shots " << shots[0] << ", sampled ticks " << hashes.size() << '\n';
            } else expect(hashes == reference, "capital facing: every tick matches on 1/2/4/8 workers");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC AI turn staging\n";
        return 0;
    }
    const bool capital_only = argc > 1 && std::string(argv[1]) == "--capital-facing";
    const std::uint64_t ticks = !capital_only && argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1050;
    auto content = load(*root);
    if (!content) return 1;
    if (capital_only) {
        capital_station_test(*content, argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        return failures == 0 ? 0 : 1;
    }
    double staged = 0;
    for (const auto& unit : content->start.setup.units) {
        if (unit.entity_id == content->acclamator) staged = yaw_of(unit.rotation);
    }

    std::vector<Run> idle;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks, false);
        if (!result) return 1;
        idle.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < idle.size(); ++index) {
        expect(idle[index].hashes == idle.front().hashes, "idle: every tick's hash matches worker count 1");
    }
    const Run& quiet = idle.front();
    const auto order = quiet.first_order;
    const auto turn = turned(quiet, staged, 10.0);
    const auto moved = turned(quiet, staged, 0.001);
    std::cout << "idle: first AI order tick " << text(order) << " (player " << quiet.first_order_player << ", kind "
              << tactical::to_string(quiet.first_order_kind) << "), first yaw change " << text(moved) << ", turn past 10 deg "
              << text(turn) << ", first shot " << text(quiet.first_shot) << '\n';
    // A-05 (AT-02, AT-12; retail staging #668): the frigate astern is its target, not an ordered
    // one, and nothing turns it before the AI orders it.
    expect(quiet.target.size() > 1 && quiet.target[1] == content->nebulon && !quiet.direct[1],
        "idle: the Acclamator takes the frigate astern as its own, not ordered, target");
    expect(order.has_value() && quiet.first_order_player == 2, "idle: the Empire AI orders the Acclamator");
    expect(!moved || (order && *moved > *order), "A-05: the Acclamator keeps its heading until the AI's first order");

    std::vector<Run> ordered;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks, true);
        if (!result) return 1;
        ordered.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        expect(ordered[index].hashes == ordered.front().hashes, "ordered: every tick's hash matches worker count 1");
    }
    const Run& attack = ordered.front();
    const auto attack_moved = turned(attack, staged, 0.001);
    const auto attack_turn = turned(attack, staged, 10.0);
    std::cout << "ordered at tick 30: first yaw change " << text(attack_moved) << ", turn past 10 deg " << text(attack_turn)
              << ", first shot " << text(attack.first_shot) << '\n';
    expect(attack.first_order == std::uint64_t{30} && attack.first_order_kind == tactical::OrderKind::attack,
        "ordered: the attack is the Acclamator's first order");
    expect(attack.direct.size() > 31 && attack.direct[31] && attack.target[31] == content->nebulon,
        "A-01: the attack makes the frigate its ordered target");
    // A-04, MV-20: the turn in place starts a few frames after the order, as S-26's does.
    expect(attack_moved && *attack_moved >= 31 && *attack_moved <= 35, "A-04: the ordered Acclamator starts turning within 5 frames");
    expect(attack_turn && (!order || *attack_turn < *order), "A-04: the ordered turn comes before the idle case's first AI order");

    capital_station_test(*content);
    if (failures != 0) {
        std::cerr << failures << " AI turn check(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI turn staging passed\n";
    return 0;
}

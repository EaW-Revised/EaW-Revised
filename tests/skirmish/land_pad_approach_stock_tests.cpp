// #6976: on a stock land start a squad ordered onto an EMPTY build pad reaches it (a pad is a collision body only while it carries a
// building or its construction), and a squad ordered onto an occupied pad ends beside it. The map, the factions, the squads and the
// pads are all read from the installed data (the first stock two-player land map whose path names Naboo, else the first one); the
// run prints the squad's size, the issued destination and the stop distances for the stage log, and gives the same state at 1/2/4/8.
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/text.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/scene/scene.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
using namespace sim::tactical;

testing::Checker checks;
void expect(const bool ok, const std::string& message) { checks.check(ok, message.c_str(), std::cerr); }
template<class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
void must(core::Result<void> value) { if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message); }

sim::math::Fixed whole(const std::int64_t value) { return sim::math::Fixed::from_ratio(value, 1).value(); }
sim::math::Fixed planar(const sim::math::Vec2 a, const sim::math::Vec2 b) {
    const auto dx = static_cast<long double>(a.x.raw() - b.x.raw()), dy = static_cast<long double>(a.y.raw() - b.y.raw());
    return sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::sqrt(dx * dx + dy * dy)));
}

struct Prepared {
    skirmish::Fixture fixture;
    units::UnitTables tables;
    skirmish::LandStartInputs inputs;
    skirmish::LandStart land;
};

// One walk: the squad `squad` of player 1 is ordered to `goal` and the run records how close it got and where it stopped.
using Bytes = std::vector<std::uint8_t>;
struct Walk { Bytes state; sim::math::Fixed closest{}, final_distance{}, length{}; bool reached{}; };

Walk walk(const Prepared& prepared, const sim::PartitionExecutor& executor, const sim::EntityId squad, const sim::math::Vec2 goal, const int ticks) {
    auto session = take(TacticalSession::create(prepared.land.start.setup, prepared.land.content));
    must(session.submit({{0, 1, 1}, {squad}, MovePayload{{goal.x, goal.y, {}}}}));
    Walk result;
    result.closest = sim::math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max() / 4);
    const auto measure = [&] {
        for (const auto& row : session.land_movement()->squads()) {
            if (row.object != squad) continue;
            const auto distance = planar(row.position, goal);
            result.closest = std::min(result.closest, distance);
            result.final_distance = distance;
            result.reached = row.order == LandMoveOrder::reached;
            result.length = sim::math::Fixed::from_raw(row.formation_length.raw() + row.jiggle.raw()); // the squad's own extent
        }
    };
    for (int tick = 0; tick < ticks; ++tick) { take(session.step(executor)); measure(); }
    result.state = session.canonical_state_bytes();
    return result;
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto loaded = take(data::load_catalog(filesystem, data::Profile::foc));
    const auto maps = take(skirmish::land_setup_maps(filesystem, loaded.catalog));
    std::vector<std::string> lobby;
    for (const auto& entry : skirmish::setup_factions(loaded.catalog)) lobby.push_back(entry.name);
    expect(lobby.size() >= 2, "the catalog has two playable lobby factions");
    if (lobby.size() < 2) return checks.finish();
    scene::VfsAssetCache cache(filesystem);

    const auto prepare = [&](const std::string& map_path, const std::string& first, const std::string& second, const bool pre_built)
        -> std::unique_ptr<Prepared> {
        auto prepared = std::make_unique<Prepared>();
        auto& fixture = prepared->fixture;
        fixture.map = map_path; fixture.seed = 6976;
        fixture.slots = {{1, first, 0, true, {}}, {2, second, 1, true, {}}};
        units::UnitTables empty;
        const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, empty));
        fixture.map_sha256 = discovered.common.map_sha256;
        fixture.match = discovered.common.match_defaults;
        fixture.match->pre_built_base = pre_built;
        units::LoadInput load;
        load.domain = BattleDomain::land; load.catalog = &loaded.catalog;
        load.filesystem = &filesystem; const auto access = cache.access(); load.model = access.model;
        std::set<std::string> requested;
        for (const auto& slot : fixture.slots) {
            const auto faction = std::find_if(discovered.factions.begin(), discovered.factions.end(),
                [&](const auto& row) { return row.faction == slot.faction; });
            if (faction != discovered.factions.end()) requested.insert(faction->forces.begin(), faction->forces.end());
        }
        if (requested.empty()) return nullptr;
        load.types.assign(requested.begin(), requested.end());
        prepared->tables = take(units::load_unit_tables(load));
        for (const auto& company : prepared->tables.land_companies)
            for (const auto& member : company.members) requested.insert(member.type);
        load.types.assign(requested.begin(), requested.end());
        prepared->tables = take(units::load_unit_tables(load));
        prepared->inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, prepared->tables));
        const auto gates = take(skirmish::authored_land_content_inputs(prepared->tables));
        prepared->land = take(skirmish::build_land_start(fixture, prepared->inputs, gates));
        return prepared;
    };

    // The first stock two-player land map that names Naboo, else the first one; the first faction is the Empire when the lobby has it.
    const auto naboo = [](const std::string& path) { return core::to_lower(path).find("naboo") != std::string::npos; };
    std::vector<const skirmish::SetupMap*> candidates;
    for (const auto& map : maps) {
        if (!map.unavailable.empty() || map.metadata.capacity != 2 || map.teams.size() < 2) continue;
        candidates.push_back(&map);
    }
    std::stable_partition(candidates.begin(), candidates.end(), [&](const auto* map) { return naboo(map->path); });
    std::string first = lobby[0], second = lobby[1];
    for (std::size_t i = 0; i < lobby.size(); ++i)
        if (core::to_lower(lobby[i]).find("empire") != std::string::npos) { first = lobby[i]; second = lobby[(i + 1) % lobby.size()]; }
    for (const bool pre_built : {false, true}) {
        std::unique_ptr<Prepared> prepared;
        const skirmish::SetupMap* chosen = nullptr;
        for (const auto* map : candidates) {
            prepared = prepare(map->path, first, second, pre_built);
            if (prepared) { chosen = map; break; }
        }
        expect(prepared != nullptr, "a stock two-player land map starts both factions with land forces");
        if (!prepared) return checks.finish();
        auto probe = take(TacticalSession::create(prepared->land.start.setup, prepared->land.content));
        // Player 1's squads and the pads of the start, by their state in the opening session.
        std::vector<sim::EntityId> squads;
        for (const auto& row : probe.land_movement()->squads())
            if (const auto unit = probe.unit_state(row.object); unit && unit->owner == 1) squads.push_back(row.object);
        std::cout << "map " << chosen->path << ", " << first << " vs " << second << ", pre-built base " << pre_built << ": "
                  << squads.size() << " squads of player 1, " << probe.pads().size() << " pads\n";
        expect(!squads.empty() && !probe.pads().empty(), "the start has a squad of player 1 and build pads");
        if (squads.empty() || probe.pads().empty()) continue;
        const auto squad = squads.front();
        const auto squad_position = probe.unit_state(squad)->position;
        // The nearest build pad of each state to the squad: empty (no building, no construction) and occupied.
        std::optional<sim::EntityId> empty_pad, occupied_pad;
        sim::math::Fixed empty_best, occupied_best;
        for (const auto& [id, state] : probe.pads()) {
            const auto unit = probe.unit_state(id);
            if (!unit) continue;
            // Only a pad with a collision body row is a build pad: the economy tracks other points (stations) that have none.
            const auto has_body = std::any_of(prepared->land.content.land.pad_bodies.begin(), prepared->land.content.land.pad_bodies.end(),
                [&](const auto& entry) { return entry.type == unit->type_id; });
            if (!has_body) continue;
            const auto distance = planar({unit->position.x, unit->position.y}, {squad_position.x, squad_position.y});
            const bool occupied = state.constructed != 0 || state.under_construction != 0;
            auto& best = occupied ? occupied_pad : empty_pad;
            auto& best_distance = occupied ? occupied_best : empty_best;
            if (!best || distance < best_distance) { best = id; best_distance = distance; }
        }
        const sim::InlineExecutor serial;
        const auto report = [&](const char* kind, const sim::EntityId pad, const sim::math::Vec2 goal, const char* where) {
            const auto result = walk(*prepared, serial, squad, goal, 900);
            std::cout << kind << " pad " << pad << " " << where << ": goal (" << goal.x.raw() << ", " << goal.y.raw() << ") closest raw="
                      << result.closest.raw() << " final raw=" << result.final_distance.raw() << " reached=" << result.reached << '\n';
            return result;
        };
        if (empty_pad) {
            const auto centre = probe.unit_state(*empty_pad)->position;
            const auto at_centre = report("empty", *empty_pad, {centre.x, centre.y}, "centre");
            // The retail squads came within 4.4 and 6.0 of an empty pad's centre (Naboo, Bespin); a squad held off by a body stops
            // a squad radius plus the pad's radius away, well above 8.
            expect(at_centre.closest < whole(8), "#6976: a squad ordered onto an empty pad reaches its centre");
            const auto edge = report("empty", *empty_pad, {sim::math::Fixed::from_raw(centre.x.raw() + whole(12).raw()), centre.y}, "edge");
            expect(edge.closest < whole(8), "#6976: and its edge goal");
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                platform::ThreadWorkerAdapter threads(workers);
                expect(walk(*prepared, threads, squad, {centre.x, centre.y}, 900).state == at_centre.state,
                    "#6976: the stock approach to an empty pad equals on 1/2/4/8 workers");
            }
        } else std::cout << "no empty pad in this start\n";
        if (occupied_pad) {
            const auto centre = probe.unit_state(*occupied_pad)->position;
            const auto onto = report("occupied", *occupied_pad, {centre.x, centre.y}, "centre");
            // The pad's footprint is the content's own row for its type; the squad stops outside it (a disc of the half diagonal) and
            // within one squad extent (formation length plus jiggle) of its edge. Retail stops beside a live building at a distance
            // that depends on the building, so no fixed number is asserted.
            const auto type = probe.unit_state(*occupied_pad)->type_id;
            const auto row = std::find_if(prepared->land.content.land.pad_bodies.begin(), prepared->land.content.land.pad_bodies.end(),
                [&](const auto& entry) { return entry.type == type; });
            expect(row != prepared->land.content.land.pad_bodies.end(), "the occupied pad's type has a collision body row");
            if (row != prepared->land.content.land.pad_bodies.end()) {
                const auto half_diagonal = planar({row->half_x, row->half_y}, {sim::math::Fixed{}, sim::math::Fixed{}});
                const auto limit = sim::math::Fixed::from_raw(half_diagonal.raw() + onto.length.raw());
                std::cout << "occupied pad footprint half " << row->half_x.raw() << " x " << row->half_y.raw() << " half diagonal raw="
                          << half_diagonal.raw() << ", squad extent raw=" << onto.length.raw() << ", stop raw=" << onto.closest.raw() << '\n';
                expect(onto.closest >= half_diagonal, "#6976: a squad ordered onto an occupied pad stops outside the building's footprint");
                expect(onto.closest <= limit, "#6976: and within one squad extent of its edge");
            }
        } else std::cout << "no occupied pad in this start\n";
    }
    return checks.finish();
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        std::cout << "SKIPPED: the stock land pad approach test needs --installed\n";
        return 77;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

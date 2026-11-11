// A stock land skirmish whose AI owns a build pad: the pad structures the map's pads offer must be visible to the land AI type
// registry, so the AI script's pad build is accepted and the structure grows (WBP-09, EXP-E01). The script is a fixture that asks
// for a structure by name; which pad, which structure and which faction are read from the installed data, nothing is named here.
// The same run on 1, 2, 4 and 8 workers must give the same state digest every tick.
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/core/text.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/scene/scene.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace foc = script::foc;
using namespace sim::tactical;

testing::Checker checks;
void expect(const bool ok, const std::string& message) { checks.check(ok, message.c_str(), std::cerr); }
template<class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}

// One AI pad owner and one structure its pad offers it, chosen from the start's own economy: the shortest affordable structure
// of the first pad (without a prebuilt child) of the AI player.
struct Candidate {
    PlayerId owner{};
    sim::EntityId pad{};
    std::string pad_type;
    std::string structure;
    TypeId pad_type_id{}, structure_type_id{}, constructed_type_id{};
    std::uint64_t build_frames{};
};

std::string name_of(const units::UnitTables& tables, const TypeId type) {
    for (const auto& row : tables.units)
        if (skirmish::type_id(row.id) == type) return row.id;
    return {};
}

std::optional<Candidate> pick(const skirmish::LandStart& land, const skirmish::LandStartInputs& inputs, const skirmish::Fixture& fixture,
    const PlayerId owner) {
    const auto& economy = land.content.economy;
    const auto faction = skirmish::faction_id(fixture.slots[owner - 1].faction);
    std::optional<Candidate> best;
    for (const auto& unit : land.start.setup.units) {
        if (unit.owner != owner) continue;
        if (std::any_of(land.content.land.initial_pad_children.begin(), land.content.land.initial_pad_children.end(),
                [&](const auto& link) { return link.first == unit.entity_id; })) continue;
        for (const auto& menu : economy.menus) {
            if (menu.station != unit.type_id || menu.faction != faction) continue;
            for (const auto& option : menu.options) {
                if (option.kind != BuildKind::structure || !option.available || !(option.price <= land.start.match.credits)) continue;
                const auto child = std::find_if(economy.pads.construction.begin(), economy.pads.construction.end(),
                    [&](const auto& row) { return row.type == option.type && economy.stream(row.constructed) == nullptr; });
                if (child == economy.pads.construction.end()) continue;
                const auto frames = static_cast<std::uint64_t>(std::max(option.build_frames, option.ai_build_frames));
                if (best && best->build_frames <= frames) continue;
                Candidate found;
                found.owner = owner; found.pad = unit.entity_id; found.pad_type_id = unit.type_id;
                found.structure_type_id = option.type; found.constructed_type_id = child->constructed;
                found.build_frames = frames;
                found.pad_type = name_of(inputs.pad_tables, unit.type_id);
                found.structure = name_of(inputs.pad_tables, option.type);
                if (found.pad_type.empty() || found.structure.empty()) continue;
                best = std::move(found);
            }
        }
        if (best) break;
    }
    return best;
}

// The AI player's freestore: every pump asks its own pad of the chosen type to build the chosen structure, until one request
// was accepted. Both names come from the data (see pick).
std::map<std::string, std::string> modules(const Candidate& candidate) {
    const std::string source =
        "function Base_Definitions() ServiceRate=0; UnitServiceRate=0; accepted=0; attempts=0 end\n"
        "function main()\n"
        "  while true do\n"
        "    if accepted == 0 then\n"
        "      local pads = Find_All_Objects_Of_Type(PlayerObject, '" + candidate.pad_type + "')\n"
        "      for index, pad in pairs(pads) do\n"
        "        if accepted == 0 then\n"
        "          attempts = attempts + 1\n"
        "          if pad.Build('" + candidate.structure + "') then accepted = 1 end\n"
        "        end\n"
        "      end\n"
        "    end\n"
        "    coroutine.yield(true)\n"
        "  end\n"
        "end\n";
    return {{"PadProbe.lua", source},
        {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
}

struct Run {
    std::vector<std::string> hashes;
    bool ordered{};          // the script's pad build reached the world as an accepted order
    bool commanded{};        // the script produced the AI pad build command
    bool child_appeared{};   // the under-construction structure exists as an object
    bool completed{};        // the constructed structure exists as an object of the authored type
    std::uint64_t ordered_tick{}, completed_tick{};
};

bool has(const foc::AiSetup& setup, const TypeId type) {
    return std::any_of(setup.content.types.begin(), setup.content.types.end(), [&](const auto& row) { return row.type_id == type; });
}

Run run(const skirmish::LandStart& land, foc::AiSetup setup, const Candidate& candidate, const std::size_t workers) {
    setup.freestore_module = "PadProbe.lua";
    auto session = take(foc::create_session(take(TacticalSession::create(land.start.setup, land.content)), setup, modules(candidate)));
    const platform::ThreadWorkerAdapter executor(workers);
    Run out;
    const std::uint64_t limit = candidate.build_frames + 300;
    for (std::uint64_t tick = 0; tick < limit && !out.completed; ++tick) {
        auto stepped = take(session.step(executor));
        out.hashes.push_back(stepped.state_sha256);
        for (const auto& routed : stepped.script_input) out.commanded |= routed.command.verb == foc::verb_pad_build;
        const auto& snapshot = *stepped.world.snapshot;
        for (const auto& event : snapshot.events()) {
            if (event.kind == EventKind::order_accepted && event.order == OrderKind::pad_build && event.player == candidate.owner) {
                if (!out.ordered) out.ordered_tick = snapshot.completed_tick();
                out.ordered = true;
            }
        }
        const auto state = session.world().pads().find(candidate.pad);
        if (state == session.world().pads().end()) continue;
        const auto exists = [&](const sim::EntityId id, const TypeId type) {
            const auto instances = snapshot.instances();
            return id != 0 && std::any_of(instances.begin(), instances.end(),
                [&](const auto& row) { return row.entity_id == id && (type == 0 || row.type_id == type); });
        };
        if (exists(state->second.under_construction, 0)) out.child_appeared = true;
        if (exists(state->second.constructed, candidate.constructed_type_id)) {
            out.completed = true;
            out.completed_tick = snapshot.completed_tick();
        }
    }
    return out;
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
    scene::VfsAssetCache cache(filesystem);
    const auto selected = std::find_if(maps.begin(), maps.end(), [](const auto& map) {
        return map.path.find("_mp_land_alderaan") != std::string::npos && map.unavailable.empty() && map.metadata.capacity == 2
            && map.teams.size() >= 2 && map.teams[0].spawn_capacity.value_or(0) >= 1 && map.teams[1].spawn_capacity.value_or(0) >= 1;
    });
    expect(selected != maps.end(), "a stock two-player land map exists");
    if (selected == maps.end()) return checks.finish();
    // Slot 1 is the AI under test; slot 2 is a human seat so that only one freestore runs.
    skirmish::Fixture fixture; fixture.map = selected->path; fixture.seed = 6578;
    fixture.slots = {{1, "Rebel", 0, false, {}}, {2, "Empire", 1, true, {}}};
    units::UnitTables empty;
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, empty));
    fixture.map_sha256 = discovered.common.map_sha256;
    fixture.match = discovered.common.match_defaults;
    fixture.match->pre_built_base = true;
    units::LoadInput load; load.domain = BattleDomain::land; load.catalog = &loaded.catalog;
    load.filesystem = &filesystem; const auto access = cache.access(); load.model = access.model;
    std::set<std::string> requested;
    for (const auto& slot : fixture.slots) {
        const auto faction = std::find_if(discovered.factions.begin(), discovered.factions.end(),
            [&](const auto& row) { return row.faction == slot.faction; });
        if (faction != discovered.factions.end()) requested.insert(faction->forces.begin(), faction->forces.end());
    }
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies)
        for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end());
    tables = take(units::load_unit_tables(load));
    const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, tables));
    const auto gates = take(skirmish::authored_land_content_inputs(tables));
    const auto land = take(skirmish::build_land_start(fixture, inputs, gates));

    const auto candidate = pick(land, inputs, fixture, 1);
    expect(candidate.has_value(), "the AI starts with a build pad that offers an affordable structure");
    if (!candidate) return checks.finish();
    std::cout << "AI pad " << candidate->pad_type << " builds " << candidate->structure << " frames=" << candidate->build_frames << '\n';

    // The registry: the AI sees the pad and the structure the pad menu offers; the tables the caller holds do not.
    const auto setup = skirmish::land_ai_setup(land, inputs, tables);
    expect(has(setup, candidate->pad_type_id) && has(setup, candidate->structure_type_id) && has(setup, candidate->constructed_type_id),
        "the land AI type registry holds the pad, its structure and the constructed type");
    const auto again = skirmish::land_ai_setup(land, inputs, tables);
    expect(setup.content.types.size() == again.content.types.size() && std::equal(setup.content.types.begin(), setup.content.types.end(),
            again.content.types.begin(), [](const auto& a, const auto& b) { return a.type_id == b.type_id; }),
        "the land AI type registry is built in the same order every time");
    expect(!has(skirmish::ai_setup(land.start, inputs.common, tables), candidate->structure_type_id),
        "the tables of the faction forces alone do not hold the pad structure (the gap the merge closes)");

    // The scripted pad build, on 1, 2, 4 and 8 workers.
    std::optional<Run> first;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto result = run(land, setup, *candidate, workers);
        const auto label = "workers=" + std::to_string(workers) + ": ";
        expect(result.commanded, label + "the AI script's pad build is issued");
        expect(result.ordered, label + "the pad build order is accepted");
        expect(result.child_appeared, label + "the structure appears on the pad");
        expect(result.completed, label + "the constructed structure of the authored type exists");
        std::cout << label << "ordered tick " << result.ordered_tick << ", completed tick " << result.completed_tick
                  << ", ticks " << result.hashes.size() << '\n';
        if (!first) { first = result; continue; }
        expect(result.hashes == first->hashes && result.ordered_tick == first->ordered_tick && result.completed_tick == first->completed_tick,
            label + "the state digest of every tick equals the one-worker run");
    }
    return checks.finish();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        std::cout << "SKIPPED: the stock land AI pad test needs --installed\n";
        return 77;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

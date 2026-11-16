// #6152 (owner decision C3 on #4344): a solo human raid resolves like an invasion.
//
// Drives the installed Sandbox_Equal_Footing_Underworld campaign through the ordinary live path (real movement, the stock
// raid policy, the stock conflict services and the stock auto-resolver): a Rebel player launches one loaded transport
// from its own surface into a new fleet and orders a raid (WGM-37) on a defended enemy planet. The raid lands without
// an orbital fight (WGM-34/39), its land conflict is admitted as a raid (WAR-14) and is auto-resolved like any invasion.
// The AI raid case waits on the raid Lua bindings (#7271).
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "eawr/vfs/vfs.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/graph.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;

const gc::ControlPlanet* control_row(const gc::GalacticSnapshot& snapshot, gc::PlanetId id) {
    if (!snapshot.control()) return nullptr;
    for (const auto& row : snapshot.control()->planets) if (row.planet == id) return &row;
    return nullptr;
}
const gc::Fleet* fleet_row(const gc::GalacticSnapshot& snapshot, gc::FleetId id) {
    for (const auto& row : snapshot.fleets()) if (row.id == id) return &row;
    return nullptr;
}
gc::PlayerId planet_owner(const gc::GalacticSnapshot& snapshot, gc::PlanetId id) {
    for (const auto& row : snapshot.planets()) if (row.id == id) return row.owner;
    return 0;
}
// The company carried by a landed transport, when it is a single loaded company.
std::size_t cargo(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId container) {
    if (!snapshot.movement()) return 0;
    for (const auto& row : snapshot.movement()->contents) if (row.container == container) return row.children.size();
    return 0;
}
std::vector<gc::PlanetId> shortest_path(const gc::GalacticSnapshot& snapshot, gc::PlanetId from, gc::PlanetId to) {
    if (!snapshot.maximum_movement_distance()) throw std::runtime_error("raid route requires sourced movement range");
    const auto graph = take(gc::PlanetGraph::build(snapshot.planets(), snapshot.routes(), *snapshot.maximum_movement_distance()));
    std::map<gc::PlanetId, gc::PlanetId> parent{{from, from}};
    std::deque<gc::PlanetId> queue{from};
    while (!queue.empty()) {
        const auto at = queue.front(); queue.pop_front();
        if (at == to) break;
        for (const auto& edge : graph.node(at)->edges)
            if (edge.to && !parent.contains(edge.to)) { parent.emplace(edge.to, at); queue.push_back(edge.to); }
    }
    if (!parent.contains(to)) return {};
    std::vector<gc::PlanetId> path;
    for (auto at = to; at != from; at = parent.at(at)) path.push_back(at);
    path.push_back(from);
    std::reverse(path.begin(), path.end());
    return path;
}

struct Pick { gc::PlanetId source{}, target{}; eawr::sim::EntityId transport{}; std::vector<gc::PlanetId> path; };
// The nearest defended enemy planet from a raider surface holding one loaded transport.
std::optional<Pick> choose(const gc::GalacticSnapshot& snapshot, gc::PlayerId raider) {
    std::vector<gc::PlanetId> targets;
    for (const auto& planet : snapshot.planets()) {
        if (!planet.owner || planet.owner == raider) continue;
        const gc::ControlPlayer* owner = nullptr;
        if (snapshot.control()) for (const auto& row : snapshot.control()->players) if (row.player == planet.owner) owner = &row;
        const auto* surface = control_row(snapshot, planet.id);
        if (!owner || owner->neutral || !owner->playable || !surface || !surface->surface_accessible || surface->destroyed) continue;
        if (std::none_of(surface->landed.begin(), surface->landed.end(), [&](const auto& force) { return force.owner == planet.owner; }))
            continue;
        targets.push_back(planet.id);
    }
    std::optional<Pick> best;
    for (const auto& planet : snapshot.planets()) {
        if (planet.owner != raider) continue;
        const auto* surface = control_row(snapshot, planet.id);
        if (!surface) continue;
        for (const auto& force : surface->landed) {
            if (force.owner != raider || cargo(snapshot, force.id) == 0) continue;
            for (const auto target : targets) {
                auto path = shortest_path(snapshot, planet.id, target);
                if (path.size() >= 2 && (!best || path.size() < best->path.size()))
                    best = Pick{planet.id, target, force.id, std::move(path)};
            }
            break; // one transport per source is enough
        }
    }
    return best;
}

struct Trace {
    bool launched{}, ordered{}, raid_retained{}, raid_conflict{}, orbital_conflict{}, resolved{};
    gc::PlayerId winner{};
    std::string state_hash, log;
};

Trace run(cp::CampaignRoot& root, const Pick& pick, gc::PlayerId raider, std::size_t workers) {
    Trace trace; std::ostringstream log;
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto& game = *root.game();
    std::uint64_t sequence = 0;
    const gc::SeatId seat{raider};
    const auto send = [&](gc::CommandPayload payload) {
        auto sent = game.submit({{game.completed_tick(), seat, ++sequence}, std::move(payload)});
        if (!sent) log << "  submit refused: " << sent.error().message << '\n';
        return static_cast<bool>(sent);
    };
    // Launch the one loaded transport into a new fleet at slot 0 (a raid force holds no warships).
    trace.launched = send(gc::LaunchFromSurface{{pick.transport}, 0, 0});
    gc::FleetId raid{};
    std::optional<eawr::sim::EntityId> voted;
    for (unsigned tick = 0; tick < 3000; ++tick) {
        auto stepped = game.step(executor);
        if (!stepped) { log << "  step failed: " << stepped.error().message << '\n'; break; }
        const auto& galaxy = *stepped.value().galaxy;
        if (!raid) {
            for (const auto& fleet : galaxy.fleets())
                if (fleet.owner == raider && fleet.orbit == pick.source && !fleet.motion.transit
                    && std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& m) { return m.id == pick.transport; })) {
                    raid = fleet.id;
                    trace.ordered = send(gc::MoveFleet{raid, pick.path, {}, true});
                    log << "  tick " << stepped.value().completed_tick << ": raid fleet " << raid << " ordered=" << trace.ordered << '\n';
                }
            continue;
        }
        if (const auto* fleet = fleet_row(galaxy, raid); fleet && fleet->raid_destination == pick.target) trace.raid_retained = true;
        if (const auto view = game.pending_conflict_view()) {
            if (view->conflict.planet == pick.target) {
                if (view->conflict.layer == cp::BattleLayer::land && view->conflict.raid) trace.raid_conflict = true;
                if (view->conflict.layer == cp::BattleLayer::space) trace.orbital_conflict = true;
            }
            // The involved solo player chooses auto-resolve in the battle dialog.
            if (view->choice_ready && voted != view->conflict.id) {
                voted = view->conflict.id;
                send(gc::BattleChoice{view->conflict.id, false});
                log << "  tick " << stepped.value().completed_tick << ": auto-resolve chosen for conflict " << view->conflict.id
                    << " layer=" << (view->conflict.layer == cp::BattleLayer::land ? "land" : "space") << " raid=" << view->conflict.raid << '\n';
            }
        }
        if (const auto outcome = game.latest_auto_resolve_outcome(); outcome && outcome->land && !trace.resolved) {
            trace.resolved = true; trace.winner = outcome->result.winner;
            log << "  tick " << stepped.value().completed_tick << ": land auto-resolve winner=" << trace.winner
                << " owner_now=" << planet_owner(galaxy, pick.target) << '\n';
        }
        if (trace.resolved && !game.pending_conflict_view() && !game.auto_resolve_needs_service()) break;
    }
    trace.state_hash = game.state_sha256();
    trace.log = log.str();
    return trace;
}

void installed() {
    const auto root_dir = eawr::testing::game_root();
    if (!root_dir) { eawr::testing::skip_installed("SKIPPED: stock raid needs EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root_dir) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = take(eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog));
    auto types = eawr::units::galactic_type_table(catalog.catalog);
    // The stock raid policy names the raiding faction (Raid_Force_Required_Faction); the test follows the data.
    const auto* required = data.constant("Raid_Force_Required_Faction");
    expect(required != nullptr, "installed data authors a raid faction");
    if (!required) return;
    std::string raider_name(required->text);
    raider_name.erase(0, raider_name.find_first_not_of(" \t\r\n"));
    raider_name.erase(raider_name.find_last_not_of(" \t\r\n") + 1);
    const std::string name = "Sandbox_Equal_Footing_Underworld";
    cp::CampaignSetupOptions options; options.seed = 42;
    gc::PlayerId raider{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool local = faction.name == raider_name;
        if (local) { options.solo_reference_human = id; raider = id; }
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
    }
    expect(raider != 0, "the raid faction is a player of the stock campaign");
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto build = [&] {
        auto planned = take(cp::plan_live_campaign_setup(filesystem, data, types, name, options, "Normal_Default"));
        auto services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, planned, "Normal_Default"));
        auto created = take(cp::CampaignRoot::create(std::move(planned), services));
        const auto installed_services = cp::install_live_conflict_adapters(*created.game(), filesystem, catalog.catalog, data,
            types, *created.plan(), [](const auto&) {
                eawr::core::Diagnostic error; error.message = "auto-resolve must not request tactical content";
                return eawr::core::Result<eawr::sim::tactical::TacticalContent>::failure(std::move(error));
            });
        if (!installed_services) throw std::runtime_error(eawr::core::format_diagnostic(installed_services.error()));
        return created;
    };
    auto probe = build();
    const auto pick = choose(*probe.game()->galaxy().snapshot(), raider);
    expect(pick.has_value(), "the raider has a loaded landed transport and a defended enemy planet in reach");
    if (!pick) return;
    std::cout << "raid source=" << pick->source << " target=" << pick->target << " transport=" << pick->transport
              << " hops=" << pick->path.size() - 1 << '\n';
    std::optional<std::string> hash;
    for (const std::size_t workers : {1U, 4U}) {
        auto root = build();
        const auto trace = run(root, *pick, raider, workers);
        std::cout << "workers=" << workers << '\n' << trace.log;
        expect(trace.launched && trace.ordered, "the raid force launches and its raid order is accepted");
        expect(trace.raid_retained, "WGM-37: the moving fleet retains its raid destination");
        expect(trace.raid_conflict, "WAR-14: the raid's land conflict is admitted as a raid");
        expect(!trace.orbital_conflict, "WGM-34/39: a raid lands without an orbital conflict at its target");
        expect(trace.resolved && trace.winner != 0, "C3: the raid's land conflict auto-resolves like an invasion");
        if (!hash) hash = trace.state_hash;
        else expect(*hash == trace.state_hash, "the raid outcome is equal on 1 and 4 workers");
    }
}
} // namespace

int case_galactic_raid_live_tests() {
    try { installed(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

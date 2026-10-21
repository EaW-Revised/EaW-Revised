#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
// #3189: an invasion won by auto-resolve must hand the planet to the invader.
//
// Drives the installed Sandbox_Equal_Footing_Underworld campaign through the ordinary live path (real
// movement, the stock conflict services, the stock auto-resolver and the political-control service):
// an Empire fleet with a loaded transport leaves its planet, arrives at an enemy planet that has
// ground defenders, any space conflict is auto-resolved, the transports are landed and the land
// conflict is auto-resolved. WAR-56/62 + WGM-65..75: ownership follows the surviving surface presence.
#include "galactic_support.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/graph.hpp"
#include "eawr/sim/galactic/production.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;

using eawr::testing::result_diagnostic::take;
std::optional<std::string> game_root() {
    return eawr::testing::game_root();
}

struct Candidate {
    gc::PlanetId source{};
    std::vector<gc::FleetId> fleets; // every attacker fleet in orbit at the source; the first absorbs the rest
    std::vector<eawr::sim::EntityId> troops; // landed loaded transports launched into the fleet at the start
    gc::PlanetId target{};
    std::vector<gc::PlanetId> path;
    bool two_wave{}; // the warfleet fights first; the troops follow in a second fleet
};
struct Trace {
    std::string log;
    std::string state_hash;
    bool arrived{}, space_resolved{}, space_won{}, land_resolved{}, land_won{}, reverted{};
    gc::PlayerId observed_known{};
    gc::PlayerId owner_after{};
    std::size_t ticks{};
};

auto elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

const gc::ControlPlanet* control_row(const gc::GalacticSnapshot& snapshot, gc::PlanetId id) {
    if (!snapshot.control()) return nullptr;
    for (const auto& row : snapshot.control()->planets) if (row.planet == id) return &row;
    return nullptr;
}
gc::PlayerId planet_owner(const gc::GalacticSnapshot& snapshot, gc::PlanetId id) {
    for (const auto& row : snapshot.planets()) if (row.id == id) return row.owner;
    return 0;
}
const gc::Fleet* fleet_row(const gc::GalacticSnapshot& snapshot, gc::FleetId id) {
    for (const auto& row : snapshot.fleets()) if (row.id == id) return &row;
    return nullptr;
}
const gc::ObservedPlanet* observation(const gc::GalacticSnapshot& snapshot, gc::PlayerId player, gc::PlanetId planet) {
    for (const auto& row : snapshot.observations()) if (row.player == player && row.planet == planet) return &row;
    return nullptr;
}
std::string describe_observation(const gc::GalacticSnapshot& snapshot, gc::PlayerId player, gc::PlanetId planet) {
    std::ostringstream text;
    if (const auto* row = observation(snapshot, player, planet)) {
        text << "observed discovered=" << row->discovered << " ownership_known=" << row->knowledge.ownership
        << " known_owner=" << (row->known_owner ? static_cast<long long>(*row->known_owner) : -1)
             << " production_known=" << row->knowledge.production << " ground_known=" << row->knowledge.ground_contents
             << " apparent_owner=" << (row->apparent_owner ? static_cast<long long>(*row->apparent_owner) : -1);
    } else text << "no observation row";
    return text.str();
}
bool loaded_transport(const gc::GalacticSnapshot& snapshot, const gc::Fleet& fleet) {
    if (!snapshot.fleet_control() || !snapshot.movement()) return false;
    for (const auto& member : fleet.members) {
        const auto& types = snapshot.fleet_control()->types;
        const auto trait = std::find_if(types.begin(), types.end(), [&](const auto& row) { return row.type == member.type; });
        if (trait == types.end() || !trait->transport) continue;
        const auto& contents = snapshot.movement()->contents;
        const auto cargo = std::find_if(contents.begin(), contents.end(), [&](const auto& row) { return row.container == member.id; });
        if (cargo != contents.end() && !cargo->children.empty()) return true;
    }
    return false;
}

// Shortest jump path over the actual movement graph, including proximity links.
std::vector<gc::PlanetId> shortest_path(const gc::GalacticSnapshot& snapshot, gc::PlanetId from, gc::PlanetId to) {
    if (!snapshot.maximum_movement_distance()) throw std::runtime_error("capture route requires sourced movement range");
    const auto graph = take(gc::PlanetGraph::build(snapshot.planets(), snapshot.routes(),
        *snapshot.maximum_movement_distance()));
    std::map<gc::PlanetId, gc::PlanetId> parent{{from, from}};
    std::deque<gc::PlanetId> queue{from};
    while (!queue.empty()) {
        const auto at = queue.front(); queue.pop_front();
        if (at == to) break;
        for (const auto& edge : graph.node(at)->edges) {
            const auto next = edge.to;
            if (next && !parent.contains(next)) { parent.emplace(next, at); queue.push_back(next); }
        }
    }
    if (!parent.contains(to)) return {};
    std::vector<gc::PlanetId> path;
    for (auto at = to; at != from; at = parent.at(at)) path.push_back(at);
    path.push_back(from);
    std::reverse(path.begin(), path.end());
    return path;
}

bool has_cargo(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId container) {
    if (!snapshot.movement()) return false;
    for (const auto& row : snapshot.movement()->contents) if (row.container == container) return !row.children.empty();
    return false;
}

std::vector<Candidate> choose_candidates(const gc::GalacticSnapshot& snapshot, gc::PlayerId attacker, std::ostream& note,
    bool neutral_targets) {
    std::vector<Candidate> out;
    std::vector<gc::PlanetId> enemy_targets;
    for (const auto& planet : snapshot.planets()) {
        if (planet.owner == attacker || !planet.owner) continue;
        const gc::ControlPlayer* player = nullptr;
        if (snapshot.control()) for (const auto& row : snapshot.control()->players) if (row.player == planet.owner) player = &row;
        if (!player || player->neutral != neutral_targets || (!neutral_targets && !player->playable)) continue;
        const auto* surface = control_row(snapshot, planet.id);
        if (!surface || !surface->surface_accessible) continue;
        if (!neutral_targets && surface->landed.empty() && !surface->ground_base.level) continue;
        enemy_targets.push_back(planet.id);
    }
    std::set<gc::PlanetId> seen_sources;
    for (const auto& fleet : snapshot.fleets()) {
        if (fleet.owner != attacker || fleet.motion.transit || !seen_sources.insert(fleet.orbit).second) continue;
        const auto* surface = control_row(snapshot, fleet.orbit);
        if (!surface) continue;
        Candidate row; row.source = fleet.orbit;
        for (const auto& other : snapshot.fleets())
            if (other.owner == attacker && other.orbit == fleet.orbit && !other.motion.transit) row.fleets.push_back(other.id);
        for (const auto& force : surface->landed)
            if (force.owner == attacker && has_cargo(snapshot, force.id)) row.troops.push_back(force.id);
        if (row.troops.empty()) continue;
        std::vector<gc::PlanetId> best;
        for (const auto target : enemy_targets) {
            auto path = shortest_path(snapshot, fleet.orbit, target);
            if (path.size() >= 2 && (best.empty() || path.size() < best.size())) { best = std::move(path); row.target = target; }
        }
        if (best.empty()) continue;
        row.path = std::move(best);
        out.push_back(std::move(row));
    }
    note << "attacker sources with fleets and loaded landed transports=" << out.size() << " defended enemy planets=" << enemy_targets.size() << '\n';
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path.size() < b.path.size(); });
    return out;
}

Trace run(cp::CampaignRoot& root, const Candidate& pick, gc::PlayerId attacker, gc::SeatId seat, std::size_t workers) {
    Trace trace; std::ostringstream log;
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto& game = *root.game();
    std::uint64_t sequence = 0;
    const auto send = [&](gc::CommandPayload payload) {
        auto sent = game.submit({{game.completed_tick(), seat, ++sequence}, std::move(payload)});
        if (!sent) log << "  submit refused: " << sent.error().message << '\n';
        return static_cast<bool>(sent);
    };
    // The loaded landed transports join the biggest warfleet in orbit, then the merged fleet moves.
    const gc::FleetId lead = pick.fleets.front();
    for (std::size_t i = 1; i < pick.fleets.size(); ++i)
        if (!send(gc::MergeFleets{pick.fleets[i], lead})) { trace.log = log.str(); return trace; }
    if (!pick.two_wave && !send(gc::LaunchFromSurface{pick.troops, lead, 0})) { trace.log = log.str(); return trace; }
    {
        auto launched = game.step(executor);
        if (!launched) { log << "  launch step failed: " << launched.error().message << '\n'; trace.log = log.str(); return trace; }
        const auto* joined = fleet_row(*launched.value().galaxy, lead);
        std::size_t aboard = 0;
        if (joined) for (const auto id : pick.troops)
            aboard += std::any_of(joined->members.begin(), joined->members.end(), [&](const auto& m) { return m.id == id; });
        log << "  assembled: troops aboard=" << aboard << "/" << pick.troops.size() << " fleet members=" << (joined ? joined->members.size() : 0) << '\n';
        for (const auto& diagnostic : launched.value().diagnostics) log << "  diagnostic: " << diagnostic.message << '\n';
        for (const auto& event : launched.value().galaxy->events())
            if (event.kind == gc::GalacticEventKind::command_rejected) log << "  rejected reason=" << static_cast<int>(event.reason) << '\n';
        if ((!aboard && !pick.two_wave) || !joined) { trace.log = log.str(); return trace; }
    }
    if (!send(gc::MoveFleet{lead, pick.path, {}})) { trace.log = log.str(); return trace; }
    const gc::PlanetId target = pick.target;
    std::shared_ptr<const cp::AutoResolveOutcomeApplication> seen;
    bool landing_sent = false, voted = false, wave2_launched = false;
    gc::FleetId lander = pick.two_wave ? gc::FleetId{} : lead; // the fleet whose transports invade
    std::uint64_t land_tick = 0, arrived_tick = 0, captured_tick = 0, landing_tick = 0, lander_arrived_tick = 0;
    constexpr std::size_t max_ticks = 3000, settle_ticks = 40, hold_ticks = 1500;
    const auto stepping_started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < max_ticks; ++i) {
        auto stepped = game.step(executor);
        if (!stepped) { log << "  step failed at " << game.completed_tick() << ": " << stepped.error().message << '\n'; break; }
        const auto& tick = stepped.value();
        trace.ticks = i + 1; trace.state_hash = tick.state_sha256;
        for (const auto& diagnostic : tick.diagnostics) log << "  diagnostic: " << diagnostic.message << '\n';
        const auto& galaxy = *tick.galaxy;
        const auto* fleet = fleet_row(galaxy, lead);
        if (!trace.arrived && fleet && fleet->orbit == target && !fleet->motion.transit) {
            trace.arrived = true; arrived_tick = tick.completed_tick;
            const auto* surface = control_row(galaxy, target);
            log << "  tick " << tick.completed_tick << ": fleet arrived; owner=" << planet_owner(galaxy, target)
                << " landed=" << (surface ? surface->landed.size() : 0) << " ground_base=" << (surface ? surface->ground_base.level : 0)
                << " starbase=" << (surface ? surface->starbase.level : 0) << '\n';
        }
        if (const auto view = game.pending_conflict_view(); view && view->choice_ready && !voted
            && (view->arbitration.route == cp::ConflictRoute::vote || view->arbitration.route == cp::ConflictRoute::pending_tactical)) {
            voted = send(gc::BattleChoice{view->conflict.id, false});
            log << "  tick " << tick.completed_tick << ": vote auto-resolve for conflict " << view->conflict.id << '\n';
        }
        if (const auto outcome = game.latest_auto_resolve_outcome(); outcome && outcome != seen) {
            seen = outcome; voted = false;
            const bool won = outcome->result.winner == attacker;
            log << "  tick " << tick.completed_tick << ": auto-resolve " << (outcome->land ? "LAND" : "SPACE")
                << " winner=" << outcome->result.winner << " attacker_won=" << won
                << " land_invaders=" << outcome->land_invaders.size() << " destroyed=" << outcome->destroyed.size()
                << " owner_now=" << planet_owner(galaxy, target) << '\n';
            {
                std::size_t own_deaths = 0, troop_deaths = 0;
                for (const auto& death : outcome->deaths) own_deaths += death.owner == attacker;
                for (const auto id : pick.troops)
                    troop_deaths += std::binary_search(outcome->destroyed.begin(), outcome->destroyed.end(), id);
                log << "    attacker deaths=" << own_deaths << " troop transports destroyed=" << troop_deaths << "/" << pick.troops.size()
                    << " retreat_player=" << outcome->result.retreat_player.value_or(0) << " retreat_fleets=" << outcome->retreat_fleets.size() << '\n';
            }
            if (outcome->land) { trace.land_resolved = true; trace.land_won = won; land_tick = tick.completed_tick; }
            else { trace.space_resolved = true; trace.space_won = won; }
            if (!outcome->land && !won) break; // the invasion force was beaten in space
        }
        for (const auto& event : galaxy.events()) {
            if (event.kind == gc::GalacticEventKind::movement_conflict || event.kind == gc::GalacticEventKind::land_conflict_requested
                || event.kind == gc::GalacticEventKind::conflict_pending || event.kind == gc::GalacticEventKind::combat_imminent
                || event.kind == gc::GalacticEventKind::planet_owner_changed || event.kind == gc::GalacticEventKind::command_rejected)
                log << "  tick " << tick.completed_tick << ": event kind=" << static_cast<int>(event.kind) << " subject=" << event.subject
                    << " value=" << event.value << " reason=" << static_cast<int>(event.reason) << '\n';
        }
        if (trace.arrived && (i % 250 == 0 || tick.completed_tick < arrived_tick + 12)) {
            const auto view = game.pending_conflict_view();
            const auto progress = game.auto_resolve_progress();
            log << "  tick " << tick.completed_tick << " (galaxy tick " << galaxy.completed_tick() << "): pending=" << view.has_value();
            if (view) log << " route=" << static_cast<int>(view->arbitration.route) << " ready=" << view->choice_ready
                << " forced=" << view->arbitration.forced_tactical << " layer=" << static_cast<int>(view->conflict.layer)
                << " forces=" << view->conflict.forces.size();
            log << " needs_service=" << game.auto_resolve_needs_service() << " progress=" << progress.has_value();
            if (progress) log << " initiated=" << progress->initiated << " elapsed=" << progress->elapsed_mode_seconds.raw();
            log << " paused=" << galaxy.clock().pause_depth << '\n';
        }
        // Second wave (starbase/fleet first, troops after): the warfleet won the space battle, so the
        // troops launch from the home surface into a fleet of their own and follow.
        if (pick.two_wave && !wave2_launched && trace.arrived && trace.space_won && !game.pending_conflict_view()
            && !game.auto_resolve_needs_service()) {
            wave2_launched = send(gc::LaunchFromSurface{pick.troops, 0, 0});
            log << "  tick " << tick.completed_tick << ": troops launched (accepted=" << wave2_launched << ")\n";
        }
        if (pick.two_wave && wave2_launched && !lander) {
            for (const auto& row : galaxy.fleets()) {
                if (row.owner != attacker || row.orbit != pick.source || row.motion.transit) continue;
                const bool aboard = std::any_of(row.members.begin(), row.members.end(),
                    [&](const auto& m) { return m.id == pick.troops.front(); });
                if (aboard) { lander = row.id; break; }
            }
            if (lander && send(gc::MoveFleet{lander, pick.path, {}}))
                log << "  tick " << tick.completed_tick << ": troop fleet " << lander << " moves out\n";
        }
        const auto* lander_fleet = lander ? fleet_row(galaxy, lander) : nullptr;
        if (lander_fleet && !lander_arrived_tick && lander_fleet->orbit == target && !lander_fleet->motion.transit) {
            lander_arrived_tick = tick.completed_tick;
            log << "  tick " << tick.completed_tick << ": troop fleet arrived\n";
        }
        // Once the arrival raised no space conflict, or a space conflict has been resolved,
        // the invasion is the ordinary LandFleet order (WGM-39/42).
        if (lander_arrived_tick && !landing_sent && tick.completed_tick >= lander_arrived_tick + 10
            && !game.auto_resolve_needs_service() && !game.pending_conflict_view()
            && lander_fleet && lander_fleet->orbit == target && !lander_fleet->motion.transit
            && loaded_transport(galaxy, *lander_fleet) && planet_owner(galaxy, target) != attacker) {
            landing_sent = send(gc::LandFleet{lander, target});
            landing_tick = tick.completed_tick;
            log << "  tick " << tick.completed_tick << ": LandFleet sent (accepted=" << landing_sent << ")\n";
        }
        // An uncontested landing raises no conflict: give the control service a moment, then stop.
        if (landing_sent && !trace.land_resolved && !captured_tick && !game.pending_conflict_view()
            && !game.auto_resolve_needs_service() && tick.completed_tick >= landing_tick + 60) break;
        if (trace.land_resolved && !captured_tick && tick.completed_tick >= land_tick + settle_ticks) break;
        if (landing_sent && !captured_tick && planet_owner(galaxy, target) == attacker) {
            captured_tick = tick.completed_tick;
            log << "  tick " << tick.completed_tick << ": planet changed hands; "
                << describe_observation(galaxy, attacker, target) << '\n';
        }
        if (captured_tick) {
            const auto owner_now = planet_owner(galaxy, target);
            if (owner_now != attacker && !trace.reverted) {
                trace.reverted = true;
                log << "  tick " << tick.completed_tick << ": REVERTED to owner " << owner_now << '\n';
            }
            if ((tick.completed_tick - captured_tick) % 200 == 0) {
                const auto* surface = control_row(galaxy, target);
                log << "  tick " << tick.completed_tick << ": owner=" << owner_now << " landed=" << (surface ? surface->landed.size() : 0) << '\n';
            }
            if (tick.completed_tick >= captured_tick + hold_ticks) break;
        }
    }
    log << "  capture timing stepping_ms=" << elapsed_ms(stepping_started) << " ticks=" << trace.ticks << '\n';
    const auto checks_started = std::chrono::steady_clock::now();
    const auto snapshot = game.galaxy().snapshot();
    trace.owner_after = planet_owner(*snapshot, target);
    trace.state_hash = game.state_sha256();
    if (const auto* lead_fleet = fleet_row(*snapshot, lead)) {
        log << "  lead fleet at orbit=" << lead_fleet->orbit << " transit=" << lead_fleet->motion.transit.has_value()
            << " members=" << lead_fleet->members.size() << " loaded=" << loaded_transport(*snapshot, *lead_fleet)
            << " landing_sent=" << landing_sent << '\n';
    } else log << "  lead fleet is gone (landing_sent=" << landing_sent << ")\n";
    log << "  end: owner=" << trace.owner_after << " ticks=" << trace.ticks << " "
        << describe_observation(*snapshot, attacker, target) << '\n';
    if (const auto* row = observation(*snapshot, attacker, target)) {
        trace.observed_known = row->known_owner.value_or(0);
        if (trace.owner_after == attacker)
            expect(row->knowledge.production && row->knowledge.ground_contents && row->knowledge.ground_count,
                "pre-fix symptom: captured installed planet is owned but its mask lacks production/ground contents");
    }
    if (trace.owner_after == attacker && snapshot->production()) {
        const auto producer = std::find_if(snapshot->production()->planets.begin(), snapshot->production()->planets.end(),
            [&](const auto& row) { return row.planet == target; });
        expect(producer != snapshot->production()->planets.end() && producer->target_owner == attacker
            && producer->ground_slots > 0, "captured installed planet retains new-owner build slots");
        if (producer == snapshot->production()->planets.end()) throw std::runtime_error("captured producer is absent");
        expect(std::any_of(snapshot->production_offers().begin(), snapshot->production_offers().end(), [&](const auto& row) {
            return row.player == attacker && row.planet == target && row.domain == gc::ProductionDomain::ground
                && row.admission == gc::ProductionAdmission::allowed;
        }), "captured installed planet publishes an admitted ground build offer");
        auto reference = *snapshot->production(); reference.technology_types.clear();
        const auto& player = *std::find_if(reference.players.begin(), reference.players.end(),
            [&](const auto& row) { return row.player == attacker; });
        const auto& account = *std::find_if(snapshot->player_economy()->players.begin(), snapshot->player_economy()->players.end(),
            [&](const auto& row) { return row.player == attacker; });
        const auto builder = std::find_if(root.plan()->player_bindings().begin(), root.plan()->player_bindings().end(),
            [&](const auto& row) { return row.player.player_id == attacker; });
        if (builder == root.plan()->player_bindings().end()) throw std::runtime_error("captured builder is absent");
        std::size_t structures{}, foreign{}, changed{};
        for (const auto& row : snapshot->production_offers()) {
            if (row.player != attacker || row.planet != target || row.domain != gc::ProductionDomain::ground
                || row.kind != gc::ProductionKind::structure) continue;
            auto original = *std::find_if(producer->offers.begin(), producer->offers.end(),
                [&](const auto& offer) { return offer.type == row.type; });
            const auto* authored = root.plan()->original_type(row.type);
            expect(authored && builder != root.plan()->player_bindings().end(), "captured offer binds original type and builder");
            if (!authored || builder == root.plan()->player_bindings().end()) continue;
            const bool affiliated = std::find(authored->affiliation.begin(), authored->affiliation.end(), builder->faction)
                != authored->affiliation.end();
            changed += original.affiliated != affiliated;
            expect(row.price.has_value() && row.seconds.has_value(), "captured structure has prepared price and duration");
            if (!row.price || !row.seconds) continue;
            original.affiliated = affiliated; original.price = *row.price; original.seconds = *row.seconds;
            const auto admission = gc::production_admission({reference, *producer, player, account,
                snapshot->fleets(), attacker}, &original, gc::ProductionDomain::ground, true);
            // WGUI-170: the menu omits population/resource limits, while the purchase retains them.
            const bool expected = admission == gc::ProductionAdmission::allowed
                || admission == gc::ProductionAdmission::population || admission == gc::ProductionAdmission::resource_limit;
            expect(row.current == expected, "captured structure list equals independently rebound XML affiliation and live gates");
            if (!affiliated) {
                ++foreign;
                expect(!row.current && row.admission == gc::ProductionAdmission::unavailable,
                    "previous-faction structure is hidden and cannot be purchased");
            }
            if (row.current) ++structures;
        }
        expect(structures > 0 && foreign > 0 && changed > 0,
            "installed conquest covers both new-owner structures and stale previous-owner flags");
        log << "  build-list: faction=" << builder->faction << " current structures=" << structures
            << " foreign refused=" << foreign << " rebound flags=" << changed << '\n';
    }
    if (const auto* surface = control_row(*snapshot, target)) {
        if (trace.owner_after == attacker) {
            expect(!surface->landed.empty() && std::all_of(surface->landed.begin(), surface->landed.end(),
                [&](const auto& force) { return force.owner == attacker; }), "captured installed surface has invading ground ownership");
            for (const auto& force : surface->landed)
                expect(std::none_of(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) {
                    return std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == force.id; });
                }), "landed installed invaders leave orbit without duplicated force identities");
        }
        log << "  surface: landed=" << surface->landed.size() << " ground_base=" << surface->ground_base.level << "/" << surface->ground_base.owner
            << " starbase=" << surface->starbase.level << "/" << surface->starbase.owner << '\n';
        for (const auto& force : surface->landed) log << "    landed " << force.id << " owner=" << force.owner << '\n';
    }
    if (snapshot->control()) for (const auto& state : snapshot->control()->state) if (state.planet == target)
        log << "  control: current_player=" << state.current_player << " target_player=" << state.target_player
            << " controller=" << state.controller << " current=" << state.current.raw() << " target=" << state.target.raw() << '\n';
    log << "  capture timing checks_ms=" << elapsed_ms(checks_started) << '\n';
    trace.log = log.str();
    return trace;
}

void installed(std::string_view attacker_name = "Empire", bool build_list_only = false) {
    const auto root_dir = game_root();
    if (!root_dir) { eawr::testing::skip_installed("SKIPPED: stock capture return needs EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root_dir) / folder / "Data")).mount);
    std::filesystem::path mod_root, mod_file;
    const auto cleanup = std::shared_ptr<void>(nullptr, [&](void*) {
        if (mod_file.empty()) return;
        std::error_code error;
        std::filesystem::remove(mod_file, error);
        auto directory = mod_file.parent_path();
        while (directory != mod_root) {
            std::filesystem::remove(directory, error); directory = directory.parent_path();
        }
        std::filesystem::remove(mod_root, error);
    });
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = take(eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog));
    auto types = eawr::units::galactic_type_table(catalog.catalog);
    if (attacker_name == "RenamedBuilder") {
        // Register a catalog alias too: corruption/production authoring reads its eligibility and factors.
        const auto* faction_original = catalog.catalog.find("Empire", eawr::data::Category::faction);
        if (!faction_original) throw std::runtime_error("rename fixture requires the installed builder faction");
        const auto logical = take(eawr::vfs::canonicalize(faction_original->root.source.logical_path));
        const auto bytes = take(filesystem.open(logical));
        std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto at = xml.find("<Faction Name=\"Empire\"");
        if (at == std::string::npos) throw std::runtime_error("rename fixture requires the authored faction name");
        const auto end = xml.find("</Faction>", at);
        if (end == std::string::npos) throw std::runtime_error("rename fixture requires a complete faction record");
        auto alias = xml.substr(at, end + std::string_view("</Faction>").size() - at);
        alias.replace(alias.find("Name=\"Empire\""), std::string_view("Name=\"Empire\"").size(), "Name=\"RenamedBuilder\"");
        // Keep the original faction registered for unrelated authored market/item references.
        xml.insert(end + std::string_view("</Faction>").size(), alias);
        mod_root = std::filesystem::current_path() / "out" /
            ("capture-faction-mod-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        mod_file = mod_root / logical;
        std::filesystem::create_directories(mod_file.parent_path());
        { std::ofstream output(mod_file, std::ios::binary); output << xml;
            if (!output) throw std::runtime_error("could not write renamed faction fixture"); }
        mounts.insert(mounts.begin(), {"renamed-fixture", mod_root / "data", "data", {}});
        filesystem = take(eawr::vfs::Vfs::mount(mounts));
        catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
        expect(catalog.catalog.find("RenamedBuilder", eawr::data::Category::faction) != nullptr,
            "synthetic faction name reaches the actual registered faction catalog");
        // Synthetic faction authoring over installed types: no retail faction identity reaches admission.
        const auto rename = [](std::string& value) { if (value == "Empire") value = "RenamedBuilder"; };
        for (auto& faction : data.factions) {
            rename(faction.name); rename(faction.primary_enemy);
            for (auto& ally : faction.allies) rename(ally);
            for (auto& enemy : faction.enemies) rename(enemy);
        }
        for (auto& type : types.types) for (auto& faction : type.affiliation) rename(faction);
        for (auto& campaign : data.campaigns) {
            rename(campaign.starting_active_player);
            for (auto& row : campaign.home_locations) rename(row.faction);
            for (auto& row : campaign.starting_credits) rename(row.faction);
            for (auto& row : campaign.starting_tech_levels) rename(row.faction);
            for (auto& row : campaign.max_tech_levels) rename(row.faction);
            for (auto& row : campaign.starting_forces) rename(row.faction);
            for (auto& row : campaign.ai_player_control) rename(row.faction);
            for (auto& row : campaign.markup_filenames) rename(row.faction);
            for (auto& row : campaign.special_case_production) rename(row.faction);
        }
    }
    const std::string name = "Sandbox_Equal_Footing_Underworld";
    cp::CampaignSetupOptions options; options.seed = 42;
    gc::PlayerId empire{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool local = faction.name == attacker_name;
        if (local) { options.solo_reference_human = id; empire = id; }
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto build = [&](const eawr::data::galactic::GalacticData& authored) {
        auto planned = take(cp::plan_live_campaign_setup(filesystem, authored, types, name, options, "Normal_Default"));
        auto services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog, authored, types, planned, "Normal_Default"));
        auto created = take(cp::CampaignRoot::create(std::move(planned), services));
        const auto installed_services = cp::install_live_conflict_adapters(*created.game(), filesystem, catalog.catalog, authored,
            types, *created.plan(), [](const auto&) {
                eawr::core::Diagnostic error; error.message = "auto-resolve must not request tactical content";
                return eawr::core::Result<eawr::sim::tactical::TacticalContent>::failure(std::move(error));
            });
        if (!installed_services) throw std::runtime_error(eawr::core::format_diagnostic(installed_services.error()));
        return created;
    };
    // Authored variants of the stock campaign: the enemy's space forces are removed (as after a won space
    // battle), optionally with its ground companies too, leaving only structures to defend the surface.
    const auto has = [&](const std::string& type, const char* behavior) {
        const auto* row = types.find(type);
        return row && std::find(row->behavior.begin(), row->behavior.end(), behavior) != row->behavior.end();
    };
    const auto variant = [&](bool strip_companies, bool keep_starbase = false, bool keep_one_ship = false) {
        auto copy = data;
        std::set<std::string> kept;
        for (auto& campaign : copy.campaigns) if (campaign.name == name)
            std::erase_if(campaign.starting_forces, [&](const auto& row) {
                if (row.faction == attacker_name) return false;
                if (has(row.type, "DUMMY_GROUND_COMPANY")) return strip_companies;
                if (keep_starbase && has(row.type, "DUMMY_STAR_BASE")) return false;
                if (keep_one_ship && (has(row.type, "DUMMY_STARSHIP") || has(row.type, "DUMMY_SPACE_FIGHTER_SQUADRON")))
                    return !kept.insert(row.faction + "|" + row.location).second;
                return !(has(row.type, "DUMMY_GROUND_STRUCTURE") || has(row.type, "DUMMY_GROUND_BASE"));
            });
        return copy;
    };
    std::size_t land_battles = 0, captured = 0;
    for (const int mode : build_list_only ? std::vector<int>{1} : std::vector<int>{0, 1, 3}) {
        const auto mode_started = std::chrono::steady_clock::now();
        // 0: space forces gone, companies stay; 1: space forces and companies gone;
        // 3: one weak ship per enemy planet and ground structures: a won space battle, then a second
        // wave of troops lands (the troops stay home so the space battle cannot take them).
        const bool strip_companies = mode != 0;
        const auto authored = variant(strip_companies, false, mode == 3);
        auto probe = build(authored);
        std::ostringstream note;
        auto candidates = choose_candidates(*probe.game()->galaxy().snapshot(), empire, note, false);
        // This installed planet starts Underworld-owned in Equal Footing for either capturer.
        // Kuat starts Empire-owned in this authored family, so it cannot reproduce an Empire conquest.
        const auto fondor = std::find_if(probe.plan()->locations().begin(), probe.plan()->locations().end(),
            [](const auto& row) { return row.original_type == "Fondor" && row.planet.has_value(); });
        expect(fondor != probe.plan()->locations().end(), "the stock reproduction includes its selected capture target");
        if (fondor != probe.plan()->locations().end()) {
            const auto target = fondor->planet->id;
            expect(planet_owner(*probe.game()->galaxy().snapshot(), target) != empire, "the target starts available for builder conquest");
            if (build_list_only) {
                const auto underworld = std::find_if(options.players.begin(), options.players.end(),
                    [](const auto& player) { return player.faction == "Underworld"; });
                expect(underworld != options.players.end()
                    && planet_owner(*probe.game()->galaxy().snapshot(), target) == underworld->player.player_id,
                    "focused installed fixture starts controlled by Underworld");
            }
            for (auto& candidate : candidates) {
                candidate.target = target;
                candidate.path = shortest_path(*probe.game()->galaxy().snapshot(), candidate.source, target);
            }
            std::erase_if(candidates, [](const auto& row) { return row.path.size() < 2; });
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.path.size() < b.path.size(); });
        }
        std::cout << "variant mode=" << mode << ' ' << note.str();
        expect(!candidates.empty(), "the builder has a loaded landed transport, an orbital fleet and a defended enemy target");
        for (std::size_t n = 0; n < candidates.size() && n < (build_list_only ? 1U : 2U); ++n) {
            std::optional<std::string> hash; std::optional<gc::PlayerId> owner;
            const std::vector<std::size_t> worker_counts = !build_list_only && (mode == 1 || mode == 3)
                ? std::vector<std::size_t>{1, 2, 4, 8} : std::vector<std::size_t>{1};
            for (const std::size_t workers : worker_counts) {
                std::cout << "capture timing begin mode=" << mode << " candidate=" << n
                    << " workers=" << workers << " mode_ms=" << elapsed_ms(mode_started) << std::endl;
                const auto setup_started = std::chrono::steady_clock::now();
                auto root = build(authored);
                const auto setup_ms = elapsed_ms(setup_started);
                auto pick = candidates[n]; pick.two_wave = mode == 3;
                const auto run_started = std::chrono::steady_clock::now();
                const auto trace = run(root, pick, empire, gc::SeatId{empire}, workers);
                std::cout << "capture timing end mode=" << mode << " candidate=" << n
                    << " workers=" << workers << " setup_ms=" << setup_ms
                    << " run_ms=" << elapsed_ms(run_started) << " ticks=" << trace.ticks
                    << " mode_ms=" << elapsed_ms(mode_started) << std::endl;
                std::cout << "attempt " << n << " source=" << candidates[n].source << " fleets=" << candidates[n].fleets.size()
                    << " troops=" << candidates[n].troops.size() << " target=" << candidates[n].target
                    << " hops=" << candidates[n].path.size() - 1 << " workers=" << workers << '\n' << trace.log;
                // Count each (variant, candidate) invasion once, independently of worker reruns.
                if (workers == 1 && trace.land_resolved) ++land_battles;
                if (mode == 3) expect(trace.space_won && trace.land_resolved && trace.land_won,
                    "a won space battle followed by a landing wins the land battle (starbase-then-landing)");
                if (trace.land_resolved && !trace.land_won)
                    expect(trace.owner_after != empire && trace.observed_known != empire, "a lost land battle changes nothing");
                if (trace.land_resolved && trace.land_won) {
                    expect(trace.owner_after == empire && !trace.reverted, "a land victory by auto-resolve hands the planet to the invader and keeps it (#3189)");
                    expect(trace.observed_known == empire, "the invader's own planet observation shows it as theirs (#3189)");
                    if (workers == 1 && trace.owner_after == empire) ++captured;
                }
                if (!hash) { hash = trace.state_hash; owner = trace.owner_after; }
                else expect(*hash == trace.state_hash && *owner == trace.owner_after, "capture outcome and hash equal across worker counts");
            }
        }
        std::cout << "capture timing mode=" << mode << " total_ms=" << elapsed_ms(mode_started) << std::endl;
    }
    std::cout << "land battles=" << land_battles << " captured=" << captured << '\n';
    expect(build_list_only ? land_battles >= 1 && captured >= 1 : land_battles >= 3 && captured >= 2,
        "stock invasions reach auto-resolved land battles and capture");
}
} // namespace

int case_galactic_capture_return_tests(int argc, const char* const argv[]) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--build-list") installed(argv[2], true);
        else installed();
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

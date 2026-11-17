#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "../presentation/galactic/stock_hero_place.hpp"
#include "galactic_support.hpp"
#include "galactic/map_input.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"

// #7357 / WGCH-126 / WGCH-197: on stock data, a solo human's space corruption choice (Piracy) gets a real mission battle.
// Part 1: the stock space entry prepares each campaign planet's space mission map; for every such choice the entry builds
// the queued mission conflict (the actor's owner invades, the planet's owner defends, no galactic force enters) and
// resolves it on the mission map, where the map places the invader's units.
// Part 2, the player's route: a corruption hero is dropped on a hostile planet with a space mission choice (the viewer's
// own drop preview and order), the published Corrupt Planet menu offers the choice available, executing it (the menu's
// order) starts the forced child battle on the mission map, and the child steps. The whole route runs on 1, 2, 4 and 8
// workers with equal state hashes. Every step prints what it saw: a failure names the first step that stopped.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;

constexpr std::string_view kFile = "campaigns_underworld_gc.xml";
// The route starts one hop from a hostile space mission planet: Bothawui (Consortium) borders Corellia (Empire, Piracy).
// A two-hop drop opens no menu yet (#7426); that case is the issue's repro, not this test's.
constexpr std::string_view kRouteCampaign = "Sandbox_Equal_Footing_Underworld";
constexpr std::string_view kRouteHome = "Bothawui";
constexpr std::string_view kCompany = "Underworld_Saboteur_Team";
// Piracy's unit requirement (Tyber Zann): the sandbox gives the Consortium no Tyber at the start.
constexpr std::string_view kRequiredCompany = "Tyber_Zann_Team";
// #7380 review / #7438: a mod whose space mission names a map that does not exist. The entry leaves that map out, so the
// battle can't be built and the choice takes the direct route instead (WGCH-199): offered, charged once, no battle, and the
// corruption applies after the timed transition.
constexpr std::string_view kPlanetFile = "planets.xml";
constexpr std::string_view kRouteMap = "_Space_Planet_Corellia_Piracy.ted";
constexpr std::string_view kMissingMap = "_Space_Planet_Corellia_Not_Shipped.ted";
constexpr std::string_view kMember = "Underworld_Saboteur";

struct Mission { gc::PlanetId planet{}; gc::CorruptionChoice choice; };

cp::CampaignSetupOptions campaign_options(const eawr::data::galactic::GalacticData& data, const eawr::units::GalacticTypeTable& types,
    std::string_view campaign) {
    const auto* source = data.campaign(campaign);
    if (!source) throw std::runtime_error("missing stock campaign " + std::string(campaign));
    cp::CampaignSetupOptions options; options.forced_seed = 1;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool human = faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, true});
        if (!faction.is_neutral.value_or(false))
            options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        if (human) options.solo_reference_human = id;
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    return options;
}

void provider_missions(const std::vector<eawr::vfs::MountSpec>& mounts) {
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto referenced = cp::campaign_referenced_types(data, campaign);
    const auto types = eawr::units::galactic_type_table(loaded.catalog, false, referenced);
    const auto options = campaign_options(data, types, campaign);
    const auto plan = take(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    const auto provider = take(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
    const auto human = *options.solo_reference_human;
    auto root_game = take(cp::CampaignRoot::create(plan, services));
    take(cp::configure_live_campaign_conflicts(root_game, files, loaded.catalog, data, types, provider));
    auto& game = *root_game.game();
    const auto snapshot = game.galaxy().snapshot();
    const auto& host = game.galaxy().setup().ability_host;
    if (!host) throw std::runtime_error("the stock campaign has no ability host");

    std::vector<Mission> missions;
    for (const auto& planet : host->corruption_planets) {
        const auto row = std::find_if(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == planet.planet; });
        if (row == snapshot->planets().end() || row->owner == human) continue;
        for (const auto& choice : planet.slots) if (choice.mode == 2 && !choice.map.empty()) missions.push_back({planet.planet, choice});
    }
    std::cout << "space corruption missions in " << campaign << ": " << missions.size() << '\n';
    expect(!missions.empty(), "the stock Underworld sandbox has space corruption missions (Piracy) on planets it does not own");
    const auto next = game.galaxy().next_entity_id();
    for (const auto& mission : missions) {
        gc::LiveCorruptionMission live; live.request = 1; live.planet = mission.planet; live.player = human;
        const auto built = take(provider->corruption_mission(*snapshot, live, mission.choice, next));
        expect(built.has_value(), mission.choice.map + ": the prepared mission map admits the mission battle");
        const auto owner = std::find_if(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == mission.planet; });
        // #7466 contract: the pre-debit readiness check and the service agree, so a charged mission is never refused by it.
        expect(provider->corruption_mission_ready(mission.choice, human, owner->owner) == built.has_value(),
            mission.choice.map + ": readiness answers exactly what the mission service builds (#7466)");
        if (!built) continue;
        const auto& record = built->conflict;
        expect(record.corruption_mission && record.layer == cp::BattleLayer::space && record.planet == mission.planet
            && record.invader == human && record.defender == owner->owner && record.forces.empty()
            && record.map_identity == mission.choice.map && record.story_plot == mission.choice.script
            && built->arbitration.route == cp::ConflictRoute::tactical && built->arbitration.forced_tactical,
            mission.choice.map + ": WGCH-126: the actor's owner invades, the planet's owner defends, no galactic force, forced tactical");
        auto resolved = provider->resolve(record, *snapshot, next + 1);
        expect(static_cast<bool>(resolved), mission.choice.map + ": the mission map resolves as a space battle");
        if (!resolved) { std::cerr << resolved.error().code << ": " << resolved.error().message << '\n'; continue; }
        std::size_t invaders{}, defenders{};
        for (const auto& unit : resolved.value().conflict.tactical.units) {
            if (unit.owner == record.invader) ++invaders;
            if (unit.owner == record.defender) ++defenders;
        }
        std::cout << mission.choice.map << " planet=" << mission.planet << " defender=" << record.defender
            << " invader units=" << invaders << " defender units=" << defenders << '\n';
        expect(invaders > 0, mission.choice.map + ": the mission map places the invader's units");
    }
    if (!missions.empty()) {
        gc::LiveCorruptionMission live; live.request = 1; live.planet = missions.front().planet; live.player = human;
        const auto owner = std::find_if(snapshot->planets().begin(), snapshot->planets().end(),
            [&](const auto& p) { return p.id == missions.front().planet; })->owner;
        auto land = missions.front().choice; land.mode = 1;
        expect(!take(provider->corruption_mission(*snapshot, live, land, next)).has_value(), "a land choice is not a space mission");
        expect(!provider->corruption_mission_ready(land, human, owner), "readiness refuses a land choice like the service (#7466)");
        auto unknown = missions.front().choice; unknown.map = "_Space_Planet_Not_Authored_Piracy.ted";
        expect(!take(provider->corruption_mission(*snapshot, live, unknown, next)).has_value(), "an unprepared mission map refuses the mission");
        expect(!provider->corruption_mission_ready(unknown, human, owner), "readiness refuses an unprepared map like the service (#7466)");
        expect(!provider->corruption_mission_ready(missions.front().choice, human, human),
            "readiness refuses a mission whose invader is its defender: the conflict queue would refuse it (#7466)");
    }
    root_game.stop_root();
}

// As the viewer's input_planets: the allegiance is the current player; route planning treats the map as discovered.
std::vector<view::InputPlanet> input_planets(const gc::GalacticSnapshot& snapshot) {
    std::vector<view::InputPlanet> result;
    for (const auto& planet : snapshot.planets()) {
        auto allegiance = planet.owner;
        if (const auto* control = snapshot.control()) for (const auto& row : control->state)
            if (row.planet == planet.id) allegiance = row.current_player;
        result.push_back({planet.id, {}, allegiance, true});
    }
    return result;
}

struct Route {
    std::vector<std::string> hashes;
    bool battle{};
};

// The player's route on one worker count. Returns every state hash of the run. With missing_map, the target's mission
// names a map that is not shipped: the route then checks the direct route instead of the battle.
Route player_route(const std::vector<eawr::vfs::MountSpec>& mounts, unsigned workers, bool missing_map = false) {
    const std::string label = std::string(missing_map ? "missing-map-route" : "mission-route") + " workers=" + std::to_string(workers);
    Route out;
    const auto base = take(eawr::vfs::Vfs::mount(mounts));
    const auto bytes = take(base.open("data/xml/" + std::string(kFile)));
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto campaign = xml.find("<Campaign Name=\"" + std::string(kRouteCampaign) + "\">");
    const auto active = campaign == std::string::npos ? campaign : xml.find("<Starting_Active_Player>", campaign);
    if (active == std::string::npos) throw std::runtime_error("missing stock campaign or active player");
    std::string faction;
    for (auto at = active + std::string_view("<Starting_Active_Player>").size(); xml[at] != '<'; ++at)
        if (xml[at] != ' ' && xml[at] != '\t' && xml[at] != '\r' && xml[at] != '\n') faction += xml[at];
    const auto forces = xml.find("<Starting_Forces> " + faction + ",", campaign);
    if (forces == std::string::npos) throw std::runtime_error("the active faction has no starting forces");
    const std::string home(kRouteHome);
    if (xml.find("<Starting_Forces> " + faction + ", " + home + ",", campaign) == std::string::npos)
        throw std::runtime_error("the active faction does not start at " + home);
    // The stock corruption hero (the Consortium saboteur) and Tyber Zann join the faction at the route's home planet.
    xml.insert(forces, "<Starting_Forces> " + faction + ", " + home + ", " + std::string(kCompany) + " </Starting_Forces>\n"
        "<Starting_Forces> " + faction + ", " + home + ", " + std::string(kRequiredCompany) + " </Starting_Forces>\n");
    const auto directory = std::filesystem::current_path() / ("mission-route-" + std::to_string(workers) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream file(directory / "xml" / std::string(kFile), std::ios::binary); file << xml;
        if (!file) throw std::runtime_error("sandbox overlay write failed");
    }
    if (missing_map) {
        const auto planet_bytes = take(base.open("data/xml/" + std::string(kPlanetFile)));
        auto planets_xml = std::string(reinterpret_cast<const char*>(planet_bytes.data()), planet_bytes.size());
        std::size_t renamed{};
        for (auto at = planets_xml.find(kRouteMap); at != std::string::npos; at = planets_xml.find(kRouteMap, at + kMissingMap.size())) {
            planets_xml.replace(at, kRouteMap.size(), kMissingMap); ++renamed;
        }
        if (!renamed) throw std::runtime_error("the stock planets name no " + std::string(kRouteMap));
        std::ofstream file(directory / "xml" / std::string(kPlanetFile), std::ios::binary); file << planets_xml;
        if (!file) throw std::runtime_error("sandbox planet overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"mission-route", directory, "data", {}});
    const auto files = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    const auto loaded = take(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(*files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const auto options = campaign_options(data, types, kRouteCampaign);
    gc::TypeId member{};
    for (const auto& row : options.types) if (row.original_type == kMember) member = row.id;
    const auto player = *options.solo_reference_human;
    const gc::SeatId seat{player};
    std::map<gc::PlayerId, bool> neutral;
    for (const auto& row : options.players) for (const auto& f : data.factions)
        if (f.name == row.faction) neutral[row.player.player_id] = f.is_neutral.value_or(false);

    // The product route: the prepared live campaign and its live session, driven tick by tick (#7381's test does the same).
    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, data, types,
        kRouteCampaign, options, "Normal_Default", false));
    if (!prepared->layout().ability_host) throw std::runtime_error("the stock campaign has no ability host");
    const auto host = *prepared->layout().ability_host;
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = workers; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));

    std::vector<std::string> notes;
    std::size_t rejections{}, settled{};
    std::uint64_t tick{}, cursor{};
    eawr::sim::EntityId answered_conflict{};
    // advance_unattended answers a battle prompt on the way as a player would (Resolve; Fight for a forced battle such as
    // the mission itself).
    const auto step = [&] {
        const auto advanced = eawr::platform::advance_unattended(*live, ++tick, std::chrono::seconds(60), answered_conflict);
        if (!advanced.reached) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label + ": the live campaign stalled at tick " + std::to_string(tick)
                + (advanced.stalled ? " (" + advanced.stalled->describe() + ")" : std::string()));
        }
        const auto events = live->galactic_events_after(cursor);
        cursor = events.through;
        for (const auto& row : events.events) {
            if (row.event.producer != seat) continue;
            if (row.event.kind == gc::GalacticEventKind::command_rejected) ++rejections;
            if (row.event.kind == gc::GalacticEventKind::ability_settled) ++settled;
            const auto kind = gc::to_string(row.event.kind);
            // Refusals and the ability lifecycle: a failure then shows which ability, if any, the hero prepared.
            if (row.event.kind == gc::GalacticEventKind::command_rejected || kind.find("ability") != std::string_view::npos)
                notes.push_back(std::string(kind) + " at tick " + std::to_string(row.event.tick) + ": "
                    + std::string(gc::to_string(row.event.reason)) + " subject=" + std::to_string(row.event.subject)
                    + " value=" + std::to_string(row.event.value));
        }
    };
    // Orders are stamped for the next tick, so every worker count runs the same command stream.
    const auto submit = [&](gc::CommandPayload payload) {
        live->submit(eawr::platform::LiveGameOrder{seat, std::move(payload), tick + 1});
    };
    const auto report = [&] {
        for (const auto& note : notes) std::cerr << label << "   " << note << '\n';
        notes.clear();
    };
    const auto finish = [&] {
        live->stop();
        out.hashes = live->tick_hashes();
        return out;
    };

    std::optional<eawr::testing::galactic::HeroPlace> hero;
    for (int wait = 0; !hero && wait < 60; ++wait) { step(); hero = find_hero_place(*live->frame().latest, player, member); }
    expect(hero.has_value(), label + ": the corruption hero is on the map");
    if (!hero) { report(); return finish(); }

    // A neighbouring hostile planet that shows the corruption tile and offers a space mission choice (one hop: #7426).
    auto snapshot = live->frame().latest;
    auto planets = input_planets(*snapshot);
    const auto space_choice = [&](gc::PlanetId id) {
        for (const auto& planet : host.corruption_planets) if (planet.planet == id)
            for (const auto& choice : planet.slots) if (choice.mode == 2 && !choice.map.empty()) return true;
        return false;
    };
    std::optional<view::AbilityDrop> drop;
    std::vector<gc::PlanetId> route;
    for (const auto& planet : planets) {
        if (planet.allegiance == player || neutral[planet.allegiance] || !space_choice(planet.id)) continue;
        const auto tile = view::ability_drop_candidate(*snapshot, player, hero->dragged, host, {}, planet.id);
        if (!tile || tile->kind != gc::AbilityKind::corruption) continue;
        const auto path = view::destination_path(*snapshot, player, hero->planet, planet.id, planets, true);
        if (path && path->size() == 2 && (route.empty() || planet.id < route.back())) { route = *path; drop = tile; }
    }
    expect(drop.has_value(), label + ": a neighbouring hostile planet with a space mission choice shows the corruption tile");
    if (!drop) { report(); return finish(); }
    std::cout << label << " target=" << drop->planet << " hops=" << route.size() - 1 << '\n';

    // The release on the tile: the move and the action are one order (WGCH-154), as the viewer sends it.
    view::MapInput input;
    view::DropIntent intent;
    if (hero->fleet) {
        const auto movement = view::fleet_destination_intent(*snapshot, player, hero->dragged, drop->planet, planets, {});
        expect(movement.has_value(), label + ": the target has a fleet route");
        if (!movement) return finish();
        intent = *movement;
    } else {
        intent.kind = view::DropKind::launch;
        intent.source = drop->supplied; intent.planet = hero->planet; intent.path = route;
    }
    intent.destination_actor = drop->supplied;
    const auto command = input.command(intent, planets);
    expect(command.has_value(), label + ": the drop maps to an order");
    if (!command) return finish();
    submit(*command);

    const gc::CorruptionMenu* menu{};
    std::shared_ptr<const gc::GalacticSnapshot> held;
    std::optional<unsigned> arrived;
    for (unsigned wait = 0; wait < 9000 && !menu && !live->frame().conflict; ++wait) {
        step();
        held = live->frame().latest;
        menu = held->corruption_menu();
        if (const auto place = find_hero_place(*held, player, member); place && !place->transit && place->planet == drop->planet) {
            if (!arrived) arrived = wait;
            else if (wait - *arrived > 60) break; // arrived; the planet's own tile is tried below
        }
    }
    // The hero standing on the target planet: a drop on that planet's own corruption tile opens the menu (the player's
    // second route, WGCH-121). Reported, so a run shows whether the arrival itself opened it.
    if (!menu && !live->frame().conflict) {
        const auto place = find_hero_place(*live->frame().latest, player, member);
        std::cout << label << " arrival opened no menu (hero at " << (place ? place->planet : 0) << ", transit="
            << (place && place->transit) << "); dropping on the planet's own tile" << std::endl;
        if (place && !place->transit && place->planet == drop->planet) {
            const auto now = live->frame().latest;
            const auto tile = view::ability_drop_candidate(*now, player, place->dragged, host, {}, drop->planet);
            if (tile && tile->kind == gc::AbilityKind::corruption) {
                view::DropIntent same;
                same.kind = view::DropKind::ability; same.source = tile->actor; same.planet = drop->planet;
                same.ability = gc::PrepareGalacticAbility{tile->actor, drop->planet, gc::AbilityKind::corruption};
                if (const auto order = input.command(same, input_planets(*now))) submit(*order);
                else notes.push_back("the same-planet drop maps to no order");
                for (unsigned wait = 0; wait < 60 && !menu; ++wait) { step(); held = live->frame().latest; menu = held->corruption_menu(); }
            } else notes.push_back("the target planet shows no corruption tile for the hero");
        }
    }
    expect(menu && menu->player == player && menu->planet == drop->planet, label + ": the release publishes the Corrupt Planet menu");
    if (!menu) {
        const auto frame = live->frame();
        const auto place = find_hero_place(*frame.latest, player, member);
        std::cerr << label << " no menu at tick " << tick << ": hero=" << place.has_value()
            << " hero_planet=" << (place ? place->planet : 0) << " transit=" << (place && place->transit)
            << " conflict=" << static_cast<bool>(frame.conflict) << " pending=" << static_cast<bool>(frame.pending_conflict)
            << " hero_action_menu=" << (frame.latest->hero_action_menus() != nullptr) << '\n';
        report(); return finish();
    }
    const gc::ObservedCorruptionChoice* piracy{};
    for (const auto& row : menu->choices) {
        std::cout << label << " slot=" << row.slot << " type=" << static_cast<unsigned>(row.prepared.choice.type)
            << " mode=" << (row.live_choice ? static_cast<int>(row.live_choice->mode) : -1)
            << " affordable=" << row.affordable << " units=" << row.units_satisfied << " missions=" << row.missions_satisfied
            << " planets=" << row.planets_satisfied << " mission_available=" << row.mission_available
            << " prepared=" << row.live_prepared.has_value() << '\n';
        if (!piracy && row.live_choice && row.live_choice->mode == 2) piracy = &row;
    }
    if (missing_map) {
        expect(piracy && piracy->mission_available && piracy->live_prepared,
            label + ": a space mission whose map was left out is still offered (WGCH-199: it takes the direct route)");
        if (!piracy || !piracy->live_prepared) { report(); return finish(); }
        const auto credits = [&] {
            const auto snapshot_now = live->frame().latest;
            const auto& economy = snapshot_now->player_economy();
            if (economy) for (const auto& row : economy->players) if (row.player == player) return row.credits;
            throw std::runtime_error("the player has no economy row");
        };
        const auto corruption = [&] {
            const auto* state = live->frame().latest->economy_corruption();
            if (state) for (const auto& row : state->planets) if (row.planet == drop->planet) return row.corruption;
            return gc::corruption_fixture::Type{};
        };
        const auto before = credits();
        const auto refused_before = rejections;
        const auto settled_before = settled;
        const auto price = piracy->live_prepared->price;
        const auto type = piracy->live_choice->type;
        submit(gc::ExecutePreparedAbility{*piracy->live_prepared});
        // The order's outcome arrives a few ticks later: wait (bounded) for it to settle or be refused, then read the credits.
        for (unsigned wait = 0; wait < 60 && settled == settled_before && rejections == refused_before; ++wait) step();
        const auto after = credits();
        const auto charged = before.raw() - after.raw();
        // The 5 s transition, then the planet carries the choice's corruption. No mission battle on the way.
        bool applied{};
        for (unsigned wait = 0; wait < 600 && !applied && !live->frame().conflict && !live->frame().pending_conflict; ++wait) {
            step(); applied = corruption() == type;
        }
        const auto frame = live->frame();
        std::cout << label << " charged " << charged << " (price " << price.raw() << ") refused=" << (rejections - refused_before)
            << " applied=" << applied << " conflict=" << static_cast<bool>(frame.conflict) << " tick=" << tick << '\n';
        expect(settled > settled_before && rejections == refused_before && charged == price.raw(),
            label + ": executing it takes the direct route: charged the price once, nothing refused");
        expect(applied && !frame.conflict && !frame.pending_conflict,
            label + ": the corruption applies after the timed transition, with no mission battle");
        report();
        return finish();
    }
    expect(piracy && piracy->mission_available && piracy->live_prepared,
        label + ": the space mission choice is offered available (WGCH-197: the live mission service is installed)");
    if (!piracy || !piracy->live_prepared) { report(); return finish(); }
    const auto map = piracy->live_choice->map;
    const bool executable = piracy->affordable && piracy->units_satisfied && piracy->missions_satisfied && piracy->planets_satisfied;
    expect(executable, label + ": the stock space mission choice's requirements are met at the target");
    if (!executable) { report(); return finish(); }

    // The menu's order for that row (corruption_hud submits exactly this).
    const auto prepared_choice = *piracy->live_prepared;
    submit(gc::ExecutePreparedAbility{prepared_choice});
    for (unsigned wait = 0; wait < 900 && !(live->frame().conflict && live->frame().battle); ++wait) step();
    const auto frame = live->frame();
    out.battle = frame.conflict && frame.battle;
    if (!out.battle) {
        expect(false, label + ": executing the choice starts the forced mission battle");
        std::cerr << label << " no mission child at tick " << tick << ": conflict=" << static_cast<bool>(frame.conflict)
            << " battle=" << static_cast<bool>(frame.battle) << " pending=" << static_cast<bool>(frame.pending_conflict) << '\n';
        report(); return finish();
    }
    // The live entry carries the canonical map path (Data/Art/Maps/<name>); the choice names the bare file.
    const auto upper = [](std::string text) {
        for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    };
    const auto identity = upper(frame.conflict->map_identity);
    const auto suffix = "/" + upper(map);
    const bool on_map = identity.size() >= suffix.size() && identity.compare(identity.size() - suffix.size(), suffix.size(), suffix) == 0;
    std::cout << label << " mission child identity=" << frame.conflict->map_identity << " mission=" << frame.conflict->corruption_mission
        << " invader=" << frame.conflict->invader << '\n';
    expect(frame.conflict->corruption_mission && on_map && frame.conflict->invader == player,
        label + ": executing the choice starts the forced mission battle on its map");
    std::size_t invaders{};
    for (const auto& unit : frame.conflict->tactical.units) if (unit.owner == player) ++invaders;
    std::cout << label << " mission child map=" << map << " invader units=" << invaders << " tick=" << tick << '\n';
    expect(invaders > 0, label + ": the mission battle gives the player the map's units");
    // The viewer's part of a battle start: bind the player's command feed, run it and report the scene ready. The paced
    // child then steps with the galaxy frozen behind it.
    auto feed = live->battle_feed(frame.conflict->id, seat);
    expect(static_cast<bool>(feed), label + ": the player's seat binds the mission battle's command feed");
    if (!feed) { report(); return finish(); }
    // Readiness counts only once the platform's content preparation for the battle has succeeded (the viewer's loading
    // screen waits for the same outcome).
    std::shared_ptr<const eawr::platform::CampaignLoadingView> loading;
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(5);
         (loading = live->loading(seat)) && loading->outcome == eawr::platform::CampaignLoadingOutcome::pending
         && std::chrono::steady_clock::now() < until;)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool prepared_scene = !loading || loading->outcome == eawr::platform::CampaignLoadingOutcome::succeeded;
    expect(prepared_scene, label + ": the mission battle's content preparation succeeds");
    if (!prepared_scene) {
        std::cerr << label << " battle preparation " << (loading->outcome == eawr::platform::CampaignLoadingOutcome::pending
            ? std::string("still pending") : (loading->failure ? eawr::core::format_diagnostic(*loading->failure) : std::string("failed")))
            << '\n';
        feed->stop(); report(); return finish();
    }
    live->battle_ready(frame.conflict->id);
    feed->set_paused(false);
    for (unsigned more = 0; more < 90; ++more) step();
    const auto stepped = live->frame();
    expect(stepped.battle ? stepped.battle->completed_tick() > 0 : stepped.result.has_value(), label + ": the mission battle steps");
    std::cout << label << " mission child stepped to " << (stepped.battle ? stepped.battle->completed_tick() : 0) << '\n';
    feed->stop(); feed.reset();
    report();
    return finish();
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock space corruption mission test requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        provider_missions(mounts);
        static_cast<void>(player_route(mounts, 1, true)); // the direct route, not a battle: one worker count
        std::optional<Route> reference;
        for (const unsigned workers : {1U, 2U, 4U, 8U}) {
            auto route = player_route(mounts, workers);
            if (!route.battle) break; // the first failing step is already reported; other counts would repeat it
            if (!reference) reference = std::move(route);
            else expect(route.hashes == reference->hashes,
                "the player's mission route steps equally on " + std::to_string(workers) + " workers");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

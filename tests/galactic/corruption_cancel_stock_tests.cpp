#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "../presentation/galactic/stock_hero_place.hpp"
#include "galactic_support.hpp"
#include "galactic/map_input.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/vfs/vfs.hpp"

// The owner's #7357 report on stock data: the Consortium's corruption hero is sent to a hostile planet by a release on its
// corruption tile (the viewer's own preview and command path, against a live session), the Corrupt Planet dialog opens,
// and Close cancels it. Retail: opening and closing the dialog does not consume the hero (WGCH-121), so afterwards the
// hero must still be on the map, take an ordinary move order and offer its corruption tile again. Every step prints what
// it saw: a failure names the first step that stopped.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;
using namespace std::chrono_literals;

constexpr std::string_view kStockFile = "campaigns_underworld_gc.xml";
constexpr std::string_view kCompany = "Underworld_Saboteur_Team";
constexpr std::string_view kMember = "Underworld_Saboteur";

// As the viewer's input_planets: the allegiance is the current player and discovery is the local observer's.
std::vector<view::InputPlanet> input_planets(const gc::GalacticSnapshot& snapshot, gc::PlayerId player) {
    std::vector<view::InputPlanet> result;
    for (const auto& planet : snapshot.planets()) {
        auto allegiance = planet.owner;
        if (const auto* control = snapshot.control()) for (const auto& row : control->state)
            if (row.planet == planet.id) allegiance = row.current_player;
        result.push_back({planet.id, {}, allegiance, view::observer_discovery(snapshot, player, planet.id).value_or(false)});
    }
    return result;
}

std::string reject_text(const std::vector<gc::GalacticEvent>& rejected) {
    std::string text;
    for (const auto& event : rejected)
        text += " [tick " + std::to_string(event.tick) + " " + std::string(gc::to_string(event.reason))
            + " subject=" + std::to_string(event.subject) + (event.value ? " value=" + std::to_string(event.value) : std::string()) + "]";
    return text.empty() ? " none" : text;
}

// `prefer` names the planet to drop on when its tile shows (the owner's #7357 report: a hostile Corellia); otherwise the
// nearest hostile planet with the tile.
void run(const std::vector<eawr::vfs::MountSpec>& mounts, eawr::data::Profile profile, std::string_view kFile, std::string_view kCampaign,
    std::string_view source, std::string_view prefer) {
    const std::string label = "corruption-cancel " + std::string(source) + " " + std::string(kCampaign);
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto bytes = base.open("data/xml/" + std::string(kFile)).value();
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto campaign = xml.find("<Campaign Name=\"" + std::string(kCampaign) + "\">");
    const auto active = campaign == std::string::npos ? campaign : xml.find("<Starting_Active_Player>", campaign);
    if (active == std::string::npos) throw std::runtime_error("missing stock campaign or active player");
    std::string faction;
    for (auto at = active + std::string_view("<Starting_Active_Player>").size(); xml[at] != '<'; ++at)
        if (xml[at] != ' ' && xml[at] != '\t' && xml[at] != '\r' && xml[at] != '\n') faction += xml[at];
    const auto forces = xml.find("<Starting_Forces> " + faction + ",", campaign);
    if (forces == std::string::npos) throw std::runtime_error("the active faction has no starting forces");
    std::string home;
    for (auto at = forces + std::string("<Starting_Forces> ").size() + faction.size() + 1; xml[at] != ','; ++at)
        if (xml[at] != ' ') home += xml[at];
    xml.insert(forces, "<Starting_Forces> " + faction + ", " + home + ", " + std::string(kCompany) + " </Starting_Forces>\n");
    const auto directory = std::filesystem::current_path() / ("corruption-cancel-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream out(directory / "xml" / std::string(kFile), std::ios::binary); out << xml;
        if (!out) throw std::runtime_error("sandbox overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"corruption-cancel", directory, "data", {}});
    const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    const auto loaded = take(eawr::data::load_catalog(*filesystem, profile));
    const auto data = take(eawr::data::galactic::load_galactic_data(*filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);

    cp::CampaignSetupOptions options; options.seed = 42;
    std::map<std::string, gc::PlayerId> ids;
    gc::SeatId seat{};
    for (const auto& row : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        ids[row.name] = id;
        options.players.push_back({{id, id, 0}, row.name, true});
        const bool human = row.name == faction;
        if (human) { seat = gc::SeatId{id}; options.solo_reference_human = id; }
        if (!row.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    gc::TypeId member{};
    for (const auto& original : originals) {
        const gc::TypeId id = options.types.size() + 1;
        options.types.push_back({id, original});
        if (original == kMember) member = id;
    }
    expect(member != 0, "the corruption hero is a registered type");
    const auto player = ids.at(faction);
    std::map<gc::PlayerId, bool> neutral;
    for (const auto& row : data.factions) neutral[ids.at(row.name)] = row.is_neutral.value_or(false);

    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, loaded.catalog, data, types,
        kCampaign, options, "Normal_Default", false));
    if (!prepared->layout().ability_host) throw std::runtime_error("stock campaign has no ability host");
    const auto host = *prepared->layout().ability_host;
    gc::PlanetId preferred{};
    for (const auto& location : prepared->plan().locations())
        if (location.planet && location.original_type == prefer) preferred = location.planet->id;
    if (!prefer.empty()) expect(preferred != 0, "the campaign lists the owner's planet");
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));

    std::uint64_t tick{}, cursor{};
    std::vector<gc::GalacticEvent> rejected;
    std::size_t cancelled_events{};
    std::vector<gc::GalacticEvent> seat_events; // the local seat's own events, printed when a step goes wrong
    const auto step = [&] {
        live->advance_to(++tick);
        if (!live->wait_for(tick, 60s)) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label + ": the live campaign stalled at tick " + std::to_string(tick));
        }
        const auto events = live->galactic_events_after(cursor);
        cursor = events.through;
        for (const auto& row : events.events) {
            if (row.event.producer != seat) continue;
            seat_events.push_back(row.event);
            if (row.event.kind == gc::GalacticEventKind::command_rejected) rejected.push_back(row.event);
            if (row.event.kind == gc::GalacticEventKind::ability_cancelled) ++cancelled_events;
        }
    };
    std::optional<eawr::testing::galactic::HeroPlace> hero;
    while (!hero && tick < 60) { step(); hero = find_hero_place(*live->frame().latest, player, member); }
    expect(hero.has_value(), "the corruption hero is on the map");
    if (!hero) return;
    std::cout << label << " hero tick=" << tick << " bearer=" << hero->bearer << " dragged=" << hero->dragged
        << " planet=" << hero->planet << " fleet=" << hero->fleet << '\n';
    const auto start = hero->planet;
    // Who owns the hero and its fleet at each step (the coordinator's #7357 Remake lead: a Neutral-affiliated hero).
    const auto owners = [&](const char* when) {
        const auto now = live->frame().latest;
        const auto place = find_hero_place(*now, player, member);
        const auto actors = now->actor_records();
        const auto bearer = place ? std::find_if(actors.begin(), actors.end(), [&](const auto& row) { return row.id == place->bearer; }) : actors.end();
        const auto fleets = now->fleets();
        const auto fleet = place ? std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == place->dragged; }) : fleets.end();
        std::cout << label << " owners " << when << ": local=" << player << " hero=" << (bearer != actors.end() ? static_cast<long long>(bearer->owner) : -1)
            << " fleet=" << (fleet != fleets.end() ? static_cast<long long>(fleet->owner) : -1)
            << " place=" << (place ? static_cast<long long>(place->planet) : -1) << " transit=" << (place && place->transit) << '\n';
    };
    owners("before the drop");

    // The owner's planet when its corruption tile shows, else the nearest hostile planet whose tile the drag shows, as a player
    // picks it from the map.
    auto snapshot = live->frame().latest;
    auto planets = input_planets(*snapshot, player);
    for (auto& planet : planets) planet.discovered = true; // route planning only, as the owner's discovered map
    std::optional<view::AbilityDrop> drop;
    std::vector<gc::PlanetId> route;
    for (const auto& planet : planets) {
        if (planet.allegiance == player || neutral[planet.allegiance]) continue;
        const auto tile = view::ability_drop_candidate(*snapshot, player, hero->dragged, host, {}, planet.id);
        if (!tile || tile->kind != gc::AbilityKind::corruption) continue;
        const auto path = view::destination_path(*snapshot, player, hero->planet, planet.id, planets, true);
        if (path && (route.empty() || planet.id == preferred || (drop->planet != preferred && path->size() < route.size()))) {
            route = *path; drop = tile;
        }
    }
    if (preferred) expect(drop.has_value() && drop->planet == preferred, "the drop is on the owner's planet, whose corruption tile shows");
    if (!drop) {
        // #7357 on Remake: why no hostile planet shows the tile, gate by gate, with the viewer's own preview checks.
        const auto actors = snapshot->actor_records();
        const auto bearer = std::find_if(actors.begin(), actors.end(), [&](const auto& row) { return row.id == hero->bearer; });
        const auto profile = bearer == actors.end() ? host.profiles.end()
            : std::find_if(host.profiles.begin(), host.profiles.end(), [&](const auto& row) { return row.type == bearer->current_type; });
        const auto services = snapshot->ability_services();
        const auto service = std::find_if(services.begin(), services.end(), [&](const auto& row) { return row.actor == hero->bearer; });
        const bool have = profile != host.profiles.end() && service != services.end();
        std::string routing;
        for (const auto& name : host.activated_corruption_abilities) routing += " " + name;
        std::cout << label << " no tile: bearer=" << (bearer != actors.end()) << " profile=" << (profile != host.profiles.end())
            << " service=" << (service != services.end())
            << " corruption=" << (profile != host.profiles.end() && profile->corruption ? profile->corruption->name : std::string("-"))
            << " ground_listed=" << (profile != host.profiles.end() && std::find(profile->ground_activated.begin(), profile->ground_activated.end(),
                gc::AbilityKind::corruption) != profile->ground_activated.end())
            << " service_enabled=" << (service != services.end() && service->corruption_enabled)
            << " service_cancelled=" << (service != services.end() && service->corruption_cancelled)
            << " preview_enabled=" << (have && view::ability_preview::enabled(*profile, *service, host, gc::AbilityKind::corruption))
            << " ground_enabled=" << (have && gc::ground_ability_enabled(*profile, *service, gc::AbilityKind::corruption))
            << " content_planets=" << host.corruption_planets.size()
            << " routing:" << routing << '\n';
        const auto* economy = snapshot->economy_corruption();
        int shown = 0;
        for (const auto& planet : planets) {
            if (planet.allegiance == player || neutral[planet.allegiance] || shown >= 8) continue;
            const auto path = view::destination_path(*snapshot, player, hero->planet, planet.id, planets, true);
            const auto content = std::find_if(host.corruption_planets.begin(), host.corruption_planets.end(),
                [&](const auto& row) { return row.planet == planet.id; });
            const gc::CorruptionEconomyPlanet* economic = nullptr;
            if (economy) for (const auto& row : economy->planets) if (row.planet == planet.id) economic = &row;
            const auto tile = view::ability_drop_candidate(*snapshot, player, hero->dragged, host, {}, planet.id);
            std::cout << label << "   planet=" << planet.id << " owner=" << planet.allegiance << " route=" << (path ? static_cast<long long>(path->size()) : -1)
                << " tile=" << (tile ? static_cast<int>(tile->kind) : -1)
                << " admitted=" << (bearer != actors.end() && view::ability_preview::target_admitted(*snapshot, *bearer, planet.id, host, gc::AbilityKind::corruption))
                << " content=" << (content != host.corruption_planets.end())
                << " retains=" << (content != host.corruption_planets.end() && content->retains_influence)
                << " slots=" << (content != host.corruption_planets.end() ? static_cast<int>(content->slots[0].type) : -1) << ","
                << (content != host.corruption_planets.end() ? static_cast<int>(content->slots[1].type) : -1) << ","
                << (content != host.corruption_planets.end() ? static_cast<int>(content->slots[2].type) : -1)
                << " economic=" << (economic ? static_cast<int>(economic->corruption) : -1) << '\n';
            ++shown;
        }
    }
    expect(drop.has_value(), "a hostile planet shows the corruption tile and has a route");
    if (!drop) return;
    std::cout << label << " target=" << drop->planet << " hops=" << route.size() - 1 << " supplied=" << drop->supplied << '\n';

    // The release on the distant tile: the move and the action are one order (WGCH-154).
    view::MapInput input;
    view::DropIntent intent;
    if (hero->fleet) {
        const auto movement = view::fleet_destination_intent(*snapshot, player, hero->dragged, drop->planet, planets, {});
        expect(movement.has_value(), "the distant tile has a fleet route");
        if (!movement) return;
        intent = *movement;
    } else {
        intent.kind = view::DropKind::launch;
        intent.source = drop->supplied; intent.planet = hero->planet; intent.path = route;
    }
    intent.destination_actor = drop->supplied;
    const auto command = input.command(intent, planets);
    expect(command.has_value(), "the drop maps to an order");
    if (!command) return;
    live->submit(eawr::platform::LiveGameOrder{seat, *command, {}});

    const gc::CorruptionMenu* menu{};
    std::shared_ptr<const gc::GalacticSnapshot> held;
    for (const auto limit = tick + 9000; tick < limit && !menu; ) {
        step();
        held = live->frame().latest;
        menu = held->corruption_menu();
        const auto frame = live->frame();
        if (frame.pending_conflict || frame.conflict) break;
    }
    std::cout << label << " menu tick=" << tick << " published=" << (menu != nullptr)
        << " conflict=" << static_cast<bool>(live->frame().pending_conflict || live->frame().conflict)
        << " rejected:" << reject_text(rejected) << '\n';
    if (!menu) {
        // Where the Defiler is, what its fleet is doing, and what the seat's own orders produced since the drop.
        const auto now = live->frame().latest;
        const auto place = find_hero_place(*now, player, member);
        std::cout << label << " no menu: place=" << (place ? static_cast<long long>(place->planet) : -1)
            << " transit=" << (place && place->transit) << " fleet=" << (place && place->fleet)
            << " dragged=" << (place ? static_cast<long long>(place->dragged) : -1) << '\n';
        if (place && place->fleet) for (const auto& fleet : now->fleets()) if (fleet.id == place->dragged)
            std::cout << label << "   fleet orbit=" << fleet.orbit << " members=" << fleet.members.size()
                << " transit=" << fleet.motion.transit.has_value() << '\n';
        for (const auto& result : now->ability_results()) if (result.actor == drop->supplied)
            std::cout << label << "   result kind=" << static_cast<int>(result.kind) << " outcome=" << static_cast<int>(result.outcome)
                << " planet=" << result.planet << " preparation=" << result.preparation << '\n';
        for (const auto& event : seat_events)
            std::cout << label << "   event tick=" << event.tick << " " << gc::to_string(event.kind) << " subject=" << event.subject
                << " value=" << event.value << " reason=" << gc::to_string(event.reason) << '\n';
    }
    expect(menu && menu->player == player && menu->planet == drop->planet && !menu->choices.empty(),
        "the release publishes the corruption dialog for the local player");
    if (!menu) return;
    for (const auto& row : menu->choices)
        std::cout << label << " slot=" << row.slot << " type=" << static_cast<unsigned>(row.prepared.choice.type)
            << " mode=" << (row.live_choice ? static_cast<int>(row.live_choice->mode) : -1)
            << " affordable=" << row.affordable << " units=" << row.units_satisfied << " missions=" << row.missions_satisfied
            << " planets=" << row.planets_satisfied << " mission_available=" << row.mission_available
            << " prepared=" << row.live_prepared.has_value() << '\n';
    owners("with the dialog open");
    const auto actor = menu->actor;
    const auto preparation = menu->preparation;
    expect(preparation != 0, "the published dialog names its preparation, so Close can cancel it");

    // Close: the viewer submits the cancel for the published actor and preparation.
    rejected.clear();
    live->submit(eawr::platform::LiveGameOrder{seat, gc::CancelGalacticAbility{actor, preparation}, {}});
    for (int wait = 0; wait < 8; ++wait) step();
    snapshot = live->frame().latest;
    const auto after = find_hero_place(*snapshot, player, member);
    const auto actors = snapshot->actor_records();
    const auto record = std::find_if(actors.begin(), actors.end(), [&](const auto& row) { return row.id == actor; });
    std::cout << label << " cancel tick=" << tick << " cancelled_events=" << cancelled_events
        << " menu=" << (snapshot->corruption_menu() != nullptr) << " place=" << after.has_value()
        << " actor_alive=" << (record != actors.end() && !record->dead && !record->delete_pending)
        << " actor_planet=" << (record != actors.end() ? record->planet : 0)
        << " actor_container=" << (record != actors.end() ? record->container : 0)
        << " rejected:" << reject_text(rejected) << '\n';
    owners("after Cancel");
    if (after) std::cout << label << " after dragged=" << after->dragged << " planet=" << after->planet
        << " fleet=" << after->fleet << " transit=" << after->transit << '\n';
    expect(rejected.empty() && cancelled_events == 1, "Close cancels the open dialog");
    expect(snapshot->corruption_menu() == nullptr, "the dialog is gone after Close");
    expect(record != actors.end() && !record->dead && !record->delete_pending, "Close does not consume the hero (WGCH-121)");
    expect(after.has_value() && !after->transit && after->planet == drop->planet, "the hero stays at the target planet");
    if (!after) return;

    // The hero is free again: its corruption tile shows at the planet and it takes an ordinary move order home.
    planets = input_planets(*snapshot, player);
    for (auto& planet : planets) planet.discovered = true;
    const auto again = view::ability_drop_candidate(*snapshot, player, after->dragged, host, {}, after->planet);
    std::cout << label << " again tile=" << (again ? static_cast<int>(again->kind) : -1) << '\n';
    expect(again && again->kind == gc::AbilityKind::corruption, "the corruption tile shows again after Close");
    if (!again) return;

    // Dispatch again, now on the tile of the planet the hero stands at (no travel): the dialog opens again, and a second
    // Close frees the hero again.
    {
        view::DropIntent here;
        here.kind = view::DropKind::ability;
        here.source = again->supplied; here.planet = again->planet;
        here.ability = view::ability_drop_order(*again);
        const auto order = input.command(here, planets);
        expect(order.has_value(), "the drop on the planet the hero stands at maps to an order");
        if (!order) return;
        rejected.clear();
        live->submit(eawr::platform::LiveGameOrder{seat, *order, {}});
        const gc::CorruptionMenu* reopened{};
        for (int wait = 0; wait < 30 && !reopened; ++wait) { step(); held = live->frame().latest; reopened = held->corruption_menu(); }
        std::cout << label << " reopened=" << (reopened != nullptr) << " rejected:" << reject_text(rejected) << '\n';
        expect(reopened && reopened->actor == actor && reopened->preparation != preparation,
            "a second dispatch after Close opens a new dialog for the same hero");
        if (!reopened) return;
        rejected.clear();
        live->submit(eawr::platform::LiveGameOrder{seat, gc::CancelGalacticAbility{reopened->actor, reopened->preparation}, {}});
        for (int wait = 0; wait < 8; ++wait) step();
        snapshot = live->frame().latest;
        expect(rejected.empty() && cancelled_events == 2 && snapshot->corruption_menu() == nullptr, "the second Close cancels it too");
        planets = input_planets(*snapshot, player);
        for (auto& planet : planets) planet.discovered = true;
    }
    // The owner's acceptance (#7357): after Cancel the hero is sent on to another planet B (not the one it stands at, not its
    // start; a hostile one first) and the order is accepted: it travels.
    gc::PlanetId other{};
    std::size_t best{}; bool best_hostile{};
    for (const auto& planet : planets) {
        if (planet.id == after->planet || planet.id == start) continue;
        const auto path = view::destination_path(*snapshot, player, after->planet, planet.id, planets, true);
        if (!path) continue;
        const bool hostile = planet.allegiance && planet.allegiance != player && !neutral[planet.allegiance];
        if (!other || (hostile && !best_hostile) || (hostile == best_hostile && path->size() < best)) {
            other = planet.id; best = path->size(); best_hostile = hostile;
        }
    }
    expect(other != 0, "another planet B is reachable from planet A");
    if (!other) return;
    std::optional<gc::CommandPayload> onward;
    if (after->fleet) {
        if (const auto movement = view::fleet_destination_intent(*snapshot, player, after->dragged, other, planets, {}))
            onward = input.command(*movement, planets);
    } else if (const auto path = view::destination_path(*snapshot, player, after->planet, other, planets, true)) {
        onward = gc::LaunchFleet{{after->dragged}, *path, std::nullopt, 0};
    }
    std::cout << label << " planet B=" << other << " hostile=" << best_hostile << " order=" << onward.has_value() << '\n';
    expect(onward.has_value(), "the hero has a move order to planet B");
    if (!onward) return;
    rejected.clear();
    live->submit(eawr::platform::LiveGameOrder{seat, *onward, {}});
    bool moving = false;
    for (int wait = 0; wait < 30 && !moving; ++wait) {
        step();
        const auto now = find_hero_place(*live->frame().latest, player, member);
        moving = now && (now->transit || now->planet == other);
    }
    std::cout << label << " move to B moving=" << moving << " rejected:" << reject_text(rejected) << '\n';
    expect(moving && rejected.empty(), "the hero takes an ordinary move order to another planet after Cancel");
    live->stop();
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock corruption cancel test requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        // The owner's report: Galactic Conquest as the Zann Consortium, the Defiler dropped on a hostile Corellia. In Equal
        // Footing the Empire's Corellia (starbase and ships in orbit) borders Zann's Bothawui, so it is discovered, as the
        // owner's was. In Origin it is two hops out and undiscovered (#7426, its own test in #7431), so Origin drops on the
        // nearest hostile planet instead.
        run(mounts, eawr::data::Profile::foc, kStockFile, "Sandbox_Equal_Footing_Underworld", "stock", "Corellia");
        run(mounts, eawr::data::Profile::foc, kStockFile, "Sandbox_Origin_Underworld", "stock", "");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

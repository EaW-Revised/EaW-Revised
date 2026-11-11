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

// The owner's steal report on stock data: two authored Rebel sandboxes that start at tech 2 and 3, their story-spawned
// droid team sent to an Underworld planet by ordinary orders, and a release on the STEAL tile through the viewer's own
// preview and command path against a live session. Nothing is hand-listed the way a synthetic fixture lists it, which
// is why those fixtures could not see this. Every step prints what it saw: a failure names the first step that stopped.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;
using namespace std::chrono_literals;

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
            + (event.value ? " value=" + std::to_string(event.value) : std::string()) + "]";
    return text.empty() ? " none" : text;
}

// The owner's order of play with `in_orbit`: travel first, then release on the tile of the planet the team orbits.
// Without it the release is on the distant planet's tile and the move and the menu are one order.
// A bearer other than the droid stops at the assassinate's tile check below.
void run(const std::shared_ptr<const eawr::vfs::Vfs>& filesystem, const eawr::data::Catalog& catalog,
    const eawr::data::galactic::GalacticData& data, const eawr::units::GalacticTypeTable& types,
    std::string_view campaign_name, std::int64_t authored_tech, bool in_orbit, std::string_view bearer_name = "Droid_R2D2") {
    const bool hunter = bearer_name != "Droid_R2D2";
    const auto label = (hunter ? "hunter-tile " : "steal-drop ") + std::string(campaign_name) + (in_orbit ? " in-orbit" : " remote");
    const auto source = std::find_if(data.campaigns.begin(), data.campaigns.end(),
        [&](const auto& row) { return row.name == campaign_name; });
    if (source == data.campaigns.end()) throw std::runtime_error("missing stock campaign");
    cp::CampaignSetupOptions options; options.seed = 42;
    std::map<std::string, gc::PlayerId> ids;
    gc::SeatId seat{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        ids[faction.name] = id;
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) { seat = gc::SeatId{id}; options.solo_reference_human = id; }
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    gc::TypeId bearer_type{};
    for (const auto& original : originals) {
        const gc::TypeId id = options.types.size() + 1;
        options.types.push_back({id, original});
        if (original == bearer_name) bearer_type = id;
    }
    expect(bearer_type != 0, "the stock droid bearer is a registered type");
    const auto rebel = ids.at("Rebel"), underworld = ids.at("Underworld");

    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, catalog, data, types,
        campaign_name, options, "Normal_Default", false));
    if (!prepared->layout().ability_host) throw std::runtime_error("stock campaign has no ability host");
    const auto host = *prepared->layout().ability_host; // the live session keeps the setup; the viewer holds this copy
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));

    std::uint64_t tick{}, cursor{};
    std::vector<gc::GalacticEvent> rejected;
    const auto step = [&] {
        live->advance_to(++tick);
        if (!live->wait_for(tick, 60s)) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label + ": the live campaign stalled at tick " + std::to_string(tick));
        }
        const auto events = live->galactic_events_after(cursor);
        cursor = events.through;
        for (const auto& row : events.events)
            if (row.event.kind == gc::GalacticEventKind::command_rejected && row.event.producer == seat) rejected.push_back(row.event);
    };

    // The solo sandboxes spawn the named heroes from the Rebel story at its start.
    std::optional<eawr::testing::galactic::HeroPlace> team;
    while (!team && tick < 900) { step(); team = find_hero_place(*live->frame().latest, rebel, bearer_type); }
    expect(team.has_value(), "the bearer is on the map");
    if (!team) return;
    std::cout << label << " team tick=" << tick << " bearer=" << team->bearer << " dragged=" << team->dragged
        << " planet=" << team->planet << " fleet=" << team->fleet << '\n';
    const auto started = live->frame().latest;
    if (const auto& economy = started->player_economy(); economy) for (const auto& row : economy->players)
        if (row.player == rebel) {
            std::cout << label << " current_tech=" << row.current_tech << '\n';
            expect(row.current_tech == authored_tech, "the sandbox starts the Rebels at its authored tech level");
        }

    // The nearest Underworld planet the player can route to, as a player would pick it from the map. Planets start
    // undiscovered (WGUI-99), so this waits for the observer as the map does.
    auto snapshot = live->frame().latest;
    auto planets = input_planets(*snapshot, rebel);
    if (hunter) {
        // Neutralization needs only a planet with no hackable weapon, so the hunter's own planet shows the tile.
        // Neutralization is ground-activated since #6120 (WGCH-187), so the release reaches the host; the distant tile and the release are printed
        // rather than pinned: the hunter's own planet holds no enemy hero, so a refusal is expected.
        const auto here = view::ability_drop_candidate(*snapshot, rebel, team->dragged, host, {}, team->planet);
        std::optional<view::AbilityDrop> distant;
        for (const auto& planet : planets)
            if (!distant && planet.allegiance == underworld && planet.id != team->planet)
                distant = view::ability_drop_candidate(*snapshot, rebel, team->dragged, host, {}, planet.id);
        std::cout << label << " tile=" << (here ? static_cast<int>(here->kind) : -1)
            << " distant=" << (distant ? static_cast<int>(distant->kind) : -1) << '\n';
        expect(here && here->kind == gc::AbilityKind::hero_neutralization && here->planet == team->planet,
            "the stock bounty hunter's assassinate gets a tile at the planet it stands on");
        if (!here) return;
        view::MapInput input;
        view::DropIntent intent;
        intent.kind = view::DropKind::ability;
        intent.source = here->supplied; intent.planet = here->planet;
        intent.ability = view::ability_drop_order(*here);
        const auto command = input.command(intent, planets);
        rejected.clear();
        if (command) live->submit(eawr::platform::LiveGameOrder{seat, *command, {}});
        for (int wait = 0; wait < 8; ++wait) step();
        const auto after = live->frame().latest;
        std::cout << label << " release command=" << command.has_value() << " menu=" << (after->hero_action_menus() != nullptr)
            << " rejected:" << reject_text(rejected) << '\n';
        live->stop();
        return;
    }
    std::vector<gc::PlanetId> route;
    for (const auto limit = tick + 900; route.empty() && tick <= limit; step()) {
        snapshot = live->frame().latest;
        planets = input_planets(*snapshot, rebel);
        const auto from = find_hero_place(*snapshot, rebel, bearer_type);
        if (!from) continue;
        team = from;
        for (const auto& planet : planets) {
            if (planet.allegiance != underworld) continue;
            const auto path = view::destination_path(*snapshot, rebel, from->planet, planet.id, planets, false);
            if (path && (route.empty() || path->size() < route.size())) route = *path;
        }
    }
    expect(!route.empty(), "an Underworld planet is reachable from the droid team");
    if (route.empty()) return;
    const auto target = route.back();
    std::cout << label << " target=" << target << " hops=" << route.size() - 1 << '\n';

    const auto tile = [&] {
        return view::ability_drop_candidate(*live->frame().latest, rebel, find_hero_place(*live->frame().latest, rebel, bearer_type)->dragged, host, {}, target);
    };
    view::MapInput input;
    if (in_orbit) {
        // An ordinary order without any ability attached: this is the state the owner stood in.
        std::optional<gc::CommandPayload> move;
        if (team->fleet) {
            const auto intent = view::fleet_destination_intent(*snapshot, rebel, team->dragged, target, planets, {});
            if (intent) move = input.command(*intent, planets);
        } else move = gc::LaunchFleet{{team->dragged}, route, std::nullopt, 0};
        expect(move.has_value(), "the ordinary move order is admitted");
        if (!move) return;
        live->submit(eawr::platform::LiveGameOrder{seat, *move, {}});
        const auto limit = tick + 9000;
        while (tick < limit) {
            step();
            const auto now = find_hero_place(*live->frame().latest, rebel, bearer_type);
            if (now && now->fleet && !now->transit && now->planet == target) break;
            const auto frame = live->frame();
            if (frame.pending_conflict || frame.conflict) break;
        }
        const auto arrived = find_hero_place(*live->frame().latest, rebel, bearer_type);
        const bool orbiting = arrived && arrived->fleet && !arrived->transit && arrived->planet == target;
        std::cout << label << " orbit tick=" << tick << " orbiting=" << orbiting
            << " conflict=" << static_cast<bool>(live->frame().pending_conflict) << " rejected:" << reject_text(rejected) << '\n';
        expect(orbiting, "the droid team reaches the Underworld planet's orbit");
        if (!orbiting) return;
    }

    // The STEAL tile the owner saw, then the release: the same preview, order and command the viewer's drop runs.
    snapshot = live->frame().latest;
    planets = input_planets(*snapshot, rebel);
    const auto drop = tile();
    std::cout << label << " tile=" << (drop ? static_cast<int>(drop->kind) : -1) << '\n';
    expect(drop && drop->kind == gc::AbilityKind::technology_theft && drop->planet == target, "the STEAL tile shows for the droid team");
    if (!drop) return;
    const auto now = find_hero_place(*snapshot, rebel, bearer_type);
    view::DropIntent intent;
    if (in_orbit) {
        intent.kind = view::DropKind::ability;
        intent.source = drop->supplied; intent.planet = drop->planet;
        intent.ability = view::ability_drop_order(*drop);
    } else if (now->fleet) {
        const auto movement = view::fleet_destination_intent(*snapshot, rebel, now->dragged, target, planets, {});
        expect(movement.has_value(), "the distant tile has a route");
        if (!movement) return;
        intent = *movement; intent.destination_actor = drop->supplied;
    } else {
        intent.kind = view::DropKind::launch;
        intent.source = drop->supplied; intent.planet = now->planet; intent.path = route;
        intent.destination_actor = drop->supplied;
    }
    const auto command = input.command(intent, planets);
    expect(command.has_value(), "the drop maps to an order");
    if (!command) return;
    rejected.clear();
    live->submit(eawr::platform::LiveGameOrder{seat, *command, {}});

    const gc::HeroActionMenus* menu{};
    std::shared_ptr<const gc::GalacticSnapshot> held;
    const auto limit = tick + (in_orbit ? 8 : 9000);
    while (tick < limit && !menu) {
        step();
        held = live->frame().latest;
        menu = held->hero_action_menus();
        const auto frame = live->frame();
        if (frame.pending_conflict || frame.conflict) break;
    }
    std::cout << label << " menu tick=" << tick << " published=" << (menu != nullptr)
        << " theft=" << (menu && menu->theft ? static_cast<long long>(menu->theft->size()) : -1)
        << " rejected:" << reject_text(rejected) << '\n';
    expect(menu && menu->player == rebel && menu->planet == target && menu->theft && !menu->theft->empty(),
        "the release publishes the theft menu for the Rebel player");
    expect(rejected.empty(), "the drop order is not rejected");
    if (!menu || !menu->theft) return;
    std::size_t affordable{};
    for (const auto& row : *menu->theft) { affordable += row.affordable; expect(row.live_prepared.has_value(), "each row carries its prepared order"); }
    std::cout << label << " offered=" << menu->theft->size() << " affordable=" << affordable << " tech=" << menu->tech << '\n';
    live->stop();
}

// No campaign this game lists starts a bounty hunter, so this adds one team to a stock sandbox's starting forces.
void hunter_tile(const std::vector<eawr::vfs::MountSpec>& mounts) {
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto bytes = base.open("data/xml/campaigns_underworld_gc.xml").value();
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto campaign = xml.find("<Campaign Name=\"Sandbox_Clusters_Rebel\">");
    const auto forces = campaign == std::string::npos ? campaign : xml.find("<Starting_Forces>", campaign);
    if (forces == std::string::npos) throw std::runtime_error("the stock sandbox has no starting forces");
    xml.insert(forces, "<Starting_Forces> Rebel, Yavin, Bounty_Hunter_Team_R </Starting_Forces>");
    // CTest's working directory is the build tree; retail and mod folders are read-only inputs.
    const auto directory = std::filesystem::current_path() / ("hunter-start-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream out(directory / "xml" / "campaigns_underworld_gc.xml", std::ios::binary); out << xml;
        if (!out) throw std::runtime_error("sandbox overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"hunter-start", directory, "data", {}});
    const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    const auto loaded = take(eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(*filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    run(filesystem, loaded.catalog, data, types, "Sandbox_Clusters_Rebel", 2, true, "Generic_Bounty_Hunter_R");
}

// The owner's #6120 report on stock data: the Empire's Boba Fett stands in orbit of a planet and releases the NEUTRALIZE
// HERO tile on it through the viewer's own preview and command path, against a live session. `boba_planet` empty uses
// the Boba team the campaign itself lists (a planet of the Empire's own, with no enemy hero on it); otherwise the
// overlay adds the Boba team to that planet (his stealth sends it to orbit as a fleet of one) and `extra_forces` adds
// the Underworld hero team the sandbox does not list. A non-empty `enemy_hero` names the Underworld hero Boba first flies
// to (an ordinary move order, as the owner played it) unless `fly` is false, and the menu is asserted whenever it is published.
// Every step prints what it saw.
void boba_neutralize(const std::vector<eawr::vfs::MountSpec>& mounts, std::string_view file, std::string_view campaign_name,
    std::string_view boba_planet, std::string_view extra_forces, bool expect_menu, std::string_view enemy_hero = {}, bool fly = true) {
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto bytes = base.open("data/xml/" + std::string(file)).value();
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto campaign = xml.find("<Campaign Name=\"" + std::string(campaign_name) + "\">");
    const auto forces = campaign == std::string::npos ? campaign : xml.find("<Starting_Forces>", campaign);
    if (forces == std::string::npos) throw std::runtime_error("the stock sandbox has no starting forces");
    if (!boba_planet.empty())
        xml.insert(forces, "<Starting_Forces> Empire, " + std::string(boba_planet) + ", Boba_Fett_Team </Starting_Forces>"
            + std::string(extra_forces));
    const auto directory = std::filesystem::current_path() / ("boba-start-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream out(directory / "xml" / std::string(file), std::ios::binary); out << xml;
        if (!out) throw std::runtime_error("sandbox overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"boba-start", directory, "data", {}});
    const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    const auto loaded = take(eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(*filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const std::string label = "boba-neutralize " + std::string(campaign_name) + (expect_menu ? " enemy-hero" : " own-planet");
    const auto source = std::find_if(data.campaigns.begin(), data.campaigns.end(),
        [&](const auto& row) { return row.name == campaign_name; });
    if (source == data.campaigns.end()) throw std::runtime_error("missing stock campaign " + std::string(campaign_name));
    cp::CampaignSetupOptions options; options.seed = 42;
    std::map<std::string, gc::PlayerId> ids;
    gc::SeatId seat{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        ids[faction.name] = id;
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Empire";
        if (human) { seat = gc::SeatId{id}; options.solo_reference_human = id; }
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    std::map<gc::TypeId, std::string> names;
    gc::TypeId boba_type{}, enemy_type{};
    for (const auto& original : originals) {
        const gc::TypeId id = options.types.size() + 1;
        options.types.push_back({id, original}); names[id] = original;
        if (original == "Boba_Fett") boba_type = id;
        if (!enemy_hero.empty() && original == enemy_hero) enemy_type = id;
    }
    expect(boba_type != 0, "Boba Fett is a registered type");
    const auto empire = ids.at("Empire"), underworld = ids.at("Underworld");
    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, loaded.catalog, data, types,
        campaign_name, options, "Normal_Default", false));
    if (!prepared->layout().ability_host) throw std::runtime_error("stock campaign has no ability host");
    const auto host = *prepared->layout().ability_host;
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));

    std::uint64_t tick{}, cursor{}, prepared_events{};
    std::vector<gc::GalacticEvent> rejected;
    const auto step = [&] {
        live->advance_to(++tick);
        if (!live->wait_for(tick, 60s)) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label + ": the live campaign stalled at tick " + std::to_string(tick));
        }
        const auto events = live->galactic_events_after(cursor);
        cursor = events.through;
        for (const auto& row : events.events)
            if (row.event.kind == gc::GalacticEventKind::command_rejected && row.event.producer == seat) rejected.push_back(row.event);
        for (const auto& row : events.events)
            if (row.event.kind == gc::GalacticEventKind::ability_prepared && row.event.producer == seat) ++prepared_events;
    };
    const auto name_of = [&](gc::TypeId id) { const auto at = names.find(id); return at == names.end() ? std::string("?") : at->second; };
    std::optional<eawr::testing::galactic::HeroPlace> boba;
    while (!boba && tick < 60) { step(); boba = find_hero_place(*live->frame().latest, empire, boba_type); }
    expect(boba.has_value(), "Boba Fett is on the map");
    if (!boba) return;
    view::MapInput input;
    if (enemy_type && fly) {
        // Fly to the planet the Underworld hero stands on; the arrival discovers it for the Empire.
        const auto first = live->frame().latest;
        const auto there = find_hero_place(*first, underworld, enemy_type);
        expect(there.has_value(), "the Underworld hero is on the map");
        if (!there) return;
        auto routing = input_planets(*first, empire);
        for (auto& planet : routing) planet.discovered = true; // route planning only: the real discovery is read after the flight
        const auto move = view::fleet_destination_intent(*first, empire, boba->dragged, there->planet, routing, {});
        expect(move.has_value(), "Boba has a route to the hero's planet");
        if (!move) return;
        const auto move_command = input.command(*move, routing);
        expect(move_command.has_value(), "the move maps to an order");
        if (!move_command) return;
        live->submit(eawr::platform::LiveGameOrder{seat, *move_command, {}});
        bool orbiting = false;
        for (const auto limit = tick + 9000; tick < limit && !orbiting; ) {
            step();
            const auto now = find_hero_place(*live->frame().latest, empire, boba_type);
            orbiting = now && now->fleet && !now->transit && now->planet == there->planet;
            const auto frame = live->frame();
            if (frame.pending_conflict || frame.conflict) { std::cout << label << " conflict at tick " << tick << '\n'; break; }
        }
        std::cout << label << " flight tick=" << tick << " orbiting=" << orbiting << '\n';
        expect(orbiting, "Boba reaches the Underworld hero's planet");
        if (!orbiting) return;
        boba = find_hero_place(*live->frame().latest, empire, boba_type);
        if (!boba) return;
        step(); // one more frame: the discovery and the menu views follow the arrival
    }
    const auto snapshot = live->frame().latest;
    std::cout << label << " discovered=" << view::observer_discovery(*snapshot, empire, boba->planet).value_or(false) << '\n';
    std::cout << label << " boba=" << boba->bearer << " dragged=" << boba->dragged << " planet=" << boba->planet
        << " fleet=" << boba->fleet << " transit=" << boba->transit << '\n';
    auto planets = input_planets(*snapshot, empire);
    // The owner's fleet (ships, not only the stealth hero) revealed the planet; a lone stealth team does not discover it, and the
    // menu view needs that discovery. The host's own preparation does not, so the drop is mapped as the viewer would map it
    // and the host's answer is what is asserted.
    for (auto& planet : planets) if (planet.id == boba->planet) planet.discovered = true;
    const auto here = view::ability_drop_candidate(*snapshot, empire, boba->dragged, host, {}, boba->planet);
    std::cout << label << " tile=" << (here ? static_cast<int>(here->kind) : -1) << '\n';
    expect(here && here->kind == gc::AbilityKind::hero_neutralization && here->planet == boba->planet,
        "Boba Fett's neutralize gets a tile at the planet he orbits");
    if (!here) return;
    view::DropIntent intent;
    intent.kind = view::DropKind::ability;
    intent.source = here->supplied; intent.planet = here->planet;
    intent.ability = view::ability_drop_order(*here);
    const auto command = input.command(intent, planets);
    expect(command.has_value(), "the drop maps to an order");
    if (!command) return;
    rejected.clear();
    live->submit(eawr::platform::LiveGameOrder{seat, *command, {}});
    const gc::HeroActionMenus* menu{};
    std::shared_ptr<const gc::GalacticSnapshot> held;
    for (int wait = 0; wait < 8 && !menu && rejected.empty(); ++wait) {
        step(); held = live->frame().latest; menu = held->hero_action_menus();
    }
    std::cout << label << " prepared_events=" << prepared_events << '\n';
    std::cout << label << " menu published=" << (menu != nullptr)
        << " neutralization=" << (menu && menu->neutralization ? static_cast<long long>(menu->neutralization->size()) : -1)
        << " rejected:" << reject_text(rejected) << '\n';
    bool tyber = false;
    if (menu && menu->neutralization) for (const auto& row : *menu->neutralization) {
        std::cout << label << " choice current=" << name_of(row.current_type) << " original=" << name_of(row.original_type)
            << " affordable=" << row.affordable << " live=" << row.live_prepared.has_value() << '\n';
        tyber = tyber || name_of(row.current_type) == "Tyber_Zann" || name_of(row.original_type) == "Tyber_Zann";
    }
    if (expect_menu) {
        expect(rejected.empty(), "the neutralize drop is not rejected");
        expect(prepared_events > 0, "the host prepared the neutralization menu for the drop");
        // The host prepares only when it finds a hero that is not the seat's own on that planet. The overlay put exactly one
        // there, Tyber Zann's team, so the preparation offers him: assert that he stands on the planet Boba dropped on, and that
        // he is in the menu whenever the menu view is published (it needs the planet discovered, which a lone stealth hero does
        // not do; the owner has it from ordinary play).
        const auto tyber_place = find_hero_place(*live->frame().latest, underworld, enemy_type);
        expect(tyber_place.has_value() && tyber_place->planet == boba->planet,
            "Tyber Zann is on the planet Boba dropped on, the only hero the host could offer there");
        if (!fly) {
            // The planet is the Empire's own, so it is discovered and the host's menu is published: Tyber Zann must be in it.
            expect(menu && menu->neutralization && !menu->neutralization->empty(), "the host's neutralization menu is published");
            expect(tyber, "Tyber Zann is in the host's neutralization menu");
        } else if (menu && menu->neutralization) expect(tyber, "Tyber Zann is in the published neutralization menu");
        // The host emits ability_prepared only when it found a hero to offer: Tyber Zann's team on the planet's ground.
        std::cout << label << " menu view=" << (menu != nullptr) << " tyber_in_menu=" << tyber << '\n';
    } else {
        // No enemy hero stands on the Empire's own planet: the refusal is right, and it says so.
        expect(!menu && std::any_of(rejected.begin(), rejected.end(), [](const auto& event) {
            return event.reason == gc::GalacticRejectReason::neutralize_no_hero; }),
            "a drop on a planet with no enemy hero is refused as no enemy hero there");
    }
    live->stop();
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock steal drop requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
        const auto loaded = take(eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc));
        const auto data = take(eawr::data::galactic::load_galactic_data(*filesystem, loaded.catalog));
        const auto types = eawr::units::galactic_type_table(loaded.catalog);
        run(filesystem, loaded.catalog, data, types, "Sandbox_Clusters_Rebel", 2, true);
        run(filesystem, loaded.catalog, data, types, "Sandbox_Heart_Of_The_Maw_Rebel", 3, true);
        run(filesystem, loaded.catalog, data, types, "Sandbox_Clusters_Rebel", 2, false);
        hunter_tile(mounts);
        // Boba on his own planet, no enemy hero there: refused, and the refusal says so.
        boba_neutralize(mounts, "campaigns_multiplayer.xml", "Multiplayer_Sandbox_Origin", "", "", false);
        // Boba flies from Coruscant to Hypori, where Tyber Zann's team stands on the ground: the menu opens and offers him.
        boba_neutralize(mounts, "campaigns_underworld_gc.xml", "Sandbox_Origin_Empire", "Coruscant",
            "<Starting_Forces> Underworld, Hypori, Tyber_Zann_Team </Starting_Forces>", true, "Tyber_Zann");
        // The same hero on the Empire's own, discovered planet (Coruscant, with Boba): no flight, so the host's menu view is
        // published and the offer itself is asserted: Tyber Zann is in it.
        boba_neutralize(mounts, "campaigns_underworld_gc.xml", "Sandbox_Origin_Empire", "Coruscant",
            "<Starting_Forces> Underworld, Coruscant, Tyber_Zann_Team </Starting_Forces>", true, "Tyber_Zann", false);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

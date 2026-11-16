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
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ability_fixture.hpp"
#include "eawr/vfs/vfs.hpp"

// Repro matrix for the owner's "the corruption actions of the corrupting units never pop up" report. It is data-driven:
// for each stock sandbox campaign it finds every hero company of the active faction that has a member carrying an authored
// galactic ability, adds those companies to the faction's first starting planet, starts the live campaign, and asks the
// viewer's own drop preview (ability_drop_candidate) what each company's drag offers on every planet. One line per company
// says which authored abilities its member carries and which tiles the preview found, so the first broken link per class
// is visible without naming any unit in code. A company with an authored ability that gets no tile on any planet fails.
namespace {
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;
using namespace std::chrono_literals;

constexpr std::string_view kFile = "campaigns_underworld_gc.xml";

std::vector<std::string> authored_kinds(const gc::GalacticAbilityProfile& profile) {
    std::vector<std::string> kinds;
    if (profile.lifecycle_only) return kinds;
    if (profile.theft) kinds.emplace_back("theft");
    if (profile.corruption) kinds.emplace_back("corruption");
    if (profile.hack) kinds.emplace_back("hack");
    if (profile.destruction) kinds.emplace_back("destruction");
    if (profile.sabotage) kinds.emplace_back("sabotage");
    if (profile.neutralize) kinds.emplace_back("neutralize");
    if (profile.spy) kinds.emplace_back("spy");
    if (profile.removal) kinds.emplace_back("removal");
    return kinds;
}

std::string kind_name(gc::AbilityKind kind) {
    switch (kind) {
    case gc::AbilityKind::black_market: return "black_market";
    case gc::AbilityKind::technology_theft: return "theft";
    case gc::AbilityKind::corruption: return "corruption";
    case gc::AbilityKind::superweapon_hack: return "hack";
    case gc::AbilityKind::planet_destruction: return "destruction";
    case gc::AbilityKind::structure_sabotage: return "sabotage";
    case gc::AbilityKind::hero_neutralization: return "neutralize";
    case gc::AbilityKind::system_spy: return "spy";
    case gc::AbilityKind::remove_corruption: return "removal";
    }
    return "?";
}

std::string join(const std::set<std::string>& values) {
    std::string text;
    for (const auto& value : values) text += (text.empty() ? "" : ",") + value;
    return text.empty() ? "-" : text;
}

using Loaded = decltype(take(eawr::data::load_catalog(std::declval<const eawr::vfs::Vfs&>(), eawr::data::Profile::foc)));
struct Content {
    Loaded loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
};
Content load_content(const eawr::vfs::Vfs& filesystem) {
    auto loaded = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = take(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    auto types = eawr::units::galactic_type_table(loaded.catalog);
    return Content{std::move(loaded), std::move(data), std::move(types)};
}

struct Sandbox {
    std::string faction, planet;
    std::size_t insert_at{};
};

Sandbox find_sandbox(const std::string& xml, std::string_view campaign_name) {
    const auto campaign = xml.find("<Campaign Name=\"" + std::string(campaign_name) + "\">");
    if (campaign == std::string::npos) throw std::runtime_error("missing stock campaign " + std::string(campaign_name));
    const auto active = xml.find("<Starting_Active_Player>", campaign);
    const auto close = xml.find("</Starting_Active_Player>", active);
    if (active == std::string::npos || close == std::string::npos) throw std::runtime_error("campaign has no active player");
    Sandbox box;
    const auto begin = active + std::string_view("<Starting_Active_Player>").size();
    for (auto at = begin; at < close; ++at) if (xml[at] != ' ' && xml[at] != '\t' && xml[at] != '\r' && xml[at] != '\n') box.faction += xml[at];
    const auto forces = xml.find("<Starting_Forces> " + box.faction + ",", campaign);
    if (forces == std::string::npos) throw std::runtime_error("the active faction has no starting forces");
    const auto planet_begin = forces + std::string("<Starting_Forces> ").size() + box.faction.size() + 1;
    const auto planet_end = xml.find(',', planet_begin);
    for (auto at = planet_begin; at < planet_end; ++at) if (xml[at] != ' ') box.planet += xml[at];
    box.insert_at = forces;
    return box;
}


eawr::sim::math::Fixed credits_of(const gc::GalacticSession& world, gc::PlayerId player) {
    for (const auto& row : world.snapshot()->player_economy()->players) if (row.player == player) return row.credits;
    return {};
}
// WGCH-198: a solo human corruption choice that authors a land mission runs on the timed route. The choice is the stock
// Underworld sandbox's own authored land choice (type, price, map, script, mode), played in the ability contract
// galaxy; its other prerequisites are separate gates and are dropped.
void land_choice_timed(gc::CorruptionChoice land, const std::string& label) {
    land.unit_count = 0; land.unit_types.clear(); land.mission_count = 0; land.planet_types.clear();
    land.required_mission_type = gc::corruption_fixture::Type::none;
    const auto wanted = land.type;
    auto config = ability_fixture::base_setup(); auto& host = *config.ability_host;
    host.profiles[0].has_market = false; host.profiles[0].corruption = gc::CorruptionAbilityProfile{"ACT", true};
    host.activated_corruption_abilities = {"ACT"};
    config.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
    auto& control = ability_fixture::add_control(config); control.types = {{1000, 0, false, false}};
    config.census->types.push_back({3000, {}, 0});
    for (const auto& planet : config.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 3000; row.retains_residual = true;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1}; control.planets.push_back(row);
        gc::CorruptionPlanet p; p.planet = planet.id; p.type = 3000; p.retains_influence = true;
        p.slots[0] = land;
        host.corruption_planets.push_back(p);
    }
    for (const gc::PlayerId id : ability_fixture::all_players) {
        gc::CorruptionPlayer player; player.player = id; host.corruption_players.push_back(player);
    }
    for (auto& player : config.player_economy->players) player.credits = take(eawr::sim::math::Fixed::from_integer(1000000));
    ability_fixture::add_observer(config, 2, {1000, 3000});
    eawr::platform::ThreadWorkerAdapter pool(2);
    auto game = take(eawr::campaign::GameSession::create(config));
    std::uint64_t sequence{};
    const auto order = [&](gc::CommandPayload payload) {
        take(game.submit({{game.completed_tick(), {2}, ++sequence}, std::move(payload)}));
        (void)take(game.step(pool));
    };
    order(gc::PrepareGalacticAbility{config.fleets[1].members[0].id, config.planets[0].id, gc::AbilityKind::corruption});
    const auto* state = game.galaxy().ability_state();
    expect(state && state->preparations.size() == 1 && !state->preparations[0].corruption_choices.empty(), label + ": the corruption preparation opens");
    if (!state || state->preparations.empty() || state->preparations[0].corruption_choices.empty()) return;
    const auto* menu = game.galaxy().snapshot()->corruption_menu();
    expect(menu && !menu->choices.empty() && menu->choices[0].mission_available, label + ": the stock land choice is offered available");
    const auto prepared = gc::prepared_ability(state->preparations[0], 0);
    const auto before = credits_of(game.galaxy(), 2);
    order(gc::ExecutePreparedAbility{prepared});
    state = game.galaxy().ability_state();
    expect(!state->transitions.empty() && state->transitions[0].transition.active() && state->missions.empty()
        && credits_of(game.galaxy(), 2) < before, label + ": the stock land choice debits, queues no mission and starts the timed transition");
    if (state->transitions.empty()) return;
    const auto end = state->transitions[0].transition.end;
    for (unsigned guard = 0; game.galaxy().clock().galactic_frame <= end && guard < 600; ++guard) (void)take(game.step(pool));
    const auto* economic = game.galaxy().economy_corruption();
    expect(economic && economic->planets[0].corruption == wanted, label + ": the stock land choice reaches its timed outcome");
}

void matrix(const std::vector<eawr::vfs::MountSpec>& mounts, std::string_view campaign_name) {
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto bytes = base.open("data/xml/" + std::string(kFile)).value();
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto box = find_sandbox(xml, campaign_name);
    const std::string label = "actor-matrix " + std::string(campaign_name);

    // Pass 1 (no overlay): which hero companies of the faction have a member with an authored ability.
    struct Row { std::string company, member; gc::TypeId member_type{}; std::vector<std::string> authored; };
    const auto plain = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    auto content = load_content(*plain);
    const auto& loaded = content.loaded;
    const auto& data = content.data;
    const auto& types = content.types;
    const auto source = std::find_if(data.campaigns.begin(), data.campaigns.end(),
        [&](const auto& row) { return row.name == campaign_name; });
    if (source == data.campaigns.end()) throw std::runtime_error("missing stock campaign " + std::string(campaign_name));

    eawr::campaign::CampaignSetupOptions options; options.seed = 42;
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
    std::map<std::string, gc::TypeId> type_ids;
    for (const auto& original : originals) {
        const gc::TypeId id = options.types.size() + 1;
        options.types.push_back({id, original});
        type_ids[original] = id;
    }
    const auto human = ids.at(source->starting_active_player);

    const auto first = take(eawr::platform::PreparedLiveCampaign::create(plain, loaded.catalog, data, types,
        campaign_name, options, "Normal_Default", false));
    if (!first->layout().ability_host) throw std::runtime_error("stock campaign has no ability host");
    if (campaign_name == "Sandbox_Origin_Underworld") {
        const gc::CorruptionChoice* donor = nullptr;
        for (const auto& planet : first->layout().ability_host->corruption_planets) {
            for (const auto& choice : planet.slots)
                if (!donor && choice.mode == 1 && !choice.map.empty() && choice.type != gc::corruption_fixture::Type::none) donor = &choice;
        }
        expect(donor, label + ": the stock sandbox authors a land mission choice");
        if (donor) land_choice_timed(*donor, label);
    }
    std::vector<Row> rows;
    for (const auto& type : types.types) {
        if (type.company_units.empty()) continue;
        if (std::find(type.affiliation.begin(), type.affiliation.end(), box.faction) == type.affiliation.end()) continue;
        for (const auto& member : type.company_units) {
            const auto id = type_ids.find(member);
            if (id == type_ids.end()) continue;
            const auto profile = std::find_if(first->layout().ability_host->profiles.begin(),
                first->layout().ability_host->profiles.end(), [&](const auto& row) { return row.type == id->second; });
            if (profile == first->layout().ability_host->profiles.end()) continue;
            auto authored = authored_kinds(*profile);
            if (authored.empty()) continue;
            rows.push_back({type.name, member, id->second, std::move(authored)});
            break;
        }
    }
    // The game constants route corruption abilities by name. The stock list names the older form first, which no stock unit
    // authors (its block is commented out), and the one the saboteur authors second: some routed name must be authored.
    {
        const auto& routed = first->layout().ability_host->activated_corruption_abilities;
        const auto& profiles = first->layout().ability_host->profiles;
        expect(routed.empty() || std::any_of(profiles.begin(), profiles.end(), [&](const auto& profile) {
            return profile.corruption && std::find(routed.begin(), routed.end(), profile.corruption->name) != routed.end(); }),
            label + ": a routed corruption ability is authored by a unit profile");
    }
    std::cout << label << " faction=" << box.faction << " planet=" << box.planet << " ability companies=" << rows.size() << '\n';
    expect(!rows.empty(), "the stock faction has hero companies with authored abilities");
    if (rows.empty()) return;

    // Pass 2: the same sandbox with every such company added at the faction's first starting planet.
    std::string overlay;
    for (const auto& row : rows)
        overlay += "<Starting_Forces> " + box.faction + ", " + box.planet + ", " + row.company + " </Starting_Forces>\n";
    xml.insert(box.insert_at, overlay);
    const auto directory = std::filesystem::current_path() / ("actor-matrix-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream out(directory / "xml" / std::string(kFile), std::ios::binary); out << xml;
        if (!out) throw std::runtime_error("sandbox overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"actor-matrix", directory, "data", {}});
    const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    auto content2 = load_content(*filesystem);
    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, content2.loaded.catalog, content2.data, content2.types,
        campaign_name, options, "Normal_Default", false));
    const auto host = *prepared->layout().ability_host;
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
    std::uint64_t tick{};
    for (int wait = 0; wait < 3; ++wait) {
        live->advance_to(++tick);
        if (!live->wait_for(tick, 60s)) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label + ": the live campaign stalled at tick " + std::to_string(tick));
        }
    }
    const auto snapshot = live->frame().latest;
    for (const auto& row : rows) {
        const auto place = find_hero_place(*snapshot, human, row.member_type);
        std::set<std::string> authored(row.authored.begin(), row.authored.end()), found, own;
        std::size_t tiles{};
        if (place) for (const auto& planet : snapshot->planets()) {
            const auto drop = view::ability_drop_candidate(*snapshot, human, place->dragged, host, {}, planet.id);
            if (!drop) continue;
            ++tiles;
            found.insert(kind_name(drop->kind));
            if (planet.id == place->planet) own.insert(kind_name(drop->kind));
        }
        // Removal and sabotage need a corrupted planet and a hack needs a weapon in orbit, which no stock start has: a company
        // whose authored kinds are all of those cannot get a tile here, so its tile count is reported but not required.
        const bool startable = std::any_of(row.authored.begin(), row.authored.end(), [](const std::string& kind) {
            return kind != "removal" && kind != "sabotage" && kind != "hack"; });
        std::cout << label << ' ' << row.company << " member=" << row.member << " placed=" << place.has_value()
            << " fleet=" << (place && place->fleet) << " authored=" << join(authored) << " tiles=" << tiles
            << " kinds=" << join(found) << " own_planet=" << join(own) << '\n';
        expect(place.has_value(), row.company + ": the company is on the map");
        expect(!place || !startable || tiles > 0, row.company + ": an authored ability that needs no prepared planet gets a drop tile on some planet");
    }
    live->stop();
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock actor matrix requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        for (const auto* campaign : {"Sandbox_Origin_Empire", "Sandbox_Origin_Rebel", "Sandbox_Origin_Underworld"})
            matrix(mounts, campaign);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

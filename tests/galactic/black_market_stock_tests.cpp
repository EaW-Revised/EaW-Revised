#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
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
#include "eawr/data/xml.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"
#include "galactic/ability_drop.hpp"

// Black-market end-to-end check on the installed stock Underworld sandbox (WGEP-04/55/58/80/138..145, #1130).
// The sandbox is the real stock campaign: its hero companies that carry the market ability are added at the first starting
// planet of every other playable faction (so each stands over an opposing planet), the planet is given the black-market
// corruption, and the hero's own ability host prepares, buys and refuses exactly as the viewer's drag does.
namespace {
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;


std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
struct Start { std::string faction, planet; };

std::vector<Start> starting_forces(const std::string& xml, std::string_view campaign) {
    const auto begin = xml.find("<Campaign Name=\"" + std::string(campaign) + "\">");
    if (begin == std::string::npos) throw std::runtime_error("missing stock campaign " + std::string(campaign));
    const auto end = xml.find("</Campaign>", begin);
    std::vector<Start> rows;
    constexpr std::string_view open = "<Starting_Forces>";
    for (auto at = xml.find(open, begin); at != std::string::npos && at < end; at = xml.find(open, at + 1)) {
        const auto close = xml.find("</Starting_Forces>", at);
        const auto body = xml.substr(at + open.size(), close - at - open.size());
        const auto first = body.find(','), second = body.find(',', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        rows.push_back({trim(body.substr(0, first)), trim(body.substr(first + 1, second - first - 1))});
    }
    return rows;
}

std::string upper(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

Fixed credits_of(const gc::GalacticSession& world, gc::PlayerId player) {
    for (const auto& row : world.snapshot()->player_economy()->players) if (row.player == player) return row.credits;
    return {};
}

using Loaded = decltype(take(eawr::data::load_catalog(std::declval<const eawr::vfs::Vfs&>(), eawr::data::Profile::foc)));
struct Content {
    Loaded loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
};
Content load_content(const eawr::vfs::Vfs& filesystem, eawr::data::Profile profile) {
    auto loaded = take(eawr::data::load_catalog(filesystem, profile));
    auto data = take(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    auto types = eawr::units::galactic_type_table(loaded.catalog);
    return Content{std::move(loaded), std::move(data), std::move(types)};
}

void check_campaign(const std::vector<eawr::vfs::MountSpec>& mounts, std::string_view campaign_name, std::string_view kFile,
    eawr::data::Profile profile) {
    const std::string label = "black-market " + std::string(campaign_name);
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto bytes = base.open("data/xml/" + std::string(kFile)).value();
    auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto forces = starting_forces(xml, campaign_name);
    const auto plain = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    auto content = load_content(*plain, profile);
    const auto source = std::find_if(content.data.campaigns.begin(), content.data.campaigns.end(),
        [&](const auto& row) { return row.name == campaign_name; });
    if (source == content.data.campaigns.end()) throw std::runtime_error("missing stock campaign " + std::string(campaign_name));
    const std::string active = source->starting_active_player;

    eawr::campaign::CampaignSetupOptions options; options.seed = 42;
    std::map<std::string, gc::PlayerId> ids;
    for (const auto& faction : content.data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        ids[upper(faction.name)] = id;
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = upper(faction.name) == upper(active);
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : content.types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    std::map<std::string, gc::TypeId> type_ids;
    for (const auto& original : originals) {
        const gc::TypeId id = options.types.size() + 1;
        options.types.push_back({id, original});
        type_ids[original] = id;
    }
    const auto human = ids.at(upper(active));

    // Targets: the first starting planet of every other playable faction that owns black-market items.
    std::set<std::string> item_factions;
    {
        const auto first = take(eawr::platform::PreparedLiveCampaign::create(plain, content.loaded.catalog, content.data,
            content.types, campaign_name, options, "Normal_Default", false));
        const auto& host = *first->layout().ability_host;
        const auto* state = first->layout().economy_corruption ? &*first->layout().economy_corruption : nullptr;
        expect(state, label + ": the campaign carries the corruption economy state");
        if (!state) return;
        std::cout << label << " items=" << state->items.size() << " buyer profiles="
            << std::count_if(host.profiles.begin(), host.profiles.end(), [](const auto& row) { return row.has_market; }) << '\n';
        for (const auto& item : state->items)
            for (const auto& player : state->players) if (player.faction == item.faction) item_factions.insert(std::to_string(player.player));
    }
    std::vector<Start> targets;
    for (const auto& row : forces)
        if (upper(row.faction) != upper(active) && ids.contains(upper(row.faction))
            && item_factions.contains(std::to_string(ids.at(upper(row.faction))))
            && std::none_of(targets.begin(), targets.end(), [&](const auto& t) { return t.faction == row.faction; }))
            targets.push_back(row);
    expect(!targets.empty(), label + ": a playable faction with black-market items starts with a planet");
    if (targets.empty()) return;

    // Market-capable hero companies of the active faction.
    const auto first = take(eawr::platform::PreparedLiveCampaign::create(plain, content.loaded.catalog, content.data,
        content.types, campaign_name, options, "Normal_Default", false));
    struct Hero { std::string company, member; gc::TypeId member_type{}; };
    std::vector<Hero> heroes;
    for (const auto& type : content.types.types) {
        if (type.company_units.empty()) continue;
        if (std::find(type.affiliation.begin(), type.affiliation.end(), active) == type.affiliation.end()
            && std::find_if(type.affiliation.begin(), type.affiliation.end(), [&](const auto& a) { return upper(a) == upper(active); }) == type.affiliation.end()) continue;
        for (const auto& member : type.company_units) {
            const auto id = type_ids.find(member);
            if (id == type_ids.end()) continue;
            const auto profile = std::find_if(first->layout().ability_host->profiles.begin(), first->layout().ability_host->profiles.end(),
                [&](const auto& row) { return row.type == id->second; });
            if (profile == first->layout().ability_host->profiles.end() || !profile->has_market || !profile->initially_enabled) continue;
            heroes.push_back({type.name, member, id->second});
            break;
        }
    }
    std::cout << label << " active=" << active << " market heroes=" << heroes.size() << " targets=" << targets.size() << '\n';
    expect(!heroes.empty(), label + ": the active faction has hero companies with the market ability");
    if (heroes.empty()) return;

    std::string overlay;
    const auto anchor = xml.find("<Starting_Forces>", xml.find("<Campaign Name=\"" + std::string(campaign_name) + "\">"));
    for (std::size_t index = 0; index < std::min(heroes.size(), targets.size()); ++index)
        overlay += "<Starting_Forces> " + active + ", " + targets[index].planet + ", " + heroes[index].company + " </Starting_Forces>\n";
    xml.insert(anchor, overlay);
    const auto directory = std::filesystem::current_path() / ("black-market-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    std::filesystem::create_directories(directory / "xml");
    {
        std::ofstream out(directory / "xml" / std::string(kFile), std::ios::binary); out << xml;
        if (!out) throw std::runtime_error("sandbox overlay write failed");
    }
    auto layered = mounts;
    layered.insert(layered.begin(), {"black-market", directory, "data", {}});
    const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(layered)));
    auto content2 = load_content(*filesystem, profile);
    const auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, content2.loaded.catalog, content2.data,
        content2.types, campaign_name, options, "Normal_Default", false));
    const gc::GalacticSetup stock = prepared->layout();
    eawr::platform::ThreadWorkerAdapter pool(2);

    for (std::size_t index = 0; index < std::min(heroes.size(), targets.size()); ++index) {
        const auto& hero = heroes[index];
        const auto owner = ids.at(upper(targets[index].faction));
        const std::string row = label + ' ' + hero.company + " over " + targets[index].faction + ':';
        auto probe = take(eawr::campaign::GameSession::create(stock));
        const auto place = find_hero_place(*probe.galaxy().snapshot(), human, hero.member_type);
        expect(place.has_value(), row + " the hero is on the map");
        if (!place) continue;
        const auto planets = probe.galaxy().snapshot()->planets();
        const auto planet = std::find_if(planets.begin(), planets.end(), [&](const auto& p) { return p.id == place->planet; });
        expect(planet != planets.end() && planet->owner == owner, row + " the hero orbits a planet of the other faction");
        if (planet == planets.end() || planet->owner != owner) continue;

        for (const bool rich : {true, false}) {
            auto config = stock;
            for (auto& state : config.economy_corruption->planets)
                if (state.planet == place->planet) state.corruption = gc::corruption_fixture::Type::black_market;
            for (auto& account : config.player_economy->players)
                if (account.player == human) account.credits = take(Fixed::from_integer(rich ? 1000000 : 0));
            auto game = take(eawr::campaign::GameSession::create(config));
            std::uint64_t sequence{};
            const auto order = [&](gc::CommandPayload payload) {
                take(game.submit({{game.completed_tick(), gc::SeatId{human}, ++sequence}, std::move(payload)}));
                (void)take(game.step(pool));
            };
            // The drag the owner makes: the viewer's own tile test on the live snapshot, as for the other hero abilities. The
            // hero must stand alone in its fleet (a stacked fleet is refused, WGCH-152), and the planet must carry the market.
            if (rich) {
                const auto tile = view::ability_drop_candidate(*game.galaxy().snapshot(), human, place->dragged, *stock.ability_host, {}, place->planet);
                std::cout << row << " drag tile=" << (tile ? static_cast<int>(tile->kind) : -1) << " fleet=" << place->fleet << '\n';
                expect(tile && tile->kind == gc::AbilityKind::black_market && tile->planet == place->planet,
                    row + " dragging the hero onto the corrupted planet gets a black-market tile");
                auto plain_config = stock;
                for (auto& account : plain_config.player_economy->players)
                    if (account.player == human) account.credits = take(Fixed::from_integer(1000000));
                auto plain_game = take(eawr::campaign::GameSession::create(plain_config));
                const auto none = view::ability_drop_candidate(*plain_game.galaxy().snapshot(), human, place->dragged, *stock.ability_host, {}, place->planet);
                std::cout << row << " drag tile without corruption=" << (none ? static_cast<int>(none->kind) : -1) << '\n';
                expect(!none || none->kind != gc::AbilityKind::black_market,
                    row + " a planet without the market corruption gets no black-market tile");
            }
            order(gc::PrepareGalacticAbility{place->bearer, place->planet, gc::AbilityKind::black_market});
            const auto* ability = game.galaxy().ability_state();
            const auto* menu_state = game.galaxy().economy_corruption();
            expect(ability && menu_state && ability->preparations.size() == 1
                && ability->preparations[0].kind == gc::AbilityKind::black_market && !ability->preparations[0].offers.empty(),
                row + (rich ? " rich" : " broke") + ": the black market opens with offers");
            if (!ability || ability->preparations.empty() || ability->preparations[0].offers.empty()) continue;
            const auto& prep = ability->preparations[0];
            const auto seller_at = std::find_if(menu_state->players.begin(), menu_state->players.end(),
                [&](const auto& p) { return p.player == owner; });
            const auto buyer_at = std::find_if(menu_state->players.begin(), menu_state->players.end(),
                [&](const auto& p) { return p.player == human; });
            expect(seller_at != menu_state->players.end() && buyer_at != menu_state->players.end(),
                row + ": the planet owner and the human both have a corruption economy row");
            if (seller_at == menu_state->players.end() || buyer_at == menu_state->players.end()) continue;
            const auto* seller = &*seller_at;
            const auto* buyer_player = &*buyer_at;
            bool filtered = true;
            for (const auto& offer : prep.offers) {
                const auto item = std::find_if(menu_state->items.begin(), menu_state->items.end(), [&](const auto& i) { return i.id == offer.item; });
                filtered = filtered && item != menu_state->items.end() && item->faction == seller->faction;
            }
            expect(filtered, row + " every offer belongs to the planet owner's faction");
            if (rich) {
                std::cout << row << " offers=" << prep.offers.size() << " first price=" << prep.offers[0].price.raw()
                    << " tech_mult=" << buyer_player->tech_purchase_multiplier.raw() << " absence=" << prep.absence.raw() << '\n';
            }
            const auto before = credits_of(game.galaxy(), human);
            const auto results_before = game.galaxy().ability_state()->results.size();
            const auto price = prep.offers[0].price;
            const auto item = prep.offers[0].item;
            order(gc::ExecutePreparedAbility{gc::prepared_ability(prep, 0)});
            const auto after = credits_of(game.galaxy(), human);
            const auto* bought = game.galaxy().economy_corruption();
            const bool unlocked = std::find_if(bought->players.begin(), bought->players.end(), [&](const auto& p) {
                return p.player == human && p.unlocks.contains(item); }) != bought->players.end();
            if (rich) {
                expect(after == Fixed::from_raw(before.raw() - price.raw()) && unlocked,
                    row + " the purchase debits the exact offer price and unlocks the item");
                const auto& results = game.galaxy().ability_state()->results;
                expect(!results.empty() && results.back().outcome == gc::AbilityOutcome::purchased,
                    row + " the host records a purchased result");
                const auto buyer = std::find_if(bought->buyers.begin(), bought->buyers.end(), [&](const auto& b) { return b.buyer == place->bearer; });
                const auto actors = game.galaxy().snapshot()->actor_records();
                const auto gone = std::find_if(actors.begin(), actors.end(), [&](const auto& a) {
                    return a.id == place->bearer && !a.dead && !a.delete_pending; }) == actors.end();
                std::cout << row << " debit=" << (before.raw() - after.raw()) << " unlocked=" << unlocked
                    << " buyer_present=" << (buyer != bought->buyers.end() && buyer->present) << " hero_gone=" << gone << '\n';
                expect(gone, row + " the buyer hero is consumed (despawn route)");
            } else {
                expect(after == before && !unlocked, row + " insufficient cash: no debit, no unlock");
                // The refusal must be the host's own answer to this purchase, not any other rejection of the command.
                const auto& results = game.galaxy().ability_state()->results;
                expect(results.size() == results_before + 1 && results.back().outcome == gc::AbilityOutcome::refused,
                    row + " insufficient cash: the host records one refused result for the purchase");
                const auto actors = game.galaxy().snapshot()->actor_records();
                const bool present = std::find_if(actors.begin(), actors.end(), [&](const auto& a) {
                    return a.id == place->bearer && !a.dead && !a.delete_pending; }) != actors.end();
                expect(present, row + " insufficient cash: the buyer hero is not consumed");
            }
        }
    }
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the black-market stock check requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        check_campaign(mounts, "Sandbox_Origin_Underworld", "campaigns_underworld_gc.xml", eawr::data::Profile::foc);
        // The same flow on the Remake chain, when the host has it (the owner plays this data): the Underworld Web Way campaign.
        if (const auto mod = eawr::testing::remake_mount()) {
            auto layers = eawr::vfs::mod_chain_roots(std::filesystem::path(mod->mod_root));
            layers.emplace_back("expansion", std::filesystem::path(*root) / "corruption" / "Data");
            layers.emplace_back("base", std::filesystem::path(*root) / "GameData" / "Data");
            std::vector<eawr::vfs::MountSpec> remake;
            for (auto& layer : take(eawr::vfs::resolve_manifest_chain(layers))) remake.push_back(std::move(layer.mount));
            check_campaign(remake, "GC_Standard_Web_Way_Underworld", "campaigns_imperial_web_way_3standard.xml", eawr::data::Profile::remake);
        } else {
            std::cout << "black-market remake: EAWR_REMAKE_MOD_ROOT/CHAIN not set, the Remake pass is skipped\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

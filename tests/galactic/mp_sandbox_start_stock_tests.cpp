#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/vfs/vfs.hpp"

// #7058: a stock multiplayer entry authors no Starting_Active_Player, so a one-human local start has to take its human
// faction from the entry's lobby row and its rules from the entry, as the lobby does. This starts the named stock entry
// through the live path the viewer and sim_headless share (prepared campaign, native AI, live session) and steps it.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using namespace std::chrono_literals;

// `ambient_only`: every playable faction is a human side, so the only AI seats left are ambient factions. A multiplayer
// entry that pairs none of them (the stock entries author no AI_Player_Control) starts without a native AI, as an online
// versus room whose humans hold every playable faction does (GCMP-AI-03), instead of refusing 'selected stock players'.
void start_entry(const std::shared_ptr<const eawr::vfs::Vfs>& filesystem, const eawr::data::Catalog& catalog,
    const eawr::data::galactic::GalacticData& data, std::string_view name, bool ambient_only) {
    const auto* source = data.campaign(name);
    if (!source) throw std::runtime_error("missing stock campaign " + std::string(name));
    expect(source->is_multiplayer.value_or(false) && source->starting_active_player.empty(),
        std::string(name) + " is a multiplayer entry with no authored starting faction");
    const auto referenced = cp::campaign_referenced_types(data, name);
    const auto types = eawr::units::galactic_type_table(catalog, false, referenced);

    cp::CampaignSetupOptions options; options.seed = 42;
    options.rules_flavour = cp::campaign_rules_flavour(*source);
    const auto human_faction = cp::local_start_faction(data, *source);
    expect(!human_faction.empty(), std::string(name) + " offers a faction for the local human");
    bool paired_ambient = false;
    std::size_t humans{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool playable = faction.is_playable.value_or(false) && !faction.is_neutral.value_or(false);
        const bool human = ambient_only ? playable : faction.name == human_faction;
        // Several humans have no solo reference: the observer roster holds every human side, as in a networked room.
        if (!ambient_only && human) options.solo_reference_human = id;
        if (!playable && !faction.is_neutral.value_or(false))
            paired_ambient = paired_ambient || std::any_of(source->ai_player_control.begin(), source->ai_player_control.end(),
                [&](const auto& pair) { return pair.faction == faction.name; });
        // A galaxy has at most two human sides: in the ambient-only layout the first human holds side 1, the rest side 2.
        const gc::HumanSideId side = !human ? 0U : !ambient_only ? static_cast<gc::HumanSideId>(id) : humans++ == 0 ? 1U : 2U;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, side});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});

    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem, catalog, data, types, name, options,
        "Normal_Default", true));
    if (ambient_only) expect(prepared->native_ai_installed() == paired_ambient, std::string(name)
        + (paired_ambient ? " installs the native AI of its paired ambient factions" : " starts its ambient-only AI seats without a native AI"));
    else expect(prepared->native_ai_installed(), std::string(name) + " installs the native AI");
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
    constexpr std::uint64_t ticks = 30;
    live->advance_to(ticks);
    if (!live->wait_for(ticks, 120s)) {
        if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
        throw std::runtime_error(std::string(name) + ": the live campaign stalled");
    }
    expect(!live->failure().has_value(), std::string(name) + " runs " + std::to_string(ticks) + " ticks without a failure");
    live->stop();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock multiplayer start requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        const bool ambient_only = argc == 3 && std::string_view(argv[2]) == "--ambient-only";
        if (argc != 2 && !ambient_only) throw std::runtime_error("usage: galactic_mp_sandbox_start_stock_tests <campaign> [--ambient-only]");
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        const auto filesystem = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
        const auto loaded = take(eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc));
        const auto data = take(eawr::data::galactic::load_galactic_data(*filesystem, loaded.catalog));
        start_entry(filesystem, loaded.catalog, data, argv[1], ambient_only);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

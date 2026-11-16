#include <algorithm>
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
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

// #7187: on stock data, a destroyed planet is a simulation fact the map reads. The planet's type authors the model the map
// draws for it (`Destroyed_Galactic_Model_Name`); the destroyed state itself comes from the shared ruins request.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;

bool destroyed(const gc::GalacticSnapshot& snapshot, const gc::PlanetId planet) {
    for (const auto& row : snapshot.control()->planets) if (row.planet == planet) return row.destroyed;
    throw std::runtime_error("the planet has no control row");
}

void run(const std::filesystem::path& root) {
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    if (!source) throw std::runtime_error("missing stock campaign");
    const auto referenced = cp::campaign_referenced_types(data, campaign);
    const auto types = eawr::units::galactic_type_table(loaded.catalog, false, referenced);
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
    const auto plan = take(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));

    // Every stock planet type with a galactic model authors a destroyed-state model (the asteroid field); a planet that is
    // itself the field authors none and keeps its model.
    const auto* victim = static_cast<const cp::CampaignLocation*>(nullptr);
    for (const auto& location : plan.locations()) {
        if (!location.planet) continue;
        const auto* planet = data.planet(location.original_type);
        if (!planet || planet->galactic_model_name.empty() || planet->destroyed_galactic_model_name.empty()) continue;
        victim = &location;
        break;
    }
    if (!victim) throw std::runtime_error("no stock planet authors a destroyed-state model");
    const auto id = victim->planet->id;

    auto first = take(cp::CampaignRoot::create(plan, services));
    expect(!destroyed(*first.game()->galaxy().snapshot(), id), "a stock planet starts intact");
    const auto before = first.game()->galaxy().native_effects().size();
    expect(take(first.game()->request_planet_ruins(id)), "the shared ruins request applies to a stock planet");
    expect(destroyed(*first.game()->galaxy().snapshot(), id), "the planet reads as destroyed");
    expect(!take(first.game()->request_planet_ruins(id)), "a destroyed planet refuses a second request");
    // The request is one journaled native effect, the record a checkpoint restores by replay.
    expect(first.game()->galaxy().native_effects().size() == before + 1, "the ruins are one journaled native effect");
    // A fresh root of the same plan is unaffected: the fact belongs to the one root.
    auto second = take(cp::CampaignRoot::create(plan, services));
    expect(!destroyed(*second.game()->galaxy().snapshot(), id), "a fresh root of the same plan starts intact");
    first.stop_root(); second.stop_root();
}
} // namespace

int main() {
    try {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("SKIPPED: the stock planet ruins test requires EAWR_EAW_GAME_ROOT\n", 0, std::cout);
        run(std::filesystem::path(*root));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}

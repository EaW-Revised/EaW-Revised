// Installed-data name-index checks: every link and find() of a loaded table against the linear-scan reference, and one
// report line (counts, notes digest, content identity) that a build without the index prints identically. Arms: the default
// unit load of stock FoC or of the Remake chain, and the GC Start shape (the shared load of a prepared campaign's types).

#include "../support/env.hpp"
#include "unit_tables_support.hpp"
#include "unit_tables_index_reference.hpp"

#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/shared_unit_tables.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
using namespace unit_tables_index_reference;
namespace {

struct Installed {
    std::vector<eawr::vfs::MountSpec> specs;
    eawr::data::Profile profile{eawr::data::Profile::foc};
};

// 0 = mounted; 77 = skip; 1 = failure.
int mount_installed(const bool mod, Installed& out) {
    const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) return 77;
    if (mod) {
        const auto mount = eawr::testing::remake_mount();
        if (!mount) { std::cout << "SKIP: Remake chain not configured\n"; return 77; }
        auto roots = eawr::vfs::mod_chain_roots(std::filesystem::path(mount->mod_root));
        for (const auto& [name, folder] : roots) {
            if (!std::filesystem::is_directory(folder)) { std::cout << "SKIP: Remake layer not installed: " << name << '\n'; return 77; }
        }
        roots.emplace_back("expansion", std::filesystem::path(*root) / "corruption" / "Data");
        roots.emplace_back("base", std::filesystem::path(*root) / "GameData" / "Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        if (!chain) { std::cerr << eawr::core::format_diagnostic(chain.error()) << '\n'; return 1; }
        for (auto& layer : chain.value()) out.specs.push_back(std::move(layer.mount));
        out.profile = eawr::data::Profile::remake;
        return 0;
    }
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        if (!manifest) { std::cerr << eawr::core::format_diagnostic(manifest.error()) << '\n'; return 1; }
        out.specs.push_back(std::move(manifest).value().mount);
    }
    return 0;
}

} // namespace

int unit_name_index_installed(const bool mod) {
    Installed installed;
    if (const int status = mount_installed(mod, installed)) return status;
    auto filesystem = eawr::vfs::Vfs::mount(installed.specs);
    if (!filesystem) { std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n'; return 1; }
    auto catalog = eawr::data::load_catalog(filesystem.value(), installed.profile);
    if (!catalog) { std::cerr << eawr::core::format_diagnostic(catalog.error()) << '\n'; return 1; }
    eawr::scene::VfsAssetCache cache(filesystem.value());
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = cache.access().model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) { std::cerr << eawr::core::format_diagnostic(tables.error()) << '\n'; return 1; }
    expect(tables.value().units.size() > 12, "the installed load produced more than the pinned fleet");
    report_tables(tables.value(), mod ? "remake chain" : "stock");
    return failures == 0 ? 0 : 1;
}

int unit_ability_lists_installed(const bool mod) {
    Installed installed;
    if (const int status = mount_installed(mod, installed)) return status;
    auto filesystem = eawr::vfs::Vfs::mount(installed.specs);
    if (!filesystem) { std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n'; return 1; }
    auto catalog = eawr::data::load_catalog(filesystem.value(), installed.profile);
    if (!catalog) { std::cerr << eawr::core::format_diagnostic(catalog.error()) << '\n'; return 1; }
    eawr::scene::VfsAssetCache cache(filesystem.value());
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = cache.access().model;
    input.types = {"Y_Wing_Squadron_Container"};
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) { std::cerr << eawr::core::format_diagnostic(tables.error()) << '\n'; return 1; }
    const auto* unit = tables.value().find("Y_Wing_Squadron_Container");
    expect(unit != nullptr, "WABL-01: installed team container loads");
    if (!unit) return 1;
    if (mod) {
        expect(unit->production.combat_bonuses.size() == 1
            && unit->production.combat_bonuses.front().name == "Black_Squadron_Vader_Combat_Bonus",
            "WABL-02: Remake childless marked container preserves its inherited combat bonus");
    } else {
        expect(unit->abilities.size() == 1 && unit->abilities.front().type == "ION_CANNON_SHOT",
            "WABL-01: stock list dispatch retains the authored ion-shot button");
        expect(unit->special_abilities.size() == 1
            && unit->special_abilities.front().name == "Y_Wing_Container_Ion_Cannon_Shot",
            "WABL-02: stock variant list replaces the base handler list");
    }
    std::cout << "ability-list installed contract: " << (mod ? "remake" : "stock")
              << " buttons " << unit->abilities.size() << " handlers " << unit->special_abilities.size()
              << " combat bonuses " << unit->production.combat_bonuses.size() << '\n';
    return failures == 0 ? 0 : 1;
}

// GC Start shape: the unit-table load a prepared campaign shares between its preparation steps (the same types and LoadInput
// as the live game session), for the stock Sandbox campaign or the Remake chain.
int unit_name_index_gc_start(const bool mod) {
    namespace cp = eawr::campaign;
    namespace gc = eawr::sim::galactic;
    Installed installed;
    if (const int status = mount_installed(mod, installed)) return status;
    auto filesystem = eawr::vfs::Vfs::mount(installed.specs);
    if (!filesystem) { std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n'; return 1; }
    auto loaded = eawr::data::load_catalog(filesystem.value(), installed.profile);
    if (!loaded) { std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n'; return 1; }
    auto data = eawr::data::galactic::load_galactic_data(filesystem.value(), loaded.value().catalog);
    if (!data) { std::cerr << eawr::core::format_diagnostic(data.error()) << '\n'; return 1; }
    const auto types = eawr::units::galactic_type_table(loaded.value().catalog);
    cp::CampaignSetupOptions options;
    options.seed = 42;
    for (const auto& faction : data.value().factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Rebel";
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false))
            options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    auto planned = cp::plan_live_campaign_setup(filesystem.value(), data.value(), types, "Sandbox_Equal_Footing_Empire",
        options, "Normal_Default");
    if (!planned) {
        std::cout << "SKIP: the Sandbox campaign does not plan on these layers: " << planned.error().message << '\n';
        return 77;
    }
    auto shared = cp::prepare_shared_unit_tables(filesystem.value(), loaded.value().catalog, planned.value());
    if (!shared) { std::cerr << eawr::core::format_diagnostic(shared.error()) << '\n'; return 1; }
    expect(shared.value()->tables().units.size() > 12, "the shared GC Start load holds the campaign's types");
    report_tables(shared.value()->tables(), mod ? "gc start remake chain" : "gc start stock");
    return failures == 0 ? 0 : 1;
}

} // namespace unit_tables_test_support

// GC shell admission checks without a renderer or GPU.
#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "ui_test_support.hpp"

#include <map>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

void atlas_names() {
    assets::MegaTexture atlas;
    atlas.entries.push_back({"I_BUTTON_BLANK.TGA", {1, 2, 3, 4}, true, false, false});
    expect(atlas.find("i_button_blank.dds") == nullptr
        && atlas.find_sibling("i_button_blank.dds") == &atlas.entries.front(),
           "shared atlas lookup resolves DDS requests to TGA entries for every consumer");
    const auto sibling = ui::resolve_ui_texture("i_button_blank.dds", &atlas, {});
    expect(sibling.origin == ui::TextureOrigin::atlas && sibling.rectangle.x == 1,
           "Texture sibling tolerance: requested DDS resolves stored uppercase TGA");
    const auto loose = [](std::string_view) { return std::optional<std::string>{"loose.dds"}; };
    expect(ui::resolve_ui_texture("i_button_blank.dds", &atlas, loose).origin == ui::TextureOrigin::standalone,
           "loose exact replacement wins over atlas sibling");
    atlas.entries.push_back({"I_BUTTON_BLANK.DDS", {5, 6, 7, 8}, false, false, false});
    expect(atlas.find("i_button_blank.dds") == &atlas.entries.back(),
           "shared atlas lookup preserves exact extension priority");
    const auto exact = ui::resolve_ui_texture("i_button_blank.dds", &atlas, {});
    expect(exact.origin == ui::TextureOrigin::atlas && exact.rectangle.x == 5 && !exact.has_alpha,
           "exact atlas name keeps priority over sibling extension");
    expect(ui::resolve_ui_texture("i_button_blank.dds", &atlas, loose).origin == ui::TextureOrigin::atlas,
           "exact atlas still wins over loose texture for normal UI");
    expect(ui::resolve_ui_texture("i_button_blank.dds", &atlas, loose, true).origin == ui::TextureOrigin::standalone,
           "legacy shell meshes preserve standalone-first source even with an exact atlas entry");
    expect(ui::resolve_ui_texture("absent.dds", &atlas, {}).origin == ui::TextureOrigin::missing,
           "unknown stem stays missing");
    expect(ui::resolve_ui_texture("i_button_blank", &atlas, {}).rectangle.x == 1,
           "extensionless atlas lookup follows loose-file TGA then DDS order");
}

void corpus(const std::string& name, const vfs::Vfs& files) {
    auto objects = data::load_catalog(files, data::Profile::foc);
    auto commands = data::ui::load_command_bar(files);
    expect(bool(objects) && bool(commands), "GC shell corpus catalogs load");
    if (!objects || !commands) return;
    const auto& catalog = commands.value().catalog;
    auto atlas = assets::load_mega_texture_atlas(files, ui::command_bar_mega_texture(catalog));
    expect(bool(atlas), "GC command atlas directory and backing pixels decode");
    if (!atlas) return;
    // Geometry and decoded textures are shared across campaigns. Cache them, but
    // retain a result for every campaign/faction/shell combination in the report.
    std::map<std::string, bool> textures;
    std::map<ui::HudFaction, bool> looks;
    std::vector<std::string> origin_log;
    std::size_t loose_checks{}, moved_to_atlas{};
    std::vector<std::string_view> components{"i_main_commandbar", "map_shell"};
    if (name == "FoC") components.insert(components.end(), {"i_main_organize", "target_type_back", "hack_shell",
        "market_frame", "corruption_shell", "pending_battle_shell", "story_pending_battle_shell"});
    for (const auto faction : {ui::HudFaction::rebel, ui::HudFaction::empire, ui::HudFaction::underworld}) {
        bool valid = true;
        for (const auto component_name : components) {
            const auto* component = catalog.find(component_name);
            if (!component && component_name != "i_main_commandbar" && component_name != "map_shell") continue;
            expect(component != nullptr, "GC shell component exists");
            if (!component) { valid = false; continue; }
            auto anchors = data::ui::load_shell_anchors(files, component->text(data::ui::Field::model_name));
            expect(bool(anchors), "GC shell authored model loads headlessly");
            if (!anchors) { valid = false; continue; }
            const auto look = ui::hud_shell(anchors.value().shell, catalog, faction);
            if (component_name == "i_main_commandbar" || component_name == "map_shell") {
                expect(!look.meshes.empty(), "GC shell has drawable meshes to check");
                valid = !look.meshes.empty() && valid;
            }
            for (const auto& mesh : look.meshes) {
                auto [cached, inserted] = textures.emplace(std::string(component_name) + ":" + mesh.texture, false);
                if (inserted) {
                    const bool standalone_first = component_name != "map_shell";
                    const auto standalone = ui::vfs_standalone_textures(files, false);
                    const auto old_path = standalone(mesh.texture);
                    const auto slot = ui::resolve_ui_texture(mesh.texture, &atlas.value().directory,
                                                            standalone, standalone_first);
                    if (standalone_first && old_path) {
                        ++loose_checks;
                        moved_to_atlas += slot.origin != ui::TextureOrigin::standalone;
                        expect(slot.origin == ui::TextureOrigin::standalone && slot.logical_path == *old_path,
                               "legacy GC shell loose image source is unchanged");
                    }
                    const auto& region = slot.rectangle;
                    const bool atlas_bounds = region.x <= atlas.value().page.width
                        && region.width <= atlas.value().page.width - region.x
                        && region.y <= atlas.value().page.height
                        && region.height <= atlas.value().page.height - region.y;
                    cached->second = (slot.origin == ui::TextureOrigin::atlas && atlas_bounds)
                        || (slot.origin == ui::TextureOrigin::standalone
                            && bool(assets::load_texture(files, slot.logical_path)));
                    origin_log.push_back(std::string(component_name) + " " + mesh.texture + " origin="
                        + std::string(ui::to_string(slot.origin)) + " resolved=" + (cached->second ? "1" : "0"));
                }
                valid = cached->second && valid;
            }
        }
        looks.emplace(faction, valid);
    }
    std::cout << name << " legacy loose checks=" << loose_checks << " moved_to_atlas=" << moved_to_atlas << '\n';
    for (const auto& line : origin_log) std::cout << name << ' ' << line << '\n';
    std::size_t campaigns{};
    for (const auto& definition : objects.value().catalog.definitions()) {
        if (definition.category != data::Category::campaign || !definition.namespace_winner) continue;
        ++campaigns;
        for (const auto& look : looks) {
            expect(look.second, "every campaign's command-bar and map-shell meshes resolve and decode");
        }
    }
    expect(campaigns > 0, "GC shell corpus checks at least one campaign");
    std::cout << name << ": " << campaigns << " campaigns, " << textures.size() << " unique shell textures\n";
}
}

int main(int argc, char** argv) {
    atlas_names();
    if (argc == 3 && std::string_view(argv[1]) == "--mod-id") {
        const auto remake = test::ui::environment("EAWR_REMAKE_MOD_ROOT");
        const auto game = test::ui::environment("EAWR_EAW_GAME_ROOT");
        if (!remake || !game) return 77;
        const auto mod = std::filesystem::path(*remake).parent_path() / argv[2];
        expect(std::filesystem::is_directory(mod), "requested test mod is installed");
        if (!std::filesystem::is_directory(mod)) return 1;
        auto roots = vfs::mod_chain_roots(mod);
        roots.emplace_back("expansion", std::filesystem::path(*game) / "corruption" / "Data");
        roots.emplace_back("base", std::filesystem::path(*game) / "GameData" / "Data");
        auto chain = vfs::resolve_manifest_chain(roots);
        expect(bool(chain), "GC test mod manifests resolve");
        if (!chain) return 1;
        std::vector<vfs::MountSpec> mounts;
        for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
        auto files = vfs::Vfs::mount(mounts);
        expect(bool(files), "GC test mod mounts");
        if (files) corpus(argv[2], files.value());
    } else {
        if (auto files = test::ui::foc_corpus("GC shell textures")) corpus("FoC", *files);
        for (const auto& mod : test::ui::mod_corpora("GC shell textures")) corpus(mod.name, mod.filesystem);
    }
    return test::ui::failures() ? 1 : 0;
}

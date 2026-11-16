// #6771 row 4 (T10): the galaxy view's texture names (planet and family textures, the route band texture, the corruption
// particle textures) go through the shared resolver (docs/behaviour/asset-name-resolution.md, ANR-01). Over a synthetic
// mounted tree: a name authored with an extension other than the shipped one loads; a name that already matches loads
// unchanged; a miss is empty and logs one line per name. Pure CPU; no game data.
#include "galactic/galactic_texture_name.hpp"

#include "eawr/vfs/vfs.hpp"

#include "../ui/ui_test_support.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace gal = eawr::viewer::galactic;
namespace assets = eawr::assets;

std::vector<std::string> emitted;
void capture(const std::string_view line) { emitted.emplace_back(line); }

std::string backslashed(std::string name) { // an authored name with Windows separators
    for (char& c : name) if (c == '/') c = static_cast<char>(92);
    return name;
}
} // namespace

int main() {
    eawr::test::ui::TempTree tree("galactic-texture-name");
    eawr::test::ui::write_text(tree.root / "art/textures/w_planet_barren.dds", "x");
    eawr::test::ui::write_text(tree.root / "art/textures/w_route_band.dds", "x");
    eawr::test::ui::write_text(tree.root / "art/textures/w_glow.tga", "x");
    eawr::test::ui::write_text(tree.root / "art/textures/p_corruption_cloud.dds", "x");
    const std::array mounts{eawr::vfs::MountSpec{"mod", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "the fixture VFS mounts");
    if (!mounted) return 1;
    const auto& vfs = mounted.value();

    assets::reset_name_misses();
    emitted.clear();
    assets::set_name_miss_emitter(&capture);

    // Stock-shaped names are unchanged.
    expect(gal::scene_texture_path(vfs, "W_Planet_Barren.DDS") == std::optional<std::string>("data/art/textures/w_planet_barren.dds"),
           "a name that matches its file loads unchanged (case folded)");
    expect(gal::scene_texture_path(vfs, "W_Glow.tga") == std::optional<std::string>("data/art/textures/w_glow.tga"),
           "an authored .tga with a .tga file loads unchanged");
    // The wrong-extension class: authored .jpg or .tga, only the .dds ships.
    expect(gal::scene_texture_path(vfs, "W_Planet_Barren.jpg") == std::optional<std::string>("data/art/textures/w_planet_barren.dds"),
           "a planet texture authored .jpg loads from the .dds");
    expect(gal::scene_texture_path(vfs, "w_route_band.tga") == std::optional<std::string>("data/art/textures/w_route_band.dds"),
           "a route band texture authored .tga loads from the .dds");
    expect(gal::scene_texture_path(vfs, "P_Corruption_Cloud.png") == std::optional<std::string>("data/art/textures/p_corruption_cloud.dds"),
           "a corruption particle texture authored .png loads from the .dds");
    expect(gal::scene_texture_path(vfs, "w_glow.dds") == std::optional<std::string>("data/art/textures/w_glow.tga"),
           "a .dds name loads the sibling .tga (the tolerant deviation DEV-A)");
    // A name that already carries its directory is not rooted twice.
    expect(gal::scene_texture_path(vfs, backslashed("Data/Art/Textures/W_Route_Band.JPG")) == std::optional<std::string>("data/art/textures/w_route_band.dds"),
           "a data/-prefixed name is not rooted twice");
    expect(emitted.empty(), "hits log nothing");

    // Misses: empty, one line per name, labelled with the site.
    expect(!gal::scene_texture_path(vfs, "w_missing.jpg") && emitted.size() == 1
               && emitted.back() == "EAWR-ASSET-NAME-0001 texture 'w_missing.jpg' not found (site galactic_scene)",
           "a miss is empty and logs one line with the site");
    static_cast<void>(gal::scene_texture_path(vfs, "W_Missing.JPG"));
    expect(emitted.size() == 1, "the same name (folded) logs only once");
    expect(!gal::scene_texture_path(vfs, "") && emitted.size() == 1, "a blank name is empty and not a miss");

    assets::set_name_miss_emitter(nullptr);
    assets::reset_name_misses();
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}

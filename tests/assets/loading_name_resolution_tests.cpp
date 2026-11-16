// #6771 row 6: the loading screen's pictures and the loading planet's textures resolve their authored names through the
// shared resolver (docs/behaviour/asset-name-resolution.md, ANR-01, cases ANR-C1 to ANR-C3 and ANR-C10). A synthetic
// tree pins the wrong-extension case for each site; with EAWR_EAW_GAME_ROOT set, the stock loading pictures and planet
// textures must resolve to the files the sites used before the migration.
#include "loading_texture_names.hpp"

#include "../ui/ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace assets = eawr::assets;
namespace vfs = eawr::vfs;
namespace site = eawr::presentation::godot_backend;

using Resolve = std::string (*)(const vfs::Vfs&, std::string_view);

std::string lowered_path(std::string_view name) { return assets::normalized_asset_name(name); }

bool exists(const vfs::Vfs& filesystem, const std::string& logical) { return static_cast<bool>(filesystem.stat(logical)); }

// What loading_screen.cpp `load_named` tried before the migration: a rooted or slashed name as given, then the name
// without its last extension under the texture directory with .tga, then .dds. Returns the first file that exists.
std::string legacy_picture(const vfs::Vfs& filesystem, const std::string& name) {
    const auto dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::vector<std::string> candidates;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) candidates.push_back(name);
    for (const char* extension : {".tga", ".dds"}) candidates.push_back("data/art/textures/" + stem + extension);
    for (const std::string& candidate : candidates)
        if (exists(filesystem, lowered_path(candidate))) return lowered_path(candidate);
    return {};
}

// What loading_planet.cpp `texture` tried before: the name as given under the texture directory, then its stem with
// .dds, then .tga (the last-dot cut; an extensionless name cuts at npos and keeps the whole name).
std::string legacy_planet(const vfs::Vfs& filesystem, const std::string& name) {
    const std::string normalized = lowered_path(name);
    const std::string logical = normalized.starts_with("data/") ? normalized : "data/art/textures/" + normalized;
    const std::string stem = logical.substr(0, logical.find_last_of('.'));
    for (const std::string& candidate : {logical, stem + ".dds", stem + ".tga"})
        if (exists(filesystem, candidate)) return candidate;
    return {};
}

std::vector<std::string> emitted;
void capture(const std::string_view line) { emitted.emplace_back(line); }

struct Fixture final {
    eawr::test::ui::TempTree tree{"loading-names"};
    std::optional<vfs::Vfs> filesystem;
    Fixture() {
        // Only the .dds files of the install: the XML still writes the .tga names (and a .jpg in some mods).
        for (const char* file : {"load_weather_sun.dds", "_mp_space_hoth.dds", "w_planet_earth.dds", "only_tga.tga",
                                 "both.tga", "both.dds", "sky.dds"})
            eawr::test::ui::write_text(tree.root / "art/textures" / file, "x");
        const std::array mounts{vfs::MountSpec{"mod", tree.root, "data", {}}};
        auto mounted = vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(mounted), "the loading-name fixture VFS mounts");
        if (mounted) filesystem.emplace(std::move(mounted.value()));
    }
};

void wrong_extension(const std::string_view label, const Resolve resolve, const vfs::Vfs& filesystem) {
    const std::string who(label);
    expect(resolve(filesystem, "load_weather_sun.tga") == "data/art/textures/load_weather_sun.dds",
           (who + ": a .tga name finds the shipped .dds (ANR-C2)").c_str());
    expect(resolve(filesystem, "Sky.JPG") == "data/art/textures/sky.dds",
           (who + ": a .jpg name finds the shipped .dds (ANR-C1), case folded").c_str());
    expect(resolve(filesystem, "load_weather_sun.dds") == "data/art/textures/load_weather_sun.dds",
           (who + ": a .dds name finds its file").c_str());
    expect(resolve(filesystem, "w_planet_earth") == "data/art/textures/w_planet_earth.dds",
           (who + ": an extensionless name finds the .dds").c_str());
    expect(resolve(filesystem, "only_tga.dds") == "data/art/textures/only_tga.tga",
           (who + ": a .dds name finds a shipped .tga through the sibling tolerance (ANR-C3)").c_str());
    expect(resolve(filesystem, "both.tga") == "data/art/textures/both.tga",
           (who + ": the authored name wins when both files ship").c_str());
    expect(resolve(filesystem, "Data\\Art\\Textures\\_MP_Space_Hoth.dds") == "data/art/textures/_mp_space_hoth.dds",
           (who + ": a rooted path is not rooted twice").c_str());
}

void misses(const vfs::Vfs& filesystem) {
    assets::reset_name_misses();
    emitted.clear();
    assets::set_name_miss_emitter(&capture);
    expect(site::loading_picture_path(filesystem, "load_weather_sun.tga") == "data/art/textures/load_weather_sun.dds"
               && site::loading_planet_texture_path(filesystem, "sky.jpg") == "data/art/textures/sky.dds" && emitted.empty(),
           "a hit logs nothing");
    expect(site::loading_picture_path(filesystem, "Gone.TGA").empty() && emitted.size() == 1
               && emitted.front()
                      == "EAWR-ASSET-NAME-0001 texture 'Gone.TGA' not found (site loading screen picture)",
           "ANR-C10: a missing loading picture logs one resolver line with the site");
    expect(site::loading_picture_path(filesystem, "gone.tga").empty() && emitted.size() == 1,
           "the same name is not logged twice");
    expect(site::loading_planet_texture_path(filesystem, "Missing_Planet.tga").empty() && emitted.size() == 2
               && emitted.back()
                      == "EAWR-ASSET-NAME-0001 texture 'Missing_Planet.tga' not found (site loading planet texture)",
           "ANR-C10: a missing planet texture logs one resolver line with the site");
    expect(site::loading_picture_path(filesystem, "").empty() && emitted.size() == 2, "an empty name is not a miss");
    assets::set_name_miss_emitter(nullptr);
    assets::reset_name_misses();
}

void legacy_agrees(const vfs::Vfs& filesystem) {
    // Names the stock loading screen shows: weather/native icons by their authored .tga names, the map preview rooted.
    for (const char* name : {"load_weather_sun.tga", "load_weather_sun.dds", "only_tga.tga", "both.tga"})
        expect(site::loading_picture_path(filesystem, name) == legacy_picture(filesystem, name),
               (std::string("picture ") + name + " resolves as before").c_str());
    expect(site::loading_picture_path(filesystem, "data/art/textures/_mp_space_hoth.dds")
               == legacy_picture(filesystem, "data/art/textures/_mp_space_hoth.dds"),
           "the rooted map preview resolves as before");
    for (const char* name : {"w_planet_earth.tga", "w_planet_earth.dds", "only_tga.tga"})
        expect(site::loading_planet_texture_path(filesystem, name) == legacy_planet(filesystem, name),
               (std::string("planet texture ") + name + " resolves as before").c_str());
    // The authored name is tried first (ANR-01): a .dds name with both files shipped takes the .dds where the picture took .tga.
    expect(legacy_picture(filesystem, "both.dds") == "data/art/textures/both.tga"
               && site::loading_picture_path(filesystem, "both.dds") == "data/art/textures/both.dds",
           "a .dds picture name with both files shipped takes its own file (authored name first, was .tga)");
    // The one named reorder (ANR-C4): with both files shipped the planet takes .tga where it took .dds.
    expect(legacy_planet(filesystem, "both.tga") == "data/art/textures/both.tga"
               && legacy_planet(filesystem, "both") == "data/art/textures/both.dds"
               && site::loading_planet_texture_path(filesystem, "both") == "data/art/textures/both.tga",
           "an extensionless planet name with both files shipped takes .tga (ANR-C4, was .dds)");
}

void stock() {
    auto corpus = eawr::test::ui::foc_corpus("Loading name resolution");
    if (!corpus) return;
    const vfs::Vfs& filesystem = *corpus;
    auto listed = filesystem.enumerate("data/art/textures/");
    expect(static_cast<bool>(listed), "the stock texture listing reads");
    if (!listed) return;
    // Every stock texture file, by the names the XML and the model data write: the stem with .tga, with .dds, and bare.
    std::set<std::string> stems;
    for (const auto& record : listed.value()) {
        const std::string& path = record.canonical_path;
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) continue;
        const std::string extension = path.substr(dot);
        if (extension != ".tga" && extension != ".dds") continue;
        stems.insert(path.substr(std::string_view("data/art/textures/").size(), dot - std::string_view("data/art/textures/").size()));
    }
    expect(!stems.empty(), "the stock tree has textures");
    std::size_t pictures = 0, planets = 0, both_files = 0;
    std::vector<std::string> changed_planets;
    for (const std::string& stem : stems) {
        const bool both = exists(filesystem, "data/art/textures/" + stem + ".tga") && exists(filesystem, "data/art/textures/" + stem + ".dds");
        if (both) ++both_files;
        // Loading pictures: the load_* art, the map previews (<map>.dds, rooted by the screen model) and the overlay.
        if (stem.starts_with("load_") || stem.starts_with("_mp_") || stem.starts_with("_sp_")) {
            ++pictures;
            std::vector<std::string> names{stem + ".tga"};
            if (exists(filesystem, "data/art/textures/" + stem + ".dds")) names.push_back("data/art/textures/" + stem + ".dds");
            for (const std::string& name : names) {
                const std::string now = site::loading_picture_path(filesystem, name);
                expect(!now.empty() && now == legacy_picture(filesystem, name),
                       ("stock loading picture " + name + " resolves as before").c_str());
            }
        }
        // Planet textures: every stock stem as the model data may write it. Where only one file ships the result must
        // equal the old one; with both shipped the named reorder applies (the old order took .dds first).
        std::vector<std::string> spellings{stem + ".tga", stem + ".dds"};
        // A bare name that holds a dot is cut at its last dot before and at its first dot now (no stock name is authored so).
        if (stem.find('.') == std::string::npos) spellings.push_back(stem);
        for (const std::string& name : spellings) {
            const std::string now = site::loading_planet_texture_path(filesystem, name);
            const std::string before = legacy_planet(filesystem, name);
            ++planets;
            if (now == before) continue;
            if (both && now == "data/art/textures/" + stem + ".tga") changed_planets.push_back(name);
            else expect(false, ("stock planet texture " + name + " resolves as before").c_str());
        }
    }
    // The reorder may exist in stock data, but not for a texture the loading planet draws (the planet art).
    for (const std::string& name : changed_planets)
        expect(!name.starts_with("w_planet") && !name.starts_with("planet"),
               ("stock planet art " + name + " keeps its resolved file").c_str());
    expect(pictures > 0, "stock has loading pictures to check");
    std::cout << "Loading name resolution corpus: " << pictures << " loading picture stems, " << planets
              << " planet names checked, " << both_files << " stems ship both .tga and .dds, " << changed_planets.size()
              << " planet names take the ANR-C4 order\n";
}
} // namespace

int main() {
    {
        Fixture fixture;
        if (fixture.filesystem) {
            wrong_extension("loading picture", &site::loading_picture_path, *fixture.filesystem);
            wrong_extension("planet texture", &site::loading_planet_texture_path, *fixture.filesystem);
            misses(*fixture.filesystem);
            legacy_agrees(*fixture.filesystem);
        }
    }
    stock();
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}

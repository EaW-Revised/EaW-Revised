#include "../../apps/viewer/src/galactic/planet_search.hpp"
#include "../../apps/viewer/src/galactic/planet_search_layout.hpp"
#include "ui_test_support.hpp"
#include "../support/runtime_path.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

int main() {
    using namespace eawr;
    namespace search = viewer::galactic;
    using test::ui::expect;
    search::PlanetSearchIndex index;
    const auto alphabetical = [](const auto& a, const auto& b) { return a < b; };
    std::vector<search::PlanetSearchEntry> source{
        {4, "Beta", "beta", true}, {2, "Hidden system", "hidden system", false},
        {9, "Alpha", "alpha", true}, {6, "Alpha", "alpha", true},
        {11, "Planète", "planète", true}, {18, "Missing_Text_Internal", "missing_text_internal", true},
        {23, "   ", "   ", true}};
    expect(index.update(source, alphabetical), "new discovered/localized names build an index");
    expect(index.filter("") == std::vector<std::uint64_t>({9,6,4,18,11}), "empty query is alphabetical; equal names retain campaign order");
    expect(index.filter("ph") == std::vector<std::uint64_t>({9,6}), "incremental substring searches display text");
    expect(index.filter("hidden").empty(), "undiscovered exact or partial names never appear");
    expect(index.filter("   ").empty(), "a mod's deliberately blank-name dummy has no search row");
    expect(index.filter("planète") == std::vector<std::uint64_t>({11}), "localized UTF-8 keys survive the model intact");
    expect(index.filter("missing_text") == std::vector<std::uint64_t>({18}), "the caller's internal-name fallback is searchable");
    expect(!index.update(source, alphabetical), "unchanged discovery and language do not sort again");
    source[1].name = "Another hidden name";
    expect(!index.update(source, alphabetical), "hidden-name changes are not a search oracle or an index invalidation");
    source[2].discovered = false;
    expect(index.update(source, alphabetical) && index.filter("alpha") == std::vector<std::uint64_t>({6}), "observer changes remove old discoveries");
    source[3].name = "Autre nom"; source[3].folded = "autre nom";
    expect(index.update(source, alphabetical) && index.filter("alpha").empty(), "language/name changes replace the old search text");
    // A relocated test package (the cross route's Windows test stage) supplies the source root at run time (#7546).
    const auto directory = testing::runtime_path("EAWR_TEST_SOURCE_ROOT", "tools/revised_planet_search",
        EAWR_SOURCE_DIR "/tools/revised_planet_search");
    const std::array mounts{vfs::MountSpec{"project", directory, "data/xml", {}}};
    auto files = vfs::Vfs::mount(mounts);
    expect(bool(files), "the shipped project dialog mounts without retail assets");
    if (files) {
        bool invalid{};
        const auto layout = search::load_planet_search_layout(files.value(), invalid);
        expect(bool(layout) && !invalid, "the shipped dialog parses through the mounted project layer");
        if (layout) {
            expect(layout->dialog.controls.size() == 3, "dialog has only text, results and close; no unrequested extras");
            expect(layout->picture && layout->emblem, "both optional art columns are enabled by the layout");
            expect(layout->row_scale == 1.2F, "the shipped layout enlarges the result rows by Row_Scale 1.2");
        }
        auto document = data::load_document(files.value(), "Data/XML/RevisedPlanetSearch.xml");
        if (document) {
            auto unscaled = document.value();
            std::erase_if(unscaled.root.children, [](const auto& node) { return node.name == "Row_Scale"; });
            const auto plain = search::parse_planet_search_layout(unscaled);
            expect(plain && plain->row_scale == 1.0F, "an absent Row_Scale keeps the kit's List_Box size (1.0)");
            auto scaled = document.value();
            for (auto& child : scaled.root.children) if (child.name == "Row_Scale") child.raw_text = " 1.5 ";
            const auto larger = search::parse_planet_search_layout(scaled);
            expect(larger && larger->row_scale == 1.5F, "a mod restyle may set its own Row_Scale");
            for (auto& child : scaled.root.children) if (child.name == "Row_Scale") child.raw_text = "large";
            expect(!search::parse_planet_search_layout(scaled), "a non-numeric Row_Scale rejects the restyle whole");
            for (auto& child : scaled.root.children) if (child.name == "Row_Scale") child.raw_text = "0";
            expect(!search::parse_planet_search_layout(scaled), "a collapsing Row_Scale rejects the restyle whole");
            auto broken = document.value();
            std::erase_if(broken.root.children, [](const auto& node) { return node.name == "Control"; });
            expect(!search::parse_planet_search_layout(broken), "a restyle without required controls is rejected whole");
            auto restyled = document.value();
            for (auto& child : restyled.root.children) if (child.name == "Owner_Emblem") child.raw_text = "False";
            const auto without_emblem = search::parse_planet_search_layout(restyled);
            expect(without_emblem && !without_emblem->emblem, "a mod may remove the emblem column through XML");
            for (auto& child : restyled.root.children) if (child.name == "Size") child.raw_text = "NaN 280";
            expect(!search::parse_planet_search_layout(restyled), "non-numeric geometry cannot reach the viewer");
        }
    }
    const auto temporary = std::filesystem::temp_directory_path() /
        ("eawr-search-layout-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto lower = temporary / "lower", higher = temporary / "higher";
    std::filesystem::create_directories(lower / "XML");
    std::filesystem::create_directories(higher / "XML");
    std::filesystem::copy_file(directory / "RevisedPlanetSearch.xml", lower / "XML/RevisedPlanetSearch.xml");
    {
        std::ofstream broken(higher / "XML/RevisedPlanetSearch.xml");
        broken << "<Planet_Search><Size>240 280</Size><Offset>16 96</Offset></Planet_Search>";
    }
    const std::array layered_mounts{vfs::MountSpec{"mod", higher, "data", {}}, vfs::MountSpec{"project", lower, "data", {}}};
    auto layered = vfs::Vfs::mount(layered_mounts);
    expect(bool(layered), "a higher mod layout and lower project layer mount together");
    if (layered) {
        bool invalid{};
        const auto layout = search::load_planet_search_layout(layered.value(), invalid);
        expect(layout && invalid && layout->dialog.controls.size() == 3, "a broken mod restyle reports invalid but retains the valid lower dialog");
    }
    testing::remove_temp_directory(temporary);
    source.clear();
    for (std::uint64_t id = 1; id <= 240; ++id) source.push_back({id, "System " + std::to_string(id), "system " + std::to_string(id), true});
    expect(index.update(source, alphabetical) && index.filter("").size() == 240, "large mod-sized galaxies have no result cap");
    expect(index.filter("system 239") == std::vector<std::uint64_t>({239}), "a planet beyond the initial visible rows remains searchable");
    return test::ui::failures() ? 1 : 0;
}

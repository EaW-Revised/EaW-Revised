// ML-31/ML-38: the battle scoring manifest pins every `.lua` of the directories its scheduler searches
// (Library and Miscellaneous), so a scoring module may `require` a sibling of itself. Synthetic mounted
// data only; no game data.

#include "../support/check.hpp"
#include "../support/result.hpp"
#include "../support/self_test.hpp"
#include "../support/temp_dir.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using eawr::testing::take;

eawr::testing::Checker checker;
void expect(const bool condition, const std::string_view message) { checker.check(condition, message, std::cerr); }

struct Tree {
    fs::path root = fs::temp_directory_path()
        / ("eawr-battle-scoring-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { fs::create_directories(root); }
    ~Tree() { eawr::testing::remove_temp_directory(root); }
};
void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

constexpr std::string_view constants_xml =
    "<Game_Constants><Game_Scoring_Script_Name>TestScoring</Game_Scoring_Script_Name></Game_Constants>";
constexpr std::string_view helper_module = "function Helper_Value() return 7 end\n";

// The scoring module in the test's own words: it needs a sibling helper at load time and answers every
// control query with the helper's value.
std::string scoring_module(const std::string_view required) {
    return "require(\"" + std::string(required) + "\")\n"
        "function Base_Definitions() end\n"
        "function main() end\n"
        "function Game_Mode_Starting_Event(mode, map) end\n"
        "function Get_Game_Stat_For_Control_ID(player, control, for_tactical) return Helper_Value() end\n";
}

// `files` maps a path below the mounted `data` root to its text; the constants come with every tree.
eawr::core::Result<std::unique_ptr<eawr::platform::BattleScoring>> create_from(
    const std::map<std::string, std::string>& files) {
    Tree tree;
    for (const auto& [path, text] : files) write(tree.root / path, text);
    write(tree.root / "XML" / "GameConstants.xml", constants_xml);
    write(tree.root / "Scripts" / "Library" / "placeholder.lua", "-- the library directory exists in every installation\n");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto vfs = take(eawr::vfs::Vfs::mount(mounts));
    const eawr::data::Catalog catalog{};
    const eawr::units::UnitTables tables{};
    return eawr::platform::BattleScoring::create(vfs, catalog, tables, {{1, "Empire"}}, "Test_Map");
}

void sibling_in_miscellaneous() {
    auto scoring = create_from({{"Scripts/Miscellaneous/TestScoring.lua", scoring_module("Helper")},
        {"Scripts/Miscellaneous/Helper.lua", std::string(helper_module)}});
    expect(static_cast<bool>(scoring), "a scoring module requires a sibling Helper.lua from data/scripts/miscellaneous/");
    if (!scoring) { std::cerr << scoring.error().message << '\n'; return; }
    const auto value = scoring.value()->query(1, "IDC_ANY");
    expect(value && value.value() == "7", "the sibling's function answers the scoring query");
}

void sibling_in_library() {
    auto scoring = create_from({{"Scripts/Miscellaneous/TestScoring.lua", scoring_module("Helper")},
        {"Scripts/Library/Helper.lua", std::string(helper_module)}});
    expect(static_cast<bool>(scoring), "a Library helper still resolves");
}

void outside_every_configured_directory() {
    // Negative control: the same helper in a directory the scheduler does not search stays refused.
    for (const auto* directory : {"Scripts/Story/Helper.lua", "Scripts/GameObject/Helper.lua", "Scripts/AI/Helper.lua"}) {
        auto scoring = create_from({{"Scripts/Miscellaneous/TestScoring.lua", scoring_module("Helper")},
            {directory, std::string(helper_module)}});
        expect(!scoring && (scoring.error().code + scoring.error().message).find("EAWR-SCRIPT-0207") != std::string::npos,
            std::string("a helper under ") + directory + " is not admitted (EAWR-SCRIPT-0207)");
    }
}

void mixed_case_names() {
    // The VFS is case-insensitive and the manifest keys are normalised: the file name's case need not match the
    // request, as with a mod's GalacticConquestEndings in a differently cased file.
    auto scoring = create_from({{"Scripts/Miscellaneous/TestScoring.lua", scoring_module("GalacticConquestEndings")},
        {"Scripts/MISCELLANEOUS/galacticCONQUESTendings.lua", std::string(helper_module)}});
    expect(static_cast<bool>(scoring), "a mixed-case module name resolves from a differently cased file");
    if (!scoring) std::cerr << scoring.error().message << '\n';
}

void absent_module_refuses() {
    auto scoring = create_from({{"Scripts/Miscellaneous/Helper.lua", std::string(helper_module)}});
    expect(!scoring, "a named scoring module that is not authored still refuses");
}

} // namespace

int main() {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    try {
        sibling_in_miscellaneous();
        sibling_in_library();
        outside_every_configured_directory();
        mixed_case_names();
        absent_module_refuses();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return checker.finish();
}

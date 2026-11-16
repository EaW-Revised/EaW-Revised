// ML-31/ML-33/ML-38/L-03a: the contrast scripts (the auto-resolve and galactic AI contrast load) run over a module
// map read from the mounted view, so a script of the library may `require` any other module of the library: a
// subfolder module (`sub/Name`) and a differently cased name included. Synthetic mounted data only; no game data.

#include "../../support/check.hpp"
#include "../../support/result.hpp"
#include "../../support/self_test.hpp"
#include "../../support/temp_dir.hpp"

#include "host.hpp"
#include "tactical_ai_internal.hpp"

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace foc = eawr::script::foc;
namespace auth = eawr::script::authoritative;
using eawr::testing::take;

eawr::testing::Checker checker;
void expect(const bool condition, const std::string_view message) { checker.check(condition, message, std::cerr); }

struct Tree {
    fs::path root = fs::temp_directory_path()
        / ("eawr-contrast-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { fs::create_directories(root); }
    ~Tree() { eawr::testing::remove_temp_directory(root); }
};
void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

// The base script in the test's own words: it requires the modules named by `requires`, then declares one contrast
// row whose weight a required module supplies. Nothing in it is a stock module name.
std::string base_script(const std::initializer_list<std::string_view> requires_) {
    std::string text;
    for (const auto name : requires_) text += "require(\"" + std::string(name) + "\")\n";
    text += "function Base_Definitions()\n"
        "  local list = WeightedTypeList.Create()\n"
        "  list.Parse({\"Fighter\"}, {Contrast_Weight})\n"
        "  EnemyContrastTypes = {\"Fighter\"}\n"
        "  FriendlyContrastTypes = {list}\n"
        "end\n";
    return text;
}
constexpr std::string_view weight_module = "Contrast_Weight = 3\n";

struct Outcome {
    eawr::core::Result<void> loaded = eawr::core::Result<void>::success();
    std::map<std::string, std::string> modules;
    std::size_t rows{};
    bool weight_is_three{};
};

// `files` maps a path below the mounted `data` root to its text. The loader's own PGAICommands is among them.
Outcome load(const std::map<std::string, std::string>& files) {
    Tree tree;
    for (const auto& [path, text] : files) write(tree.root / path, text);
    write(tree.root / "Scripts" / "Library" / "placeholder.lua", "-- the library directory exists in every installation\n");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    const auto vfs = take(eawr::vfs::Vfs::mount(mounts));
    Outcome outcome;
    auto modules = foc::tactical_ai_detail::read_contrast_modules(vfs);
    if (!modules) { outcome.loaded = eawr::core::Result<void>::failure(modules.error()); return outcome; }
    outcome.modules = std::move(modules).value();
    foc::detail::Host host;
    host.setup.content.categories.emplace("FIGHTER", 1);
    outcome.loaded = foc::tactical_ai_detail::load_contrast(host, outcome.modules);
    const auto row = host.contrast.find(1);
    if (row != host.contrast.end()) {
        outcome.rows = row->second.size();
        outcome.weight_is_three = outcome.rows == 1 && row->second.front().weight == eawr::script::numeric::LuaNumber(3);
    }
    return outcome;
}

bool refused_as_not_in_manifest(const Outcome& outcome) {
    return !outcome.loaded
        && (outcome.loaded.error().code + outcome.loaded.error().message).find("EAWR-SCRIPT-0207") != std::string::npos;
}

void library_subfolder_module() {
    const auto outcome = load({{"Scripts/Library/PGAICommands.lua", base_script({"sub/Weight"})},
        {"Scripts/Library/sub/Weight.lua", std::string(weight_module)}});
    expect(static_cast<bool>(outcome.loaded), "a library script requires sub/Weight from a subfolder of the library");
    if (!outcome.loaded) std::cerr << outcome.loaded.error().message << '\n';
    expect(outcome.weight_is_three, "the subfolder module's value reaches the contrast row");
}

void mixed_case_name() {
    // The VFS is case-insensitive and the manifest keys are normalised: the request's case need not match the file's.
    const auto outcome = load({{"Scripts/Library/PGAICommands.lua", base_script({"libContrastWeight"})},
        {"Scripts/LIBRARY/LIBcontrastWEIGHT.lua", std::string(weight_module)}});
    expect(static_cast<bool>(outcome.loaded), "a mixed-case module name resolves from a differently cased file");
    if (!outcome.loaded) std::cerr << outcome.loaded.error().message << '\n';
    expect(outcome.weight_is_three, "the mixed-case module's value reaches the contrast row");
}

void subfolder_with_mixed_case_and_chain() {
    // A module required by a module: the closure is read from the mounted view, not named up front.
    const auto outcome = load({{"Scripts/Library/PGAICommands.lua", base_script({"Eawx-Std/Chain"})},
        {"Scripts/Library/eawx-std/chain.lua", "require(\"Second\")\n"},
        {"Scripts/Library/Second.lua", std::string(weight_module)}});
    expect(static_cast<bool>(outcome.loaded), "a module of a subfolder requires a flat library module");
    expect(outcome.weight_is_three, "the second module's value reaches the contrast row");
}

void outside_the_library_stays_refused() {
    // Negative control. The request is the module's full logical path, which the scheduler's "./?.lua" candidate
    // resolves for any module that is in the manifest: so a refusal here proves the module was not admitted, not
    // that the request was unresolvable. The same request resolves for a module of the Library (positive control).
    const auto request = [](const std::string_view directory) {
        return base_script({"Data/Scripts/" + std::string(directory) + "/Weight"});
    };
    const auto control = load({{"Scripts/Library/PGAICommands.lua", request("Library")},
        {"Scripts/Library/Weight.lua", std::string(weight_module)}});
    expect(static_cast<bool>(control.loaded) && control.weight_is_three,
        "positive control: the full-path request resolves a module of the library");
    for (const auto* directory : {"Miscellaneous", "Story", "GameObject"}) {
        const auto outcome = load({{"Scripts/Library/PGAICommands.lua", request(directory)},
            {std::string("Scripts/") + directory + "/Weight.lua", std::string(weight_module)}});
        expect(refused_as_not_in_manifest(outcome), std::string("a module under ") + directory + " is refused (EAWR-SCRIPT-0207)");
        expect(outcome.modules.find("data/scripts/library/placeholder.lua") != outcome.modules.end()
            && outcome.modules.size() == 2, "only the library's modules are read");
    }
}

void absent_module_refuses() {
    const auto outcome = load({{"Scripts/Library/PGAICommands.lua", base_script({"NoSuchModule"})}});
    expect(refused_as_not_in_manifest(outcome), "a module the mounted view does not hold is refused (EAWR-SCRIPT-0207)");
}

void module_map_is_deterministic() {
    // Canonical logical keys, and a manifest digest that does not depend on the order modules were added in.
    Tree tree;
    for (const auto* name : {"b.lua", "A.lua", "sub/c.lua", "Sub/D.lua"})
        write(tree.root / "Scripts" / "Library" / name, std::string("x = '") + name + "'\n");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    const auto vfs = take(eawr::vfs::Vfs::mount(mounts));
    const auto first = take(foc::tactical_ai_detail::read_contrast_modules(vfs));
    const auto second = take(foc::tactical_ai_detail::read_contrast_modules(vfs));
    expect(first == second && first.size() == 4, "reading the library twice gives the same four modules");
    std::vector<std::string> keys;
    for (const auto& entry : first) keys.push_back(entry.first);
    expect(keys == std::vector<std::string>{"data/scripts/library/a.lua", "data/scripts/library/b.lua",
               "data/scripts/library/sub/c.lua", "data/scripts/library/sub/d.lua"},
        "the keys are the canonical lower-case logical paths, differently cased spellings of a folder merged");
    auth::ModuleManifest forward, backward;
    for (auto it = first.begin(); it != first.end(); ++it) take(forward.add(it->first, it->second));
    for (auto it = first.rbegin(); it != first.rend(); ++it) take(backward.add(it->first, it->second));
    expect(forward.digest() == backward.digest(), "the manifest digest is stable for identical input");
}

} // namespace

int main() {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    try {
        library_subfolder_module();
        mixed_case_name();
        subfolder_with_mixed_case_and_chain();
        outside_the_library_stays_refused();
        absent_module_refuses();
        module_map_is_deterministic();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return checker.finish();
}

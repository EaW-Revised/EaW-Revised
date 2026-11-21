// The standing shader-route gate for installed mods (docs/load-notes.md, EAWR-LOAD-2000). The header-only sweep of every
// winning model of the installed mod stack runs through scene::audit_model_shaders; its unhandled (layer, shader) pairs
// must equal the checked-in baseline. A mod update that starts naming a shader nothing draws fails here, instead of its
// layers vanishing in a battle; a family that gains a route fails here until the baseline drops its line.
//
//   shader_route_corpus_tests <baseline file> [--print]
//
// --print writes the current unhandled pairs in baseline format (workshop item id, a tab, the shader name). The stack is
// the Remake's leaf folder (EAWR_REMAKE_MOD_ROOT) with its workshop siblings 3229239424 and 2794270450 beneath/above it as
// the owner's stack mounts them, then the installed game (EAWR_EAW_GAME_ROOT). Without both variables the test is skipped
// (exit 77), like the other installed-data gates.

#include "eawr/scene/shader_routes.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string environment_or(const char* name, const std::string& fallback) {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, name) || !value) return fallback;
    std::string result(value); std::free(value); return result;
#else
    const char* value = std::getenv(name);
    return value ? value : fallback;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: shader_route_corpus_tests <baseline> [--print]\n"; return 2; }
    const bool print = argc > 2 && std::string(argv[2]) == "--print";
    const std::string leaf = environment_or("EAWR_REMAKE_MOD_ROOT", "");
    const std::string game = environment_or("EAWR_EAW_GAME_ROOT", "");
    if (leaf.empty() || game.empty()) { std::cout << "SKIPPED: a mod host with the installed game is required\n"; return 77; }

    // Folders in precedence order: first wins, as the owner's stack mounts FFT, then Holdouts, then the Remake.
    std::vector<fs::path> folders;
    const fs::path workshop = fs::path(leaf).parent_path();
    for (const char* sibling : {"3229239424", "2794270450"})
        if (std::error_code ec; fs::is_directory(workshop / sibling, ec)) folders.push_back(workshop / sibling);
    folders.push_back(leaf);
    std::vector<eawr::vfs::LayerRoot> roots;
    std::vector<std::string> names; // layer id -> folder name
    for (std::size_t i = 0; i < folders.size(); ++i) {
        roots.emplace_back("mod" + std::to_string(i), eawr::vfs::mod_data_root(folders[i]));
        names.push_back(folders[i].filename().string());
    }
    roots.emplace_back("expansion", fs::path(game) / "corruption/Data");
    roots.emplace_back("base", fs::path(game) / "GameData/Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) { std::cerr << "cannot resolve the mod chain: " << chain.error().message << '\n'; return 1; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    if (!filesystem) { std::cerr << "cannot mount the mod chain: " << filesystem.error().message << '\n'; return 1; }

    const auto audit = eawr::scene::audit_model_shaders(filesystem.value());
    const auto folder_of = [&](const std::string& layer) {
        if (layer.rfind("mod", 0) == 0) return names.at(static_cast<std::size_t>(std::stoul(layer.substr(3))));
        return layer;
    };
    std::set<std::string> current;
    for (const auto& entry : audit.unhandled) current.insert(folder_of(entry.layer) + "\t" + entry.shader);

    if (print) {
        for (const auto& line : current) std::cout << line << '\n';
        return 0;
    }
    std::set<std::string> baseline;
    {
        std::ifstream file(argv[1]);
        if (!file) { std::cerr << "cannot read the baseline " << argv[1] << '\n'; return 2; }
        for (std::string line; std::getline(file, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            baseline.insert(line);
        }
    }
    int failures = 0;
    for (const auto& entry : audit.unhandled) {
        const std::string line = folder_of(entry.layer) + "\t" + entry.shader;
        if (baseline.contains(line)) continue;
        std::cerr << "NEW unhandled shader " << entry.shader << " in " << folder_of(entry.layer) << ": " << entry.meshes
                  << " visible mesh layers in " << entry.models << " models (first " << entry.example_model
                  << "); its layers are not drawn. If the owner accepts it, regenerate the baseline with --print\n";
        ++failures;
    }
    for (const auto& line : baseline) {
        if (current.contains(line)) continue;
        std::cerr << "STALE baseline line (now handled or gone, drop it): " << line << '\n';
        ++failures;
    }
    std::cout << "shader route corpus folders (the baseline keys on these names):";
    for (const auto& name : names) std::cout << ' ' << name;
    std::cout << '\n';
    std::cout << "shader route corpus: " << audit.models_scanned << " models, " << audit.visible_meshes
              << " visible mesh layers, " << audit.unhandled.size() << " unhandled pairs, baseline " << baseline.size() << '\n';
    return failures == 0 ? 0 : 1;
}

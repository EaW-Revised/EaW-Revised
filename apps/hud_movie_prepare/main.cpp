// hud_movie_prepare: the VFS half of the user-side HUD movie conversion
// (docs/ui/hud-movies.md, #237). For each --name it resolves the movie through
// the player's own FoC install (and mod chain), and unless the cache already
// holds its Theora entry, copies the Bink bytes to <cache>/<key>.bik for
// tools/ui/convert_hud_movie.py to hand to the player's FFmpeg. It writes one
// "<name>\t<key>\t<cached|extracted>" line per movie to stdout and never
// changes the install.

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/movie.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace ui = eawr::data::ui;

struct Arguments {
    std::filesystem::path game_root;
    std::optional<std::filesystem::path> mod_root;
    std::filesystem::path cache;
    std::vector<std::string> names;
};

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string_view key(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (value.empty()) return std::nullopt;
        if (key == "--game-root") result.game_root = std::filesystem::path(value);
        else if (key == "--mod-root") result.mod_root = std::filesystem::path(value);
        else if (key == "--cache") result.cache = std::filesystem::path(value);
        else if (key == "--name") result.names.emplace_back(value);
        else return std::nullopt;
    }
    if (argc % 2 == 0 || result.game_root.empty() || result.cache.empty() || result.names.empty()) return std::nullopt;
    return result;
}

int fail(const eawr::core::Diagnostic& diagnostic) {
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 1;
}

int fail(const std::string_view code, std::string message) {
    return fail(eawr::core::Diagnostic{std::string(code), eawr::core::Severity::error, std::move(message), {}, {}, {}, {}});
}

} // namespace

int main(const int argc, char** argv) {
    const auto parsed = arguments(argc, argv);
    if (!parsed) {
        std::cerr << "Usage: hud_movie_prepare --game-root <FoC install> [--mod-root <leaf;parent;...>] "
                     "--cache <dir> --name <movies.xml name> [--name ...]\n";
        return 2;
    }
    const Arguments& args = *parsed;

    std::vector<eawr::vfs::LayerRoot> roots;
    if (args.mod_root) roots = eawr::vfs::mod_chain_roots(*args.mod_root);
    roots.emplace_back("expansion", args.game_root / "corruption" / "Data");
    roots.emplace_back("base", args.game_root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) return fail(chain.error());
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& layer : chain.value()) mounts.push_back(std::move(layer.mount));
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    if (!filesystem) return fail(filesystem.error());

    std::error_code error;
    std::filesystem::create_directories(args.cache, error);
    if (error) return fail(ui::diagnostic_codes::movie_conversion, "cannot create the movie cache: " + error.message());

    for (const std::string& name : args.names) {
        const auto movie = ui::resolve_hud_movie(filesystem.value(), name);
        if (!movie) return fail(movie.error());
        const std::string& key = movie.value().cache_key;
        if (std::filesystem::is_regular_file(args.cache / ui::movie_cache_file(movie.value()), error)) {
            std::cout << name << '\t' << key << "\tcached\n";
            continue;
        }
        const auto bytes = filesystem.value().open(movie.value().source.canonical_path);
        if (!bytes) return fail(bytes.error());
        std::ofstream file(args.cache / (key + ".bik"), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        file.close();
        if (!file) return fail(ui::diagnostic_codes::movie_conversion, "cannot write the movie source into the cache");
        std::cout << name << '\t' << key << "\textracted\n";
    }
    return 0;
}

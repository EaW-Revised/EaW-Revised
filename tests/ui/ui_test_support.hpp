#pragma once

// Shared helpers for ui_data_tests. Corpus checks are opt-in: they run only
// when EAWR_EAW_GAME_ROOT names a read-only game installation.

#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::test::ui {

inline int& failures() {
    static int count{};
    return count;
}

inline void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures();
    }
}

inline std::optional<std::string> environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

inline void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

inline void write_text(const std::filesystem::path& path, const std::string_view text) {
    write_bytes(path, std::as_bytes(std::span(text.data(), text.size())));
}

struct TempTree final {
    std::filesystem::path root;
    explicit TempTree(const std::string_view tag) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("eawr-ui-" + std::string(tag) + "-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

// The effective FoC view (corruption over GameData, manifest archives and
// patch slots), or nothing when EAWR_EAW_GAME_ROOT is unset.
inline std::optional<eawr::vfs::Vfs> foc_corpus(const std::string_view suite) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << suite << " corpus: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return std::nullopt;
    }
    const std::filesystem::path game_root(*root);
    auto expansion = eawr::vfs::resolve_manifest_mount("expansion", game_root / "corruption" / "Data");
    auto base = eawr::vfs::resolve_manifest_mount("base", game_root / "GameData" / "Data");
    expect(expansion && base, "FoC and base manifests resolve");
    if (!expansion || !base) return std::nullopt;
    const std::array mounts{std::move(expansion.value().mount), std::move(base.value().mount)};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "FoC VFS mounts");
    if (!filesystem) return std::nullopt;
    return std::move(filesystem.value());
}

struct ModCorpus final {
    std::string name;
    eawr::vfs::Vfs filesystem;
};

// Installed mods for opt-in corpus checks (docs/ui/mod-hud-survey.md,
// section 6). EAWR_MOD_HUD_ROOTS lists name=leaf[;parent] entries separated
// by '|'; a root is a mod folder or its Data folder, and a submod names the
// mod it builds on as parent. Each mod mounts over the FoC view, so
// EAWR_EAW_GAME_ROOT is needed too. Nothing when either is unset.
inline std::vector<ModCorpus> mod_corpora(const std::string_view suite) {
    std::vector<ModCorpus> result;
    const auto list = environment("EAWR_MOD_HUD_ROOTS");
    const auto game = environment("EAWR_EAW_GAME_ROOT");
    if (!list || !game) {
        std::cout << suite << " mod corpus: skipped (set EAWR_MOD_HUD_ROOTS and EAWR_EAW_GAME_ROOT)\n";
        return result;
    }
    const auto split = [](std::string_view text, const char separator) {
        std::vector<std::string_view> parts;
        while (!text.empty()) {
            const auto end = text.find(separator);
            if (end != 0U) parts.push_back(text.substr(0U, end));
            if (end == std::string_view::npos) break;
            text.remove_prefix(end + 1U);
        }
        return parts;
    };
    const std::filesystem::path game_root(*game);
    for (const auto entry : split(*list, '|')) {
        const auto equals = entry.find('=');
        expect(equals != std::string_view::npos && equals != 0U && equals + 1U < entry.size(),
               "EAWR_MOD_HUD_ROOTS entries are name=leaf[;parent]");
        if (equals == std::string_view::npos || equals == 0U || equals + 1U >= entry.size()) continue;
        auto roots = eawr::vfs::mod_chain_roots(std::filesystem::path(std::string(entry.substr(equals + 1U))));
        roots.emplace_back("expansion", game_root / "corruption" / "Data");
        roots.emplace_back("base", game_root / "GameData" / "Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        expect(static_cast<bool>(chain), "a mod corpus chain resolves its manifests");
        if (!chain) continue;
        std::vector<eawr::vfs::MountSpec> mounts;
        for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
        auto filesystem = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(filesystem), "a mod corpus VFS mounts");
        if (filesystem) result.push_back({std::string(entry.substr(0U, equals)), std::move(filesystem.value())});
    }
    return result;
}

} // namespace eawr::test::ui

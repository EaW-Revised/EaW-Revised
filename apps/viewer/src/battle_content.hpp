#pragma once

#include "eawr/core/content_chain.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace eawr::presentation::shader_activation { struct Generation; }

namespace eawr::presentation::godot_backend {
struct ShaderActivationReceipt;

// One setup screen's installed-content snapshot, immutable apart from the
// filesystem and catalog's mutex-guarded caches. Session data stays per battle.
struct BattleContent final {
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::shared_ptr<const data::Catalog> catalog;
    std::string profile;
    std::vector<std::string> layers;
    std::filesystem::path game_root{};
    std::filesystem::path mod_root{};
    // Empty while translation is disabled. Readers pin an immutable admitted
    // generation; no compiler/job or mutable cache enters battle content.
    std::shared_ptr<const shader_activation::Generation> shader_generation{};
    std::shared_ptr<const ShaderActivationReceipt> shader_receipt{};

    [[nodiscard]] bool matches_chain(const std::filesystem::path& requested_game_root,
        const std::filesystem::path& requested_mod_root, const std::string& requested_profile,
        const std::vector<std::string>& requested_layers) const {
        return core::content_chain_matches(game_root, mod_root, profile, layers,
            requested_game_root, requested_mod_root, requested_profile, requested_layers);
    }
};

} // namespace eawr::presentation::godot_backend

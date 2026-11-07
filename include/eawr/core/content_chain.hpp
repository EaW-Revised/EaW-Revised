#pragma once

#include <string>
#include <vector>

namespace eawr::core {

// The root paths and ordered layer IDs together identify a resolved content chain.
template <typename Path>
[[nodiscard]] inline bool content_chain_matches(const Path& game_root,
    const Path& mod_root, const std::string& profile, const std::vector<std::string>& layers,
    const Path& requested_game_root, const Path& requested_mod_root,
    const std::string& requested_profile, const std::vector<std::string>& requested_layers) {
    return game_root == requested_game_root && mod_root == requested_mod_root
        && profile == requested_profile && layers == requested_layers;
}

} // namespace eawr::core

#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <memory>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// One setup screen's immutable installed-content snapshot. Battle state and
// Godot resources remain per battle; no mutable session data lives here.
struct BattleContent final {
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::shared_ptr<const data::Catalog> catalog;
    std::string profile;
    std::vector<std::string> layers;
};

} // namespace eawr::presentation::godot_backend

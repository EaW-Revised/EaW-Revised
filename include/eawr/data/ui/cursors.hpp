#pragma once

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eawr::data::ui {

// CU-01: metadata only; the viewer decodes frames from the player's VFS.
struct CursorDefinition final {
    std::string name;
    std::string base_texture;
    std::uint32_t hot_x{};
    std::uint32_t hot_y{};
    std::uint32_t frame_delay{};
    std::vector<std::string> frames;
};
using CursorCatalog = std::map<std::string, CursorDefinition, std::less<>>;
[[nodiscard]] core::Result<CursorCatalog> load_cursors(const vfs::Vfs& filesystem);

} // namespace eawr::data::ui

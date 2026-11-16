#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace eawr::presentation::ui {

// A picture the data names by file (the main-menu overlay, a credits logo): the authored name goes through the shared
// asset-name resolver (docs/behaviour/asset-name-resolution.md, ANR-01: as authored, then its stem with the sibling
// extensions) and the file found is decoded. A name that resolves to no file is reported once as
// EAWR-ASSET-NAME-0001 with `site`. Engine free; the Godot screens turn `texture` into an image.
struct NamedPicture final {
    std::string logical_path;                // empty when no file was found
    std::optional<assets::Texture> texture;  // empty when nothing was found or the file did not decode
    std::string problem;                     // why `texture` is empty
};

[[nodiscard]] NamedPicture load_named_picture(const vfs::Vfs& filesystem, std::string_view name,
                                              std::string_view site);

} // namespace eawr::presentation::ui

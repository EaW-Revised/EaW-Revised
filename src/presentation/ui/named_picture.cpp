#include "eawr/presentation/ui/named_picture.hpp"

#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/core/diagnostic.hpp"

#include <utility>

namespace eawr::presentation::ui {

NamedPicture load_named_picture(const vfs::Vfs& filesystem, const std::string_view name, const std::string_view site) {
    NamedPicture picture;
    picture.logical_path = assets::resolve_texture(filesystem, name, site);
    if (picture.logical_path.empty()) {
        picture.problem = std::string(name) + " is not in the data";
        return picture;
    }
    auto texture = assets::load_texture(filesystem, picture.logical_path);
    if (!texture) {
        picture.problem = core::format_diagnostic(texture.error());
        return picture;
    }
    picture.texture = std::move(texture.value());
    return picture;
}

} // namespace eawr::presentation::ui

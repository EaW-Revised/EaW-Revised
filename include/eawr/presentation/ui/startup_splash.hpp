#pragma once

#include "eawr/assets/asset_names.hpp"
#include "eawr/core/text.hpp"
#include "eawr/data/xml.hpp"

#include <string>
#include <string_view>

namespace eawr::presentation::ui {

[[nodiscard]] inline core::Result<data::XmlDocument> startup_constants(const vfs::Vfs& filesystem) {
    return data::load_document(filesystem, "data/xml/gameconstants.xml");
}

[[nodiscard]] inline std::string startup_splash_name(const data::XmlNode& root,
    std::string_view language, bool uk) {
    std::string name;
    for (const auto& node : root.children) {
        if (uk && core::iequals(node.name, "Localized_UK_English_Splash_Screen")) {
            name = core::trim(node.raw_text);
        } else if (!uk && core::iequals(node.name, "Localized_Splash_Screen")) {
            const auto comma = node.raw_text.find(',');
            if (comma != std::string::npos && core::iequals(core::trim_view(node.raw_text.substr(0, comma)), language))
                name = core::trim(node.raw_text.substr(comma + 1));
        }
    }
    return name;
}

// Probe each mounted layer before moving to the next: a DDS in a higher layer
// must beat a TGA in a lower one. The probe can reject unreadable candidates.
template <typename Layers, typename Accept>
[[nodiscard]] bool select_startup_splash(const Layers& layers, std::string_view name, Accept&& accept) {
    if (name.empty()) name = "Splash.TGA";
    for (const auto& layer : layers) {
        const auto path = assets::resolve_texture_name("data/art/textures/", name,
            [&](const std::string& candidate) { return accept(layer, candidate); });
        if (!path.empty()) return true;
    }
    return false;
}

} // namespace eawr::presentation::ui

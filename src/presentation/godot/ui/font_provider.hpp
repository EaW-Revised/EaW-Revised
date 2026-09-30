#pragma once

// Godot fonts for the UI layer (docs/ui/ui-layer.md section 3.1, ticket UI-05
// #191). Cached EmpireAtWar faces become FontFiles built in memory from the
// cache bytes, other faces SystemFonts, and the end of the UI-F3 chain the
// engine's fallback font. Faces are resolved by presentation::ui::resolve_font.

#include "eawr/presentation/ui/fonts.hpp"

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/system_font.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

class FontProvider final {
public:
    // Loads every cached face into a FontFile. A face the engine cannot read
    // leaves the cache with one EAWR-UI-0402 warning, so UI-F3 falls past it.
    explicit FontProvider(presentation::ui::FontCache cache);

    [[nodiscard]] const presentation::ui::FontCache& cache() const noexcept { return cache_; }
    // The platform's font families, as the system probe of UI-F3 sees them.
    [[nodiscard]] const std::vector<std::string>& system_families() const noexcept { return system_families_; }

    [[nodiscard]] presentation::ui::ResolvedFont resolve(const presentation::ui::FontRequest& request,
                                                         std::string_view language) const;
    // The engine font of a resolved face; never null.
    [[nodiscard]] godot::Ref<godot::Font> font(const presentation::ui::ResolvedFont& resolved);
    // The FontFile of a cached face, or null.
    [[nodiscard]] godot::Ref<godot::FontFile> cached_font(std::string_view face) const;

private:
    presentation::ui::FontCache cache_;
    std::map<std::string, godot::Ref<godot::FontFile>> files_;
    std::vector<std::string> system_families_;
    std::map<std::string, godot::Ref<godot::SystemFont>> system_fonts_;
};

} // namespace eawr::presentation::godot_backend

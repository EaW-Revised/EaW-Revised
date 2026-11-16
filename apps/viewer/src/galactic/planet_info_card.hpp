#pragma once

// The persistent planet information card above the PLANET INFO button of the focused galactic
// view (docs/behaviour/galactic-planet-info-card.md, GPI-01..09). This header is the engine-free
// model: admission, content and geometry. The Godot nodes live in planet_info_card.cpp.
// Authored numbers arrive through the style from the effective command-bar data; the model holds none of its
// own. The frame and glow growth (GPI-06) is the one literal pair, set by the viewer and marked unverified.

#include "planet_hud.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// Shell units (y up), the same space as the command bar's anchors.
struct CardRect final {
    float x{}, y{}, width{}, height{};
    [[nodiscard]] float top() const noexcept { return y + height; }
    friend constexpr bool operator==(const CardRect&, const CardRect&) noexcept = default;
};

// GPI-03/04/05/06: the focused backdrop's authored values and the button anchor.
struct PlanetInfoCardStyle final {
    float anchor_x{}, anchor_y{};              // resolved shell position of the planet_info button
    float back_width{}, back_row_minimum{};    // backdrop Size: width and ordinary row minimum
    float default_offset_x{}, default_offset_y{};
    float offset_x{}, offset_y{};              // content inset (x, both edges) and added height (y)
    float header_row_minimum{};                // focused header text row minimum
    float frame_grow{}, glow_grow{};           // GPI-06: total growth over the backdrop, shell units
};

struct PlanetInfoCardLayout final {
    CardRect background, frame, glow;
    float content_left{}, content_width{};
    std::vector<CardRect> rows;                // top to bottom, inside the content box
};

// GPI-05: the greatest participating icon height or text-row minimum; excluded items never enter `icons`.
[[nodiscard]] inline float planet_info_card_row_height(const float minimum, const std::span<const float> icons,
    const float text = 0.0F) noexcept {
    float height = std::max(minimum, text);
    for (const float icon : icons) height = std::max(height, icon);
    return height;
}

// GPI-04/05: the backdrop's bottom is anchor.y + default_offset.y and its centre x is one half-width right of
// anchor.x + default_offset.x. The height is the row heights plus the backdrop's offset Y; rows stack from the top
// downward, each starting where the previous one ended.
[[nodiscard]] inline PlanetInfoCardLayout planet_info_card_layout(const PlanetInfoCardStyle& style,
    const std::span<const float> row_heights) {
    PlanetInfoCardLayout out;
    float rows = 0.0F;
    for (const float height : row_heights) rows += std::max(0.0F, height);
    const float height = rows + style.offset_y;
    out.background = {style.anchor_x + style.default_offset_x, style.anchor_y + style.default_offset_y,
        style.back_width, height};
    const auto grown = [&](const float amount) {
        return CardRect{out.background.x - amount * 0.5F, out.background.y - amount * 0.5F,
            out.background.width + amount, out.background.height + amount};
    };
    out.frame = grown(style.frame_grow);
    out.glow = grown(style.glow_grow);
    out.content_left = out.background.x + style.offset_x;
    out.content_width = std::max(0.0F, style.back_width - 2.0F * style.offset_x);
    float top = out.background.top() - style.offset_y;
    for (const float row : row_heights) {
        const float h = std::max(0.0F, row);
        out.rows.push_back({out.content_left, top - h, out.content_width, h});
        top -= h;
    }
    return out;
}

// GPI-01/02: what the card shows for a planet. Absent facts, an undiscovered planet or a missing name admit nothing.
// Discovery is a planet's only reveal state (docs note: fog under GPI-02), so it is the whole fog gate.
struct PlanetInfoCardContent final {
    ObjectId planet{};
    std::string name;
    // GPI-07: the planet type's Additional_Population_Capacity; nonzero picks the badge header.
    std::int64_t population{};
    std::optional<PlayerId> emblem_owner;      // faction emblem through the galaxy-overview fleet icon table
    std::string weather_icon;                  // GPI-08: a living planet's weather icon
    std::vector<std::string> description;      // localized Encyclopedia_Text entries, authored order
    // GPI-09: the type's maximum per category and the known filled count (zero when unknown).
    std::int64_t station_max{}, station_filled{}, land_max{}, land_filled{};
    Rgb pip_colour;                            // owner colour when known, else the neutral colour
    friend bool operator==(const PlanetInfoCardContent&, const PlanetInfoCardContent&) = default;
};

// GPI-01: focused camera, shell rendering admitted, a non-tutorial local player, advanced map closed.
struct PlanetInfoCardGate final {
    bool focused{}, shell_visible{}, tutorial_locked{}, advanced_map_open{};
    [[nodiscard]] constexpr bool open() const noexcept {
        return focused && shell_visible && !tutorial_locked && !advanced_map_open;
    }
};

[[nodiscard]] inline std::optional<PlanetInfoCardContent> planet_info_card_content(const PlanetHudFacts* facts,
    const PlanetView& planet) {
    if (!facts || !facts->discovered || facts->display_name.empty()) return std::nullopt;
    PlanetInfoCardContent out;
    out.planet = planet.id;
    out.name = facts->display_name;
    out.population = planet.info.population_capacity;
    out.emblem_owner = facts->observed_owner;
    out.weather_icon = planet.weather.icon;
    out.description = planet.info.encyclopedia;
    out.station_max = std::max<std::int64_t>(0, facts->space_capacity.value_or(0));
    out.land_max = std::max<std::int64_t>(0, facts->ground_capacity.value_or(0));
    if (facts->space_level_known && facts->space_level)
        out.station_filled = std::clamp<std::int64_t>(*facts->space_level, 0, out.station_max);
    if (facts->structures_known && facts->structures)
        out.land_filled = std::clamp<std::int64_t>(static_cast<std::int64_t>(*facts->structures), 0, out.land_max);
    out.pip_colour = facts->name_colour;
    return out;
}

// GPI-07: positive contribution is drawn as written; a negative one shows its magnitude (the colour differs).
[[nodiscard]] inline std::string planet_info_card_population_text(const PlanetInfoCardContent& content) {
    return content.population == 0 ? std::string{} : std::to_string(content.population < 0 ? -content.population : content.population);
}

} // namespace eawr::viewer::galactic

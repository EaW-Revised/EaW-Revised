#include "galactic/planet_info_card.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace eawr::viewer::galactic;

namespace {
// Synthetic authored values (not a retail pin): any backdrop must lay out from its own numbers.
PlanetInfoCardStyle synthetic_style() {
    PlanetInfoCardStyle style;
    style.anchor_x = 400.0F; style.anchor_y = 100.0F;
    style.back_width = 200.0F; style.back_row_minimum = 10.0F;
    style.default_offset_x = -90.0F; style.default_offset_y = 24.0F;
    style.offset_x = 6.0F; style.offset_y = 3.0F;
    style.header_row_minimum = 16.0F;
    style.frame_grow = 2.0F; style.glow_grow = 16.0F;
    return style;
}
bool near(const float a, const float b) { return std::fabs(a - b) < 1e-4F; }
PlanetHudFacts discovered_facts() {
    PlanetHudFacts facts;
    facts.planet = 7; facts.discovered = true; facts.display_name = "Synth";
    facts.observed_owner = 2; facts.name_colour = {0.1F, 0.2F, 0.9F};
    facts.space_capacity = 5; facts.ground_capacity = 8;
    facts.space_level = 3; facts.space_level_known = true;
    facts.structures = 2; facts.structures_known = true;
    return facts;
}
PlanetView synthetic_planet() {
    PlanetView planet;
    planet.id = 7;
    planet.weather.icon = "synthetic_weather.tga";
    planet.info.encyclopedia = {"first", "second"};
    planet.info.population_capacity = 5;
    return planet;
}
} // namespace

int case_galactic_planet_info_card_tests() {
    {
        // GPI-04/05: the backdrop's bottom is anchor + default offset; its height is the rows plus offset Y; the
        // content box is inset by offset X on both edges and rows stack from the top downward.
        const auto style = synthetic_style();
        const std::array<float, 3> rows{20.0F, 10.0F, 30.0F};
        const auto layout = planet_info_card_layout(style, rows);
        assert(near(layout.background.x, 310.0F) && near(layout.background.y, 124.0F));
        assert(near(layout.background.width, 200.0F) && near(layout.background.height, 63.0F));
        assert(near(layout.content_left, 316.0F) && near(layout.content_width, 188.0F));
        assert(layout.rows.size() == 3);
        assert(near(layout.rows[0].top(), layout.background.top() - style.offset_y));
        assert(near(layout.rows[1].top(), layout.rows[0].y) && near(layout.rows[2].top(), layout.rows[1].y));
        assert(near(layout.rows[2].y, layout.background.y));
        // GPI-06: the frame and glow grow evenly around the same centre.
        assert(near(layout.frame.x, layout.background.x - 1.0F) && near(layout.frame.width, 202.0F));
        assert(near(layout.glow.y, layout.background.y - 8.0F) && near(layout.glow.height, layout.background.height + 16.0F));
    }
    {
        // GPI-05: content-driven height. More description rows make the card taller and keep its bottom fixed.
        const auto style = synthetic_style();
        const auto small = planet_info_card_layout(style, std::array<float, 2>{16.0F, 10.0F});
        const auto large = planet_info_card_layout(style, std::array<float, 3>{16.0F, 10.0F, 25.0F});
        assert(near(large.background.y, small.background.y) && near(large.background.height - small.background.height, 25.0F));
        assert(planet_info_card_layout(style, std::span<const float>{}).rows.empty());
    }
    {
        // GPI-05: the row is the greatest participating icon height or the minimum; a wrapped text raises it.
        const std::array<float, 2> icons{12.0F, 30.0F};
        assert(near(planet_info_card_row_height(16.0F, icons), 30.0F));
        assert(near(planet_info_card_row_height(16.0F, std::span<const float>{}), 16.0F));
        assert(near(planet_info_card_row_height(10.0F, std::span<const float>{}, 44.0F), 44.0F));
    }
    {
        // GPI-01: the card needs the focused camera, the shell, a non-tutorial player and the advanced map closed.
        assert((PlanetInfoCardGate{true, true, false, false}.open()));
        assert(!(PlanetInfoCardGate{false, true, false, false}.open()));
        assert(!(PlanetInfoCardGate{true, false, false, false}.open()));
        assert(!(PlanetInfoCardGate{true, true, true, false}.open()));
        assert(!(PlanetInfoCardGate{true, true, false, true}.open()));
    }
    {
        // GPI-02/07/08/09: content from the observer's facts and the planet's authored metadata.
        const auto facts = discovered_facts();
        const auto content = planet_info_card_content(&facts, synthetic_planet());
        assert(content && content->name == "Synth" && content->population == 5);
        assert(content->emblem_owner && *content->emblem_owner == 2);
        assert(content->weather_icon == "synthetic_weather.tga");
        assert(content->description == std::vector<std::string>({"first", "second"}));
        assert(content->station_max == 5 && content->station_filled == 3 && content->land_max == 8 && content->land_filled == 2);
        assert(planet_info_card_population_text(*content) == "5");
    }
    {
        // GPI-02: an absent, undiscovered or unnamed planet admits no card.
        assert(!planet_info_card_content(nullptr, synthetic_planet()));
        auto facts = discovered_facts();
        facts.discovered = false;
        assert(!planet_info_card_content(&facts, synthetic_planet()));
        facts = discovered_facts(); facts.display_name.clear();
        assert(!planet_info_card_content(&facts, synthetic_planet()));
    }
    {
        // GPI-07: a negative contribution shows its magnitude; zero shows nothing.
        auto facts = discovered_facts();
        auto content = planet_info_card_content(&facts, synthetic_planet());
        auto planet = synthetic_planet();
        planet.info.population_capacity = -3;
        content = planet_info_card_content(&facts, planet);
        assert(content && planet_info_card_population_text(*content) == "3");
        planet.info.population_capacity = 0;
        content = planet_info_card_content(&facts, planet);
        assert(content && planet_info_card_population_text(*content).empty());
    }
    {
        // GPI-09: unknown levels fill nothing; the type's maximum still draws; fills never exceed the maximum.
        auto facts = discovered_facts();
        facts.space_level_known = false; facts.structures_known = false;
        auto content = planet_info_card_content(&facts, synthetic_planet());
        assert(content && content->station_max == 5 && content->station_filled == 0 && content->land_max == 8 && content->land_filled == 0);
        facts = discovered_facts(); facts.space_level = 9; facts.structures = 40;
        content = planet_info_card_content(&facts, synthetic_planet());
        assert(content && content->station_filled == 5 && content->land_filled == 8);
        facts = discovered_facts(); facts.space_capacity.reset(); facts.ground_capacity.reset();
        content = planet_info_card_content(&facts, synthetic_planet());
        assert(content && content->station_max == 0 && content->land_max == 0 && content->station_filled == 0);
    }
    return 0;
}

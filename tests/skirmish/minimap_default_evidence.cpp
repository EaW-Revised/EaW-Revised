#include "skirmish_start_support.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

#include <array>
#include <iostream>
#include <vector>

namespace skirmish_start_test_support {
void minimap_default_evidence() {
    namespace ui = eawr::presentation::ui;
    using eawr::data::ui::Rgba8;
    auto tables = synthetic_tables();
    auto inputs = synthetic_inputs(tables);
    // Effective stock GameConstants palette, fed into the real start builder.
    inputs.lobby_colours = {{"Blue", {78,150,237}}, {"Red", {237,78,78}},
        {"Green", {119,237,78}}, {"Orange", {237,149,78}}};
    const std::array<Rgba8, 4> expected{{{78,150,237,255}, {237,78,78,255},
        {119,237,78,255}, {237,149,78,255}}};
    for (const bool teammate : {false, true}) {
        auto fixture = synthetic_fixture();
        fixture.slots = {{1, "Rebel", 0, true, {}}, {2, teammate ? "Rebel" : "Empire", teammate ? 0U : 1U, false, {}},
            {3, "Empire", 1, false, {}}, {4, "Empire", 1, false, {}}};
        const auto start = skirmish::build_start(fixture, inputs);
        expect(static_cast<bool>(start), "SK-5346 default colour setup builds");
        if (!start) continue;
        std::vector<tactical::SnapshotPlayer> players;
        for (const auto& p : start.value().players) if (p.lobby)
            players.push_back({p.player.player_id, p.player.team_id, false, true});
        ui::MinimapTypeLooks looks;
        looks.visible = true;
        ui::MinimapSettings settings;
        settings.colorize_enemy = false;
        std::vector<ui::MinimapUnit> units;
        for (const auto& p : start.value().players) {
            if (!p.lobby || !p.colour) continue;
            const auto rgb = p.colour->rgb;
            ui::MinimapUnit unit;
            unit.id = p.player.player_id;
            unit.owner_colour = {rgb[0], rgb[1], rgb[2], 255};
            unit.hostile = tactical::players_hostile(players, p.player.player_id, 1);
            units.push_back(unit);
        }
        const auto blips = ui::minimap_blips(units, [&](std::string_view) -> const ui::MinimapTypeLooks& { return looks; },
            ui::minimap_extents(-1, 1, -1, 1), settings);
        expect(blips.size() == 4, "SK-5346 all four visible blips admitted");
        for (const auto& b : blips) {
            const auto index = static_cast<std::size_t>(b.id - 1);
            expect(index < expected.size() && b.colour == expected[index], "SK-5346 blip retains its lobby colour");
            std::cout << "SK-5346 " << (teammate ? "two-team-with-ally" : "default-two-team")
                << " player=" << b.id << " index=" << index << " hostile="
                << tactical::players_hostile(players, static_cast<tactical::PlayerId>(b.id), 1)
                << " rgb=" << static_cast<unsigned>(b.colour.r) << ',' << static_cast<unsigned>(b.colour.g)
                << ',' << static_cast<unsigned>(b.colour.b) << '\n';
        }
    }
}
} // namespace skirmish_start_test_support

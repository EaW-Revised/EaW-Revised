// #494: FoC's space fog of war in the world (docs/behaviour/space-fog-presentation.md FW-01 to
// FW-14): the constants, the grid, the per-cell fade, the border ring, the blur and the ramp.
#include "eawr/presentation/space/fog_field.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] eawr::data::XmlNode node(std::string name, std::string text) {
    eawr::data::XmlNode result;
    result.name = std::move(name);
    result.raw_text = std::move(text);
    return result;
}

void test_looks() {
    eawr::data::XmlNode root;
    root.name = "GameConstants";
    root.children.push_back(node("SpaceFOWColor", "255,255,255,255"));
    root.children.push_back(node("SpaceFOWHeight", "-80.0"));
    root.children.push_back(node("DesiredSpaceFOWCellSize", "100.0"));
    root.children.push_back(node("SpaceFOWRegrowTime", "6.0"));
    root.children.push_back(node("SpaceReinforceFOWColor", "255,0,0,254"));
    const space::FogLooks looks = space::fog_looks(root);
    expect(looks.diagnostics.empty(), "FoC's values read without a fallback");
    expect(looks.reinforce_colour == std::array<std::uint8_t, 4>{255, 0, 0, 254}, "FW-22: the reinforcement colour");
    expect(looks.colour == std::array<std::uint8_t, 4>{255, 255, 255, 255} && looks.height == -80.0
               && looks.cell_size == 100.0 && looks.regrow_seconds == 6.0,
           "FW-01: colour, height, cell size and regrow time");
    root.children.push_back(node("SpaceFOWColor", "10, 20, 30, 40"));
    root.children.push_back(node("DesiredSpaceFOWCellSize", "-5"));
    const space::FogLooks later = space::fog_looks(root);
    expect(later.colour == std::array<std::uint8_t, 4>{10, 20, 30, 40}, "the last entry wins");
    expect(later.cell_size == 100.0 && later.diagnostics.size() == 1, "a bad cell size falls back and says so");
    eawr::data::XmlNode empty;
    expect(space::fog_looks(empty).diagnostics.size() == 5, "every absent value is reported");
}

void test_steps_and_table() {
    expect(space::fog_fade_step_per_frame(6.0) == 21.0 / 16.0, "FW-05: 6 s regrow drops 21 a service, 21/16 a frame");
    expect(space::fog_intensity(0.0) == 0 && space::fog_intensity(8.0) == 127 && space::fog_intensity(16.0) == 255
               && space::fog_intensity(238.0) == 255,
           "FW-09: fade values 0..16 run from fogged to clear, then stay clear");
    expect(space::fog_intensity(239.0) == 28 && space::fog_intensity(255.0) == 255 && space::fog_intensity(247.0) == 141,
           "FW-09: the fade-in runs from 28 at 239 to 255 at 255");
}

void test_layout() {
    const auto layout = space::fog_field_layout(-6500.0, 6500.0, -6450.0, 6450.0, 100.0);
    expect(layout && layout->wide == 130 && layout->tall == 129 && layout->left == -6500.0 && layout->top == 6450.0,
           "FW-07: whole cells over the rectangle, row 0 at the top");
    const auto odd = space::fog_field_layout(0.0, 250.0, 0.0, 100.0, 100.0);
    expect(odd && odd->wide == 3 && odd->left == -25.0, "a partial cell widens around the centre");
    expect(!space::fog_field_layout(0.0, 0.0, 0.0, 1.0, 100.0), "an empty rectangle has no grid");
    expect(!space::fog_field_layout(0.0, 1.0e6, 0.0, 1.0, 100.0), "more than 4096 cells a side is refused");
}

[[nodiscard]] space::FogField field() {
    // 21 x 21 cells of 100 around the origin: cell (10, 10) is centred on (0, 0).
    return space::FogField(*space::fog_field_layout(-1050.0, 1050.0, -1050.0, 1050.0, 100.0), space::FogLooks{});
}

void test_fade_in_and_out() {
    space::FogField fog = field();
    const std::vector<space::FogFieldRevealer> one{{0.0, 0.0, 300.0}};
    fog.advance(one, 1.0);
    expect(fog.held_cells() == 29, "FW-08: a 300 range holds the 29 cell centres within 3 cells (inclusive)");
    expect(fog.value(10, 10) == 239.0, "FW-09: a fogged cell starts its fade-in at 239");
    expect(fog.value(13, 10) == 239.0 && fog.value(14, 10) == 0.0, "the circle's edge cell is held, the next is not");
    for (int frame = 0; frame < 20; ++frame) fog.advance(one, 1.0);
    expect(fog.value(10, 10) == 255.0 && fog.intensity(10, 10) == 255, "sixteen frames later the cell is clear");
    expect(fog.intensity(13, 10) < 255 && fog.intensity(13, 10) > 0, "FW-12: the blur softens the circle's edge");
    const std::vector<space::FogFieldRevealer> none;
    fog.advance(none, 1.0);
    expect(std::abs(fog.value(10, 10) - (16.0 - 21.0 / 16.0)) < 1e-9, "FW-10: a released cell fades from 16");
    int frames = 1;
    while (fog.value(10, 10) > 0.0 && frames < 100) {
        fog.advance(none, 1.0);
        ++frames;
    }
    expect(frames == 13, "FW-10: the fade-out takes 13 frames at 21/16 a frame");
    expect(fog.fogged_cells() == 21U * 21U, "everything is fogged again");
    fog.advance(none, 0.0);
    expect(!fog.changed(), "an unchanged frame changes no texel");
}

void test_resume_and_pause() {
    space::FogField fog = field();
    const std::vector<space::FogFieldRevealer> one{{0.0, 0.0, 150.0}};
    fog.advance(one, 1.0);
    for (int frame = 0; frame < 7; ++frame) fog.advance(one, 1.0);
    expect(fog.value(10, 10) == 246.0, "seven frames into the fade-in");
    const std::vector<space::FogFieldRevealer> none;
    fog.advance(none, 0.0);
    const double resumed = space::fog_intensity(246.0) / 255.0 * 16.0;
    expect(std::abs(fog.value(10, 10) - resumed) < 1e-9, "FW-10: an unfinished fade-in fades out from where it shows");
    fog.advance(one, 0.0);
    expect(fog.value(10, 10) == std::floor(255.0 - resumed), "FW-09: a fading cell held again resumes the fade-in");
    const double before = fog.value(10, 10);
    fog.advance(one, 0.0);
    expect(fog.value(10, 10) == before, "a paused battle holds every fade");
}

// FW-08, FW-10 with the session's fog cells (#494 on #495's grid).
void test_cells() {
    space::FogField fog = field();
    std::vector<std::uint8_t> cells(21U * 21U, 0U);
    const auto at = [](const std::uint32_t column, const std::uint32_t row) { return row * 21U + column; };
    cells[at(10, 10)] = 255U;
    fog.advance(cells, 0.0, 1.0);
    expect(fog.held_cells() == 1 && fog.value(10, 10) == 239.0, "FW-09: a held cell fades in from 239");
    for (int frame = 0; frame < 16; ++frame) fog.advance(cells, 0.0, 1.0);
    expect(fog.value(10, 10) == 255.0, "and is clear 16 frames later");
    // Released: the grid lingers at 238, lowered by 21 a service; between services the shown value
    // drops 21/16 a frame.
    cells[at(10, 10)] = 238U;
    fog.advance(cells, 5.0, 1.0);
    expect(std::abs(fog.value(10, 10) - (238.0 - 5.0 * 21.0 / 16.0)) < 1e-9 && fog.intensity(10, 10) > 0,
           "FW-10: a lingering cell shows its grid value less the frames since the service");
    expect(fog.held_cells() == 0, "a lingering cell is not held");
    cells[at(10, 10)] = 8U;
    fog.advance(cells, 0.0, 1.0);
    expect(fog.value(10, 10) == 8.0 && space::fog_intensity(8.0) == 127, "below 16 the lingering cell fades");
    fog.advance(cells, 4.0, 1.0);
    expect(std::abs(fog.value(10, 10) - (8.0 - 4.0 * 21.0 / 16.0)) < 1e-9, "the fade runs between services");
    fog.advance(cells, 7.0, 1.0);
    expect(fog.value(10, 10) == 0.0, "and never drops below fogged");
    cells[at(10, 10)] = 0U;
    fog.advance(cells, 8.0, 1.0);
    expect(fog.value(10, 10) == 0.0 && fog.fogged_cells() == 21U * 21U, "a zero cell is fogged");
    // An unfinished fade-in finishes while the cell lingers.
    cells[at(4, 4)] = 255U;
    fog.advance(cells, 0.0, 1.0);
    fog.advance(cells, 0.0, 3.0);
    cells[at(4, 4)] = 200U;
    fog.advance(cells, 0.0, 2.0);
    expect(fog.value(4, 4) == 244.0, "FW-10: a lingering cell finishes its fade-in first");
    const std::vector<std::uint8_t> wrong(10U, 255U);
    fog.advance(wrong, 0.0, 1.0);
    expect(fog.value(4, 4) == 244.0, "cells of another size change nothing");
}

void test_texture() {
    space::FogField fog = field();
    const auto texels = fog.texels();
    expect(texels.size() == 21U * 21U * 4U, "one RGBA texel per cell");
    expect(texels[0] == 255 && texels[3] == 255, "FW-11: a fogged cell takes SpaceFOWColor");
    const std::vector<space::FogFieldRevealer> all{{0.0, 0.0, 5000.0}};
    for (int frame = 0; frame < 20; ++frame) fog.advance(all, 1.0);
    const auto centre = (10U * 21U + 10U) * 4U;
    expect(fog.texels()[centre + 3] == 0 && fog.texels()[centre] == 0, "FW-11: a clear cell is transparent");
    expect(fog.intensity(0, 0) == 0 && fog.intensity(20, 10) == 0 && fog.texels()[3] == 255,
           "FW-12: the outermost ring stays fogged");
    expect(fog.intensity(1, 1) == 143, "FW-12: the ring blurs into its inner neighbour (9 x 255 / 16, rounded)");
}

// FW-22 (#563): the deployment overlay draws the fogged cells and the unplayable border red, and
// any clear cell the caller reports blocked, and leaves the rest clear.
void test_deployment_overlay() {
    space::FogField fog = field();
    const std::vector<space::FogFieldRevealer> all{{0.0, 0.0, 5000.0}};
    for (int frame = 0; frame < 20; ++frame) fog.advance(all, 1.0);
    const auto centre = (10U * 21U + 10U) * 4U;
    expect(fog.texels()[centre + 3] == 0, "without the overlay a clear cell is transparent");
    fog.set_deployment_overlay(true);
    expect(fog.deployment_overlay() && fog.changed(), "turning the overlay on redraws the texture");
    expect(fog.texels()[0] == 255 && fog.texels()[1] == 0 && fog.texels()[2] == 0 && fog.texels()[3] == 254,
           "FW-22: the fogged border ring takes the reinforcement colour, not the fog's");
    expect(fog.texels()[centre + 3] == 0 && fog.texels()[centre] == 0, "FW-22: a clear cell with no blocker stays clear");
    // A blocked disc around (0, 0), 250 units wide: the cells whose centres lie inside it turn red.
    fog.set_deployment_overlay(true, [](const double x, const double y) { return x * x + y * y <= 250.0 * 250.0; });
    expect(fog.texels()[centre + 3] > 200 && fog.texels()[centre] > 200 && fog.texels()[centre + 1] == 0,
           "FW-22: a cell the caller reports blocked draws in the reinforcement colour");
    const auto far_cell = (10U * 21U + 15U) * 4U;
    expect(fog.texels()[far_cell + 3] == 0, "and a clear cell outside the blocked disc stays clear");
    fog.set_deployment_overlay(false);
    expect(!fog.deployment_overlay() && fog.texels()[0] == 255 && fog.texels()[1] == 255, "turning it off restores the fog's colour");
}

} // namespace

int main() {
    test_looks();
    test_steps_and_table();
    test_layout();
    test_fade_in_and_out();
    test_resume_and_pause();
    test_texture();
    test_cells();
    test_deployment_overlay();
    if (failures != 0) {
        std::cerr << failures << " fog field contract(s) failed\n";
        return 1;
    }
    std::cout << "fog field contracts passed\n";
    return 0;
}

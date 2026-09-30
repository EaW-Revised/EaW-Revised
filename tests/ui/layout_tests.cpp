// UI layout, scaling and font-size contracts (UI-04 #171, UI-04b #195): table
// tests for rules UI-L1 to UI-L5 and UI-F1 to UI-F2, under both the retail
// rules and the aspect-correct rules of decision D4. The rig-capture values
// are measured from the FoC coruscant-space captures of design section 1.4
// (1280x720 and the clamped 1920x1061 client); the captures stay local.
// With EAWR_EAW_GAME_ROOT set, the real tactical shell anchors are placed too.

#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include "ui_test_support.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace layout = presentation::ui;
using data::ui::ReferenceRect;
using layout::LayoutRules;
using layout::Placement;
using layout::PixelRect;
using layout::RcRect;
using layout::ShellPlacement;
using layout::Viewport;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

bool near(const double actual, const double expected, const double tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

std::string name_of(const Viewport viewport) {
    return std::to_string(viewport.width) + "x" + std::to_string(viewport.height);
}

bool rect_near(const PixelRect& actual, const PixelRect& expected, const double tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) &&
           near(actual.width, expected.width, tolerance) && near(actual.height, expected.height, tolerance);
}

layout::ReferenceSpace retail_space(const Viewport viewport) {
    return layout::reference_space(viewport, LayoutRules::retail);
}

// Anchors from the FoC tactical shell (UI-03 corpus values, design section 1.3).
constexpr ReferenceRect planet_name{84.16F, 228.0F, 189.68F, 22.0F};
constexpr ReferenceRect minimap{14.5F, 10.5F, 175.0F, 175.0F};
constexpr ReferenceRect faceplate{-1.0F, -1.0F, 1078.0F, 284.0F};
// Shell extents from the origin to the right edge (mod HUD survey 2.2 and 5.3).
constexpr double foc_tactical_width = 1077.0;        // the faceplate spans -1 to 1077
constexpr double widescreen_tactical_width = 1352.0; // 3689306867's tactical faceplate
constexpr double remake_galactic_width = 1379.0;     // the Remake galactic faceplate

// Planet-name text ink in the rig captures: first and last ink column and row
// ("Coruscant", EmpireAtWar-Bold 10, centred in its rect).
struct InkBox {
    Viewport viewport;
    int left;
    int right;
    int top;
    int bottom;

    [[nodiscard]] double centre_x() const { return (left + right + 1) / 2.0; }
    [[nodiscard]] double centre_y() const { return (top + bottom + 1) / 2.0; }
    [[nodiscard]] int width() const { return right - left + 1; }
};
constexpr std::array<InkBox, 2> planet_ink{{
    {{1280U, 720U}, 120, 214, 491, 500},
    {{1920U, 1061U}, 176, 319, 725, 737},
}};

void reference_space_table() {
    struct Row {
        Viewport viewport;
        bool height_based;
        double width;
        double height;
        double scale;
    };
    constexpr std::array<Row, 9> rows{{
        {{1024U, 768U}, true, 1024.0, 768.0, 1.0},
        {{1280U, 720U}, true, 1365.3333333, 768.0, 0.9375},
        {{1920U, 1080U}, true, 1365.3333333, 768.0, 1.40625},
        {{1280U, 1024U}, false, 1024.0, 819.2, 1.25},
        {{2560U, 1080U}, true, 1820.4444444, 768.0, 1.40625},
        {{3440U, 1440U}, true, 1834.6666667, 768.0, 1.875},
        {{5120U, 1440U}, true, 2730.6666667, 768.0, 1.875},
        {{1920U, 1061U}, true, 1389.7832234, 768.0, 1.3815104},
        {{1024U, 1024U}, false, 1024.0, 1024.0, 1.0},
    }};
    for (const Row& row : rows) {
        const std::string label = "UI-L1 " + name_of(row.viewport);
        for (const LayoutRules rules : {LayoutRules::retail, LayoutRules::aspect_correct}) {
            const auto space = layout::reference_space(row.viewport, rules);
            expect(space.rules == rules, label + ": rules recorded");
            expect(space.height_based == row.height_based, label + ": aspect branch");
            expect(near(space.width, row.width, 1.0e-6) && near(space.height, row.height, 1.0e-6),
                   label + ": reference size");
            expect(near(space.scale, row.scale, 1.0e-6), label + ": pixel scale");
            expect(near(space.width * space.scale, row.viewport.width, 1.0e-6) &&
                       near(space.height * space.scale, row.viewport.height, 1.0e-6),
                   label + ": reference space covers the screen");
        }
        // Retail UI-L1 has no safe area and UI-L2 puts the faceplate on the
        // lower-left corner at every size.
        const auto retail = retail_space(row.viewport);
        expect(retail.safe_left == 0.0 && retail.safe_width == retail.width, label + ": retail safe area is the screen");
        const PixelRect plate = layout::shell_to_screen(faceplate, layout::place_shell(retail, foc_tactical_width));
        expect(near(plate.x, -row.scale, 1.0e-6) && near(plate.bottom(), row.viewport.height + row.scale, 1.0e-4) &&
                   near(plate.width, 1078.0 * row.scale, 1.0e-4),
               label + ": retail faceplate anchored bottom-left, scaled uniformly");
    }
    const auto empty = layout::reference_space({0U, 0U});
    expect(empty.scale == 0.0 && empty.safe_width == 0.0, "UI-L1: an empty viewport has no scale");
    // Exactly 4:3 is height-based; one pixel narrower is width-based.
    expect(layout::reference_space({1024U, 768U}).height_based && !layout::reference_space({1023U, 768U}).height_based,
           "UI-L1: 4:3 boundary");
    // Exactly 16:9 has no side bands; one pixel wider has.
    expect(layout::reference_space({1920U, 1080U}).safe_left == 0.0 && layout::reference_space({1921U, 1080U}).safe_left > 0.0,
           "UI-L1: 16:9 boundary");
}

// Decision D4 (#166) at the acceptance sizes of #195: the safe area, the FoC
// HUD and a 320 x 240 dialog centred in it, and nothing stretched.
void aspect_correct_table() {
    struct Row {
        Viewport viewport;
        double safe_x;     // safe area, screen pixels
        double safe_width;
        double plate_x;    // FoC faceplate left edge
        PixelRect dialog;  // a 320 x 240 dialog, centred
    };
    constexpr std::array<Row, 6> rows{{
        {{1280U, 720U}, 0.0, 1280.0, -0.9375, {490.0, 247.5, 300.0, 225.0}},
        {{1920U, 1080U}, 0.0, 1920.0, -1.40625, {735.0, 371.25, 450.0, 337.5}},
        {{2560U, 1080U}, 320.0, 1920.0, 318.59375, {1055.0, 371.25, 450.0, 337.5}},
        {{3440U, 1440U}, 440.0, 2560.0, 438.125, {1420.0, 495.0, 600.0, 450.0}},
        {{5120U, 1440U}, 1280.0, 2560.0, 1278.125, {2260.0, 495.0, 600.0, 450.0}},
        // Below 4:3 the safe area is the screen and the FoC HUD, wider than
        // 1024 units, keeps the retail corner (OD-1 A).
        {{1280U, 1024U}, 0.0, 1280.0, -1.25, {440.0, 362.0, 400.0, 300.0}},
    }};
    for (const Row& row : rows) {
        const std::string label = "D4 " + name_of(row.viewport);
        const auto space = layout::reference_space(row.viewport);
        const PixelRect area = layout::safe_area(space);
        expect(rect_near(area, {row.safe_x, 0.0, row.safe_width, static_cast<double>(row.viewport.height)}, 1.0e-6),
               label + ": safe area");
        expect(near(area.centre_x(), row.viewport.width / 2.0, 1.0e-6), label + ": safe area centred");
        expect(area.width <= row.viewport.height * 16.0 / 9.0 + 1.0e-6, label + ": safe area at most 16:9");

        const ShellPlacement shell = layout::place_shell(space, foc_tactical_width);
        const PixelRect plate = layout::shell_to_screen(faceplate, shell);
        expect(near(plate.x, row.plate_x, 1.0e-6) && near(plate.bottom(), row.viewport.height + space.scale, 1.0e-4),
               label + ": FoC faceplate at the safe area's lower-left corner");
        expect(near(plate.width / 1078.0, plate.height / 284.0, 1.0e-9), label + ": HUD not stretched");
        if (foc_tactical_width <= space.width) {
            expect(plate.right() <= row.viewport.width + 1.0e-6, label + ": HUD on screen");
        }

        const auto dialog = layout::place_dialog({50, 60, 320, 240}, Placement::centre, space);
        expect(rect_near(dialog.frame, row.dialog, 1.0e-9), label + ": centred 320 x 240 dialog");
        const PixelRect square = layout::scale_rc({0, 0, 100, 100}, space);
        expect(near(square.width, square.height, 1.0e-9) && near(square.width, 100.0 * space.scale, 1.0e-9),
               label + ": .rc units scale uniformly by min(W/1024, H/768)");
        expect(dialog.frame.x >= area.x && dialog.frame.right() <= area.right(), label + ": dialog inside the safe area");
    }

    // The retail flag keeps the corner anchor and the stretched dialog.
    const auto retail = retail_space({2560U, 1080U});
    expect(layout::place_shell(retail, foc_tactical_width) == ShellPlacement{0.0, 1080.0, 1.40625},
           "retail 2560x1080: HUD in the screen corner");
    expect(rect_near(layout::place_dialog({50, 60, 320, 240}, Placement::centre, retail).frame,
                     {880.0, 371.25, 800.0, 337.5}, 1.0e-9),
           "retail 2560x1080: dialog stretched to 800 x 337.5");
    // At 16:9 the two rule sets agree on the HUD, so the rig captures validate both.
    for (const Viewport viewport : {Viewport{1280U, 720U}, Viewport{1920U, 1080U}}) {
        expect(layout::place_shell(layout::reference_space(viewport), foc_tactical_width) ==
                   layout::place_shell(retail_space(viewport), foc_tactical_width),
               "D4 " + name_of(viewport) + ": HUD placement equals retail at 16:9");
    }
}

// OD-1 (#205) option A, and a shell wider than the safe area but not the screen.
void wide_shell_contracts() {
    // A 1352-unit HUD on 4:3 keeps the retail corner and clips on the right.
    for (const Viewport viewport : {Viewport{1024U, 768U}, Viewport{1280U, 1024U}, Viewport{1600U, 1200U}}) {
        const auto space = layout::reference_space(viewport);
        const ShellPlacement shell = layout::place_shell(space, widescreen_tactical_width);
        const std::string label = "OD-1 A " + name_of(viewport);
        expect(shell == layout::overwide_shell(space, widescreen_tactical_width), label + ": placed by overwide_shell");
        expect(shell == ShellPlacement{0.0, static_cast<double>(viewport.height), space.scale},
               label + ": left-anchored at the UI-L1 scale");
        const PixelRect plate = layout::shell_to_screen({0.0F, 0.0F, 1352.0F, 284.0F}, shell);
        expect(plate.right() > viewport.width && near(plate.width / 1352.0, plate.height / 284.0, 1.0e-9),
               label + ": clipped on the right, not scaled");
    }
    // The FoC HUD itself is wider than 4:3 (1077 > 1024), so on 4:3 it keeps
    // the retail corner too.
    const auto four_three = layout::reference_space({1024U, 768U});
    expect(layout::place_shell(four_three, foc_tactical_width) == layout::place_shell(retail_space({1024U, 768U}), foc_tactical_width),
           "OD-1 A 1024x768: the FoC HUD is placed as retail");
    // The 1352-unit HUD fits 16:9 (1365.33 units) and sits in the safe area.
    const auto hd = layout::reference_space({1280U, 720U});
    const PixelRect wide_hd = layout::shell_to_screen({0.0F, 0.0F, 1352.0F, 284.0F}, layout::place_shell(hd, widescreen_tactical_width));
    expect(wide_hd.x == 0.0 && near(wide_hd.right(), 1267.5, 1.0e-9), "1352-unit HUD fits 1280x720");
    const auto ultrawide = layout::reference_space({2560U, 1080U});
    expect(near(layout::place_shell(ultrawide, widescreen_tactical_width).left, 320.0, 1.0e-6),
           "1352-unit HUD at the safe area's left on 2560x1080");
    // The 1379-unit Remake galactic faceplate is wider than the 16:9 safe area
    // but not than the 1920x1061 screen: it moves left until its right edge
    // meets the screen's.
    const auto clamped = layout::reference_space({1920U, 1061U});
    const ShellPlacement galactic = layout::place_shell(clamped, remake_galactic_width);
    expect(galactic.left < layout::safe_area(clamped).x && galactic.left > 0.0,
           "1379-unit shell left of the safe area on 1920x1061");
    expect(near(galactic.left + remake_galactic_width * galactic.scale, 1920.0, 1.0e-6),
           "1379-unit shell ends at the screen's right edge");
    expect(near(layout::place_shell(clamped, foc_tactical_width).left, layout::safe_area(clamped).x, 1.0e-9),
           "the FoC HUD at the safe area's left on 1920x1061");
}

void shell_contracts() {
    // Rig-capture checks: the originals follow the retail rules.
    const auto hd = retail_space({1280U, 720U});
    const ShellPlacement hd_shell = layout::place_shell(hd, foc_tactical_width);
    const PixelRect radar = layout::shell_to_screen(minimap, hd_shell);
    expect(rect_near(radar, {13.59375, 720.0 - 185.5 * 0.9375, 164.0625, 164.0625}, 1.0e-4),
           "UI-L2: minimap rect at 1280x720");
    // Design section 1.4: minimap left edge 13 px at 1280x720 and about 20 px at 1920x1061.
    expect(near(radar.x, 13.0, 1.0), "UI-L2: minimap left edge matches the 1280x720 capture");
    const auto clamped = retail_space({1920U, 1061U});
    expect(near(layout::shell_to_screen(minimap, layout::place_shell(clamped, foc_tactical_width)).x, 20.0, 1.0),
           "UI-L2: minimap left edge matches the 1920x1061 capture");

    for (const InkBox& ink : planet_ink) {
        const auto space = retail_space(ink.viewport);
        const PixelRect rect = layout::shell_to_screen(planet_name, layout::place_shell(space, foc_tactical_width));
        const std::string label = "planet name " + name_of(ink.viewport);
        expect(near(rect.centre_x(), ink.centre_x(), 1.0) && near(rect.centre_y(), ink.centre_y(), 1.0),
               label + ": rect centre within 1 px of the capture");
        expect(rect.x <= ink.left && rect.right() >= ink.right + 1 && rect.y <= ink.top && rect.bottom() >= ink.bottom + 1,
               label + ": the text lies inside the rect");
    }
    const PixelRect at_hd = layout::shell_to_screen(planet_name, hd_shell);
    expect(rect_near(at_hd, {78.9, 485.625, 177.825, 20.625}, 1.0e-3), "UI-L2: planet-name rect at 1280x720");
    // Aspect-correct 1920x1061 is a little wider than 16:9: the HUD moves
    // right by the safe area's side band, 12.22 units.
    const auto clamped_d4 = layout::reference_space({1920U, 1061U});
    const PixelRect radar_d4 = layout::shell_to_screen(minimap, layout::place_shell(clamped_d4, foc_tactical_width));
    expect(near(radar_d4.x, (12.2249450 + 14.5) * clamped_d4.scale, 1.0e-4), "D4: minimap on 1920x1061");

    // UI-L5: offsets snap to whole reference units.
    expect(layout::snap_shell_offset({0.5, -0.5}) == layout::ReferencePoint{1.0, -1.0}, "UI-L5: halves round away from zero");
    expect(layout::snap_shell_offset({2.4, 2.6}) == layout::ReferencePoint{2.0, 3.0}, "UI-L5: nearest whole unit");
    expect(layout::shell_to_screen(minimap, hd_shell, {0.4, 0.6}) == layout::shell_to_screen(minimap, hd_shell, {0.0, 1.0}),
           "UI-L5: a fractional shell offset lands on whole units");
    const PixelRect moved = layout::shell_to_screen(minimap, hd_shell, {10.0, 20.0});
    expect(near(moved.x - radar.x, 10.0 * 0.9375, 1.0e-9) && near(radar.y - moved.y, 20.0 * 0.9375, 1.0e-9),
           "UI-L2: the shell offset moves right and up in reference units");
}

void retail_dialog_contracts() {
    // Retail UI-L3 against the predicted sizes in design section 1.4.
    const PixelRect skirmish_hd = layout::scale_rc({0, 0, 965, 422}, retail_space({1280U, 720U}));
    expect(near(skirmish_hd.width, 1206.25, 1.0e-9) && near(skirmish_hd.height, 395.625, 1.0e-9),
           "retail UI-L3: skirmish dialog 965 x 422 at 1280x720 is 1206 x 396");
    const PixelRect skirmish_fhd = layout::scale_rc({0, 0, 965, 422}, retail_space({1920U, 1061U}));
    expect(near(skirmish_fhd.width, 1809.375, 1.0e-9) && near(skirmish_fhd.height, 583.0, 0.5),
           "retail UI-L3: skirmish dialog at 1920x1061 is 1809 x 583");
    const PixelRect gadget = layout::scale_rc({8, 16, 100, 14}, retail_space({2560U, 1080U}));
    expect(rect_near(gadget, {20.0, 22.5, 250.0, 19.6875}, 1.0e-9), "retail UI-L3: x by W/1024 and y by H/768 separately");

    // Retail UI-L4 preset table at 1280x720: a 320 x 240 dialog is 400 x 225
    // px; margins 10.24 and 4.32 px; frame border 5 (left) and 7 (top).
    // Upper-right follows the FoC debug build's dialog creation: zero margin
    // on both edges, flush right, but the top frame border is kept (y = 7).
    const auto hd = retail_space({1280U, 720U});
    const layout::FrameBorder border{5.0, 7.0};
    struct Row {
        Placement placement;
        double x;
        double y;
        const char* name;
    };
    constexpr std::array<Row, 9> rows{{
        {Placement::centre, 440.0, 247.5, "centre"},
        {Placement::upper_left, 15.24, 11.32, "upper-left"},
        {Placement::centre_left, 15.24, 247.5, "centre-left"},
        {Placement::lower_left, 15.24, 490.68, "lower-left"},
        {Placement::upper_right, 880.0, 7.0, "upper-right"},
        {Placement::centre_right, 869.76, 247.5, "centre-right"},
        {Placement::lower_right, 869.76, 490.68, "lower-right"},
        {Placement::upper_centre, 440.0, 30.22, "upper-centre"},
        {Placement::lower_centre, 440.0, 490.68, "lower-centre"},
    }};
    for (const Row& row : rows) {
        const auto placed = layout::place_dialog({50, 60, 320, 240}, row.placement, hd, border);
        expect(rect_near(placed.frame, {row.x, row.y, 400.0, 225.0}, 1.0e-9), std::string("retail UI-L4 ") + row.name);
        expect(placed.origin_x == placed.frame.x && placed.origin_y == placed.frame.y,
               std::string("retail UI-L4 ") + row.name + ": gadgets keep their .rc offsets");
    }
    const auto options = layout::place_dialog({0, 0, 321, 328}, Placement::centre, hd);
    expect(rect_near(options.frame, {439.375, 206.25, 401.25, 307.5}, 1.0e-9),
           "retail UI-L3/UI-L4: IDD_GAME_OPTIONS_DIALOG 321 x 328 centred at 1280x720 (401 x 308)");
    const PixelRect button = layout::place_gadget(options, {10, 20, 100, 14});
    expect(rect_near(button, {451.875, 225.0, 125.0, 13.125}, 1.0e-9), "retail UI-L4: gadgets follow their dialog");

    const auto full = layout::place_dialog({0, 0, 320, 240}, Placement::full_screen, hd, border);
    expect(rect_near(full.frame, {0.0, 0.0, 1280.0, 720.0}, 0.0), "retail UI-L4: full screen covers the viewport");
    expect(near(full.origin_x, 352.0 * 1.25, 1.0e-9) && near(full.origin_y, 264.0 * 0.9375, 1.0e-9),
           "retail UI-L4: full-screen gadgets recentre in 1024 x 768");
    expect(rect_near(layout::place_gadget(full, {10, 20, 100, 14}), {452.5, 266.25, 125.0, 13.125}, 1.0e-9),
           "retail UI-L4: a recentred full-screen gadget");
    const auto exact = layout::place_dialog({0, 0, 1024, 768}, Placement::full_screen, hd);
    expect(near(exact.origin_x, 0.0, 1.0e-9) && near(exact.origin_y, 0.0, 1.0e-9),
           "retail UI-L4: a 1024 x 768 dialog needs no recentring");
}

void aspect_correct_dialog_contracts() {
    // UI-L4 relative to the safe area at 2560x1080: the safe area is x 320,
    // 1920 x 1080; a 320 x 240 dialog is 450 x 337.5 px; margins 15.36 and
    // 6.48 px; frame border 5 (left) and 7 (top).
    const auto ultrawide = layout::reference_space({2560U, 1080U});
    const layout::FrameBorder border{5.0, 7.0};
    struct Row {
        Placement placement;
        double x;
        double y;
        const char* name;
    };
    constexpr std::array<Row, 9> rows{{
        {Placement::centre, 1055.0, 371.25, "centre"},
        {Placement::upper_left, 340.36, 13.48, "upper-left"},
        {Placement::centre_left, 340.36, 371.25, "centre-left"},
        {Placement::lower_left, 340.36, 736.02, "lower-left"},
        {Placement::upper_right, 1790.0, 7.0, "upper-right"},
        {Placement::centre_right, 1774.64, 371.25, "centre-right"},
        {Placement::lower_right, 1774.64, 736.02, "lower-right"},
        {Placement::upper_centre, 1055.0, 41.83, "upper-centre"},
        {Placement::lower_centre, 1055.0, 736.02, "lower-centre"},
    }};
    for (const Row& row : rows) {
        const auto placed = layout::place_dialog({50, 60, 320, 240}, row.placement, ultrawide, border);
        expect(rect_near(placed.frame, {row.x, row.y, 450.0, 337.5}, 1.0e-9), std::string("D4 UI-L4 2560x1080 ") + row.name);
    }
    // 1280x720 is 16:9, so the positions follow the retail margins; only the
    // size is uniform (300 x 225, not 400 x 225).
    const auto hd = layout::reference_space({1280U, 720U});
    expect(rect_near(layout::place_dialog({0, 0, 320, 240}, Placement::upper_left, hd, border).frame,
                     {15.24, 11.32, 300.0, 225.0}, 1.0e-9),
           "D4 UI-L4 1280x720 upper-left");
    const auto options = layout::place_dialog({0, 0, 321, 328}, Placement::centre, hd);
    expect(rect_near(options.frame, {489.53125, 206.25, 300.9375, 307.5}, 1.0e-9),
           "D4 UI-L3/UI-L4: IDD_GAME_OPTIONS_DIALOG 321 x 328 at 1280x720 is 301 x 308");
    expect(rect_near(layout::place_gadget(options, {10, 20, 100, 14}), {498.90625, 225.0, 93.75, 13.125}, 1.0e-9),
           "D4 UI-L4: gadgets scale uniformly with their dialog");

    // Full screen: the frame covers the viewport and the 1024 x 768 gadget
    // area is centred in the safe area at the uniform scale.
    const auto full = layout::place_dialog({0, 0, 320, 240}, Placement::full_screen, ultrawide, border);
    expect(rect_near(full.frame, {0.0, 0.0, 2560.0, 1080.0}, 0.0), "D4 UI-L4 2560x1080: full screen covers the viewport");
    expect(rect_near(layout::place_gadget(full, {10, 20, 100, 14}), {1069.0625, 399.375, 140.625, 19.6875}, 1.0e-9),
           "D4 UI-L4 2560x1080: full-screen gadget in the centred 1024 x 768 area");
    const auto tall = layout::reference_space({1280U, 1024U});
    const auto tall_full = layout::place_dialog({0, 0, 320, 240}, Placement::full_screen, tall);
    expect(near(tall_full.origin_x, 440.0, 1.0e-9) && near(tall_full.origin_y, 362.0, 1.0e-9),
           "D4 UI-L4 1280x1024: the gadget area is centred vertically");
    expect(near(tall_full.origin_x, layout::place_dialog({0, 0, 320, 240}, Placement::centre, tall).frame.x, 1.0e-9),
           "D4 UI-L4 1280x1024: full-screen gadgets sit where the centred dialog does");
}

void font_contracts() {
    // UI-F1 table: 7 pt and 10 pt at every tested height.
    struct Row {
        std::uint32_t height;
        std::int32_t seven;
        std::int32_t ten;
    };
    constexpr std::array<Row, 7> rows{{
        {768U, 11, 16},   // 1024x768: floor(122.88) = 122
        {720U, 11, 15},   // 1280x720: 115
        {1080U, 16, 23},  // 1920x1080 and 2560x1080: 172
        {1024U, 15, 22},  // 1280x1024: 163
        {1061U, 16, 23},  // the rig's clamped client: 169
        {1440U, 22, 31},  // 3440x1440 and 5120x1440: 230
        {960U, 14, 21},   // 1280x1024 aspect-correct: 153
    }};
    for (const Row& row : rows) {
        const std::string label = "UI-F1 H " + std::to_string(row.height);
        expect(layout::font_pixel_height(7, row.height) == row.seven, label + ": 7 pt");
        expect(layout::font_pixel_height(10, row.height) == row.ten, label + ": 10 pt");
    }
    expect(layout::font_pixel_height(7, 720U) == 11 && layout::font_pixel_height(7, 1080U) == 16,
           "UI-F1: 7 pt is 11 px at 720p and 16 px at 1080p");
    expect(layout::font_pixel_height(7, 1080U, true) == 7, "UI-F1: a static-size font uses pt as pixels");
    expect(layout::font_pixel_height(0, 1080U) == 0 && layout::font_pixel_height(-3, 1080U) == 0,
           "UI-F1: non-positive sizes are empty");
    // Design section 1.4: the planet name (EmpireAtWar-Bold 10) is 95 px wide at
    // 1280x720 and 144 px at 1920x1061; the width follows the em (15 -> 23 px).
    const double predicted = planet_ink[0].width() * static_cast<double>(layout::font_pixel_height(10, 1061U)) /
                             layout::font_pixel_height(10, 720U);
    expect(near(predicted, planet_ink[1].width(), 2.0), "UI-F1: planet-name width scales with the em height");

    // The H UI-F1 takes: the screen height, except aspect-correct below 4:3.
    struct HeightRow {
        Viewport viewport;
        std::uint32_t aspect_correct;
        std::uint32_t retail;
    };
    constexpr std::array<HeightRow, 7> heights{{
        {{1280U, 720U}, 720U, 720U},
        {{1920U, 1080U}, 1080U, 1080U},
        {{2560U, 1080U}, 1080U, 1080U},
        {{3440U, 1440U}, 1440U, 1440U},
        {{5120U, 1440U}, 1440U, 1440U},
        {{1280U, 1024U}, 960U, 1024U},
        {{0U, 1024U}, 0U, 0U},
    }};
    for (const HeightRow& row : heights) {
        expect(layout::font_screen_height(layout::reference_space(row.viewport)) == row.aspect_correct &&
                   layout::font_screen_height(retail_space(row.viewport)) == row.retail,
               "UI-F1 font height at " + name_of(row.viewport));
    }

    // UI-F2 metric test on a synthetic face whose average width is 45 % of the em.
    const auto average_width = [](const std::int32_t em) { return em * 45 / 100; };
    const auto stretched = layout::font_pixels({7, false, 1.3}, 1080U);
    expect(stretched.em_height == 16 && stretched.glyph_height == 21 && stretched.width_em == 16,
           "UI-F2: Stretch_Factor 1.3 on 16 px glyphs is 21 px high");
    expect(average_width(stretched.width_em) == average_width(layout::font_pixels({7, false, 1.0}, 1080U).width_em),
           "UI-F2: the average width stays that of the unstretched face");
    expect(layout::font_pixels({10, false, 0.25}, 720U).glyph_height == 4, "UI-F2: 15 x 0.25 rounds to 4");
    expect(layout::font_pixels({11, true, 1.5}, 720U).glyph_height == 17, "UI-F2: 16.5 rounds half away from zero");
    const auto plain = layout::font_pixels({9, false, 1.0}, 720U);
    expect(plain.glyph_height == plain.em_height && plain.width_em == plain.em_height, "UI-F2: factor 1 changes nothing");
}

void corpus_contracts() {
    auto filesystem = test::ui::foc_corpus("layout");
    if (!filesystem) return;
    auto loaded = data::ui::load_shell_anchors(*filesystem, "i_tactical_controls.alo");
    expect(static_cast<bool>(loaded), "layout corpus: tactical shell loads");
    if (!loaded) return;
    const auto* anchor = loaded.value().shell.find("Text_Planet_tactical");
    expect(anchor != nullptr, "layout corpus: planet-name anchor exists");
    if (!anchor) return;
    for (const InkBox& ink : planet_ink) {
        const ShellPlacement shell = layout::place_shell(retail_space(ink.viewport), foc_tactical_width);
        const PixelRect rect = layout::shell_to_screen(anchor->rect, shell);
        std::cout << "layout corpus: planet name at " << name_of(ink.viewport) << " centre (" << rect.centre_x() << ", "
                  << rect.centre_y() << "), capture ink centre (" << ink.centre_x() << ", " << ink.centre_y() << ")\n";
        expect(near(rect.centre_x(), ink.centre_x(), 1.0) && near(rect.centre_y(), ink.centre_y(), 1.0),
               "layout corpus: real planet-name rect within 1 px of the capture at " + name_of(ink.viewport));
    }
}

} // namespace

void layout_contracts() {
    reference_space_table();
    aspect_correct_table();
    wide_shell_contracts();
    shell_contracts();
    retail_dialog_contracts();
    aspect_correct_dialog_contracts();
    font_contracts();
    corpus_contracts();
}

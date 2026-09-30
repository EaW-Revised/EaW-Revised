// #82 P2-19: battle picking, click, box, type and control-group selection, the tactical overview,
// and the replay contract that selection never reaches the simulation. The Godot half (events,
// drag box, brackets) runs in the viewer (tests/presentation/renderer/test_battle_input.py).

#include "eawr/presentation/camera/overview.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace camera = presentation::camera;
namespace tactical = sim::tactical;
using test::ui::expect;

constexpr ui::ScreenRect viewport{0.0F, 0.0F, 1280.0F, 720.0F};

bool close_to(const float a, const float b, const float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }

// A unit box of half size `half` at `at`, turned `yaw` degrees about +Z.
ui::UnitBox box_at(const ui::Vec3f at, const float half, const float yaw_degrees = 0.0F, const float scale = 1.0F) {
    const float radians = yaw_degrees * 3.14159265358979323846F / 180.0F;
    const float c = std::cos(radians) * scale;
    const float s = std::sin(radians) * scale;
    ui::UnitBox box;
    box.model_to_world = {c, -s, 0.0F, at[0], s, c, 0.0F, at[1], 0.0F, 0.0F, scale, at[2]};
    box.low = {-half, -half * 0.5F, -half * 0.25F};
    box.high = {half, half * 0.5F, half * 0.25F};
    return box;
}

ui::BattleUnit unit(const sim::EntityId entity, const tactical::TypeId type, const bool own, const ui::Vec3f at,
                    const std::optional<std::array<float, 2>> screen, const bool hostile = false) {
    ui::BattleUnit result;
    result.entity = entity;
    result.type = type;
    result.own = own;
    result.hostile = hostile;
    result.box = box_at(at, 10.0F);
    result.position = at;
    result.screen = screen;
    return result;
}

// Straight down from above the point.
ui::PickRay down_at(const float x, const float y) { return {{x, y, 500.0F}, {0.0F, 0.0F, -1.0F}}; }

void picking() {
    // A 20 x 10 x 5 box turned 90 degrees: its long side runs along Y.
    const ui::UnitBox turned = box_at({100.0F, 0.0F, 0.0F}, 10.0F, 90.0F);
    expect(ui::ray_box_contact(down_at(100.0F, 9.0F), turned).has_value(), "the ray hits the turned box's long side");
    expect(!ui::ray_box_contact(down_at(109.0F, 0.0F), turned), "and misses beyond its short side");
    const auto top = ui::ray_box_contact(down_at(100.0F, 0.0F), turned);
    expect(top && close_to((*top)[2], 2.5F), "the contact is the box's top face");
    const ui::UnitBox scaled = box_at({0.0F, 0.0F, 0.0F}, 10.0F, 0.0F, 2.0F);
    expect(ui::ray_box_contact(down_at(19.0F, 0.0F), scaled).has_value(), "the model scale grows the box");
    const ui::PickRay slanted{{-100.0F, 0.0F, 100.0F}, {1.0F, 0.0F, -1.0F}};
    const auto side = ui::ray_box_contact(slanted, box_at({0.0F, 0.0F, 0.0F}, 10.0F));
    expect(side && close_to((*side)[0], -2.5F) && close_to((*side)[2], 2.5F), "a slanted ray enters through the top face");
    expect(!ui::ray_box_contact({{0.0F, 0.0F, 500.0F}, {0.0F, 0.0F, 1.0F}}, box_at({0.0F, 0.0F, 0.0F}, 10.0F)),
           "a box behind the ray's origin is not hit");

    // Stacked units: FoC keeps the highest contact.
    std::vector<ui::BattleUnit> units{unit(4, 1, true, {0.0F, 0.0F, 0.0F}, std::nullopt),
                                      unit(9, 1, false, {0.0F, 0.0F, 30.0F}, std::nullopt, true),
                                      unit(2, 1, true, {50.0F, 0.0F, 0.0F}, std::nullopt)};
    expect(ui::pick_unit(down_at(0.0F, 0.0F), units) == sim::EntityId{9}, "the highest contact wins");
    expect(ui::pick_unit(down_at(50.0F, 0.0F), units) == sim::EntityId{2}, "a lone unit is picked");
    expect(!ui::pick_unit(down_at(300.0F, 0.0F), units), "empty space picks nothing");

    const auto plane = ui::battle_plane_point({{10.0F, 20.0F, 100.0F}, {1.0F, 0.0F, -2.0F}});
    expect(plane && close_to((*plane)[0], 60.0F) && close_to((*plane)[1], 20.0F) && (*plane)[2] == 0.0F,
           "the order point is where the ray meets Z = 0");
    expect(!ui::battle_plane_point({{0.0F, 0.0F, 10.0F}, {1.0F, 0.0F, 0.0F}}), "a ray along the plane has none");
}

std::vector<ui::BattleUnit> fleet() {
    return {unit(1, 7, true, {0.0F, 0.0F, 0.0F}, std::array{100.0F, 100.0F}),
            unit(2, 7, true, {40.0F, 0.0F, 0.0F}, std::array{200.0F, 100.0F}),
            unit(3, 8, true, {80.0F, 0.0F, 0.0F}, std::array{300.0F, 100.0F}),
            unit(4, 7, true, {120.0F, 0.0F, 0.0F}, std::array{1500.0F, 100.0F}),  // off screen
            unit(5, 7, true, {160.0F, 0.0F, 0.0F}, std::nullopt),                 // behind the camera
            unit(10, 7, false, {0.0F, 90.0F, 0.0F}, std::array{150.0F, 400.0F}, true)};
}

void clicks_and_boxes() {
    const auto units = fleet();
    ui::Selection selection;
    expect(selection.click(sim::EntityId{1}, {}, units, viewport) && selection.units() == std::vector<sim::EntityId>{1},
           "a click selects an own unit");
    expect(selection.click(sim::EntityId{3}, ui::Modifiers{true, false, false}, units, viewport)
               && selection.units() == std::vector<sim::EntityId>{1, 3}, "Shift adds");
    expect(selection.click(sim::EntityId{1}, ui::Modifiers{true, false, false}, units, viewport)
               && selection.units() == std::vector<sim::EntityId>{3}, "Shift on a selected unit removes it");
    expect(!selection.click(sim::EntityId{10}, {}, units, viewport) && selection.units() == std::vector<sim::EntityId>{3},
           "an enemy is never selected and keeps the selection");
    expect(selection.click(sim::EntityId{2}, ui::Modifiers{false, true, false}, units, viewport)
               && selection.units() == std::vector<sim::EntityId>{3, 1, 2},
           "Ctrl adds every own unit of the type on screen, not those off screen or behind the camera");
    expect(selection.click(std::nullopt, {}, units, viewport) && selection.empty(), "empty space clears");
    expect(!selection.click(std::nullopt, {}, units, viewport), "and then has nothing to clear");

    expect(selection.double_click(sim::EntityId{1}, units, viewport)
               && selection.units() == std::vector<sim::EntityId>{1, 2}, "a double click selects the type on screen");
    expect(!selection.double_click(sim::EntityId{10}, units, viewport), "not on an enemy");

    selection.clear();
    selection.click(sim::EntityId{3}, {}, units, viewport);
    expect(!selection.box({400.0F, 0.0F, 600.0F, 50.0F}, false, units)
               && selection.units() == std::vector<sim::EntityId>{3}, "an empty box keeps the selection");
    expect(selection.box({50.0F, 50.0F, 250.0F, 500.0F}, false, units)
               && selection.units() == std::vector<sim::EntityId>{1, 2}, "a box replaces it with the own units inside");
    expect(selection.box({250.0F, 50.0F, 350.0F, 150.0F}, true, units)
               && selection.units() == std::vector<sim::EntityId>{1, 2, 3}, "Shift adds the box");

    const auto rect = ui::drag_rect({300.0F, 50.0F}, {100.0F, 250.0F});
    expect(rect.min_x == 100.0F && rect.max_x == 300.0F && rect.min_y == 50.0F && rect.max_y == 250.0F,
           "a drag spans its rectangle whichever way it goes");
    expect(ui::drag_extent({0.0F, 0.0F}, {30.0F, -120.0F}) == 120.0F, "a drag is measured by its larger side");
    expect(ui::minimum_drag_select_distance == 100.0F && ui::minimum_drag_distance == 4.0F,
           "GameConstants.xml drag distances");

    selection.retain(std::vector<sim::EntityId>{2, 3});
    expect(selection.units() == std::vector<sim::EntityId>{2, 3}, "destroyed units leave the selection");
}

void control_groups() {
    auto units = fleet();
    ui::Selection selection;
    selection.box({0.0F, 0.0F, 250.0F, 150.0F}, false, units);  // 1, 2
    selection.assign_group(1);
    selection.click(sim::EntityId{3}, {}, units, viewport);
    selection.assign_group(2);
    expect(selection.group(1) == std::vector<sim::EntityId>{1, 2} && selection.group(2) == std::vector<sim::EntityId>{3},
           "Ctrl+n stores the selection");
    expect(!selection.recall_group(1, false, 10.0, units) && selection.units() == std::vector<sim::EntityId>{1, 2},
           "n selects the group");
    expect(!selection.recall_group(2, true, 10.2, units) && selection.units() == std::vector<sim::EntityId>{1, 2, 3},
           "Shift+n adds it");
    const auto focus = selection.recall_group(2, false, 10.9, units);
    expect(focus && close_to((*focus)[0], 80.0F) && close_to((*focus)[1], 0.0F),
           "the same group again within a second focuses the camera on it");
    expect(!selection.recall_group(2, false, 12.0, units), "a second later it only selects");
    expect(!selection.recall_group(1, false, 12.5, units), "another group does not focus");

    // A unit is in one group at most.
    selection.clear();
    selection.click(sim::EntityId{2}, {}, units, viewport);
    selection.assign_group(3);
    expect(selection.group(1) == std::vector<sim::EntityId>{1} && selection.group(3) == std::vector<sim::EntityId>{2},
           "assigning takes the unit out of its old group");
    const auto joined = selection.add_to_group(1, 20.0, units);
    expect(!joined && selection.group(1) == std::vector<sim::EntityId>{1, 2} && selection.group(3).empty()
               && selection.units() == std::vector<sim::EntityId>{2, 1}, "Alt+n adds the selection and selects the group");

    // Destroyed members: not selected, not counted in the focus.
    units.erase(units.begin());  // unit 1 is gone
    selection.recall_group(1, false, 30.0, units);
    const auto focus_survivor = selection.recall_group(1, false, 30.5, units);
    expect(selection.units() == std::vector<sim::EntityId>{2} && focus_survivor && close_to((*focus_survivor)[0], 40.0F),
           "only standing members are selected and focused");
}

void overview() {
    // FoC Space_Mode: clicks 10 in 1.8 s, distances 2200 and 2900, FOV 60 and 70, Pitch2 80, no Pitch.
    const std::string xml =
        "<TacticalCameras><TacticalCamera Name=\"Space_Mode\">"
        "<Tactical_Overview_Click_Time>1.8</Tactical_Overview_Click_Time>"
        "<Tactical_Overview_Clicks>10</Tactical_Overview_Clicks>"
        "<Tactical_Overview_Distance>2200.0</Tactical_Overview_Distance>"
        "<Tactical_Overview_Distance2>2900.0</Tactical_Overview_Distance2>"
        "<Tactical_Overview_FOV>60.0</Tactical_Overview_FOV><Tactical_Overview_FOV2>70.0</Tactical_Overview_FOV2>"
        "<Tactical_Overview_Pitch2>80.0</Tactical_Overview_Pitch2>"
        "</TacticalCamera></TacticalCameras>";
    const auto bytes = std::as_bytes(std::span(xml.data(), xml.size()));
    const auto loaded = camera::load_overview_constants({bytes, "data/xml/tacticalcameras.xml", ""}, camera::Mode::space);
    expect(static_cast<bool>(loaded), "the Space_Mode overview tags load");
    if (!loaded) return;
    const camera::OverviewConstants c = loaded.value();
    expect(c.clicks == 10 && close_to(c.click_time, 1.8F) && close_to(c.distance, 2200.0F) && close_to(c.distance2, 2900.0F)
               && close_to(c.fov, 60.0F) && close_to(c.pitch2, 80.0F) && close_to(c.pitch, 62.0F),
           "values as authored; the absent pitch keeps FoC's constructor 62");
    const std::string bad = "<TacticalCameras><TacticalCamera Name=\"Space_Mode\">"
                            "<Tactical_Overview_Clicks>2.5</Tactical_Overview_Clicks></TacticalCamera></TacticalCameras>";
    expect(!camera::load_overview_constants({std::as_bytes(std::span(bad.data(), bad.size())), "x.xml", ""},
                                            camera::Mode::space), "a fractional click count is refused");

    const std::string land_xml = "<TacticalCameras><TacticalCamera Name=\"Land_Mode\">"
                                 "<Tactical_Overview_Clicks>4</Tactical_Overview_Clicks>"
                                 "</TacticalCamera></TacticalCameras>";
    const auto land = camera::load_overview_constants(
        {std::as_bytes(std::span(land_xml.data(), land_xml.size())), "data/xml/tacticalcameras.xml", ""},
        camera::Mode::land);
    expect(land && land.value().clicks == 4, "land keeps FoC's four-click overview count");

    // #413: the project space configs supply five; the FoC XML baseline above stays ten.
    camera::OverviewConstants project = c;
    project.clicks = 5;
    camera::TacticalOverview project_view(project);
    for (int click = 0; click < 4; ++click) project_view.wheel(1.0F, 1900.0F, 1900.0F, click * 0.05);
    expect(project_view.level() == camera::OverviewLevel::off && project_view.pending_clicks() == 4,
           "four space clicks at maximum do not enter the overview");
    project_view.wheel(1.0F, 1900.0F, 1900.0F, 0.2);
    expect(project_view.level() == camera::OverviewLevel::overview,
           "five space clicks enter the overview");
    for (int click = 0; click < 4; ++click) project_view.wheel(1.0F, 1900.0F, 1900.0F, 0.25 + click * 0.05);
    expect(project_view.level() == camera::OverviewLevel::overview && project_view.pending_clicks() == 4,
           "four more space clicks do not enter the map overview");
    project_view.wheel(1.0F, 1900.0F, 1900.0F, 0.45);
    expect(project_view.level() == camera::OverviewLevel::map,
           "five more space clicks enter the map overview");

    camera::TacticalOverview view(c);
    constexpr float max = 1900.0F;
    // Zooming out in range resets the count; clicks at the maximum within the settle time do not count.
    expect(view.wheel(1.0F, 1500.0F, max, 0.0), "in range the wheel zooms the tactical camera");
    view.wheel(1.0F, max, max, 0.5);
    expect(view.pending_clicks() == 0, "clicks right after the zoom reached the maximum do not count");
    for (int click = 0; click < 9; ++click) view.wheel(1.0F, max, max, 1.0 + click * 0.05);
    expect(view.level() == camera::OverviewLevel::off && view.pending_clicks() == 9, "nine clicks are not enough");
    view.advance(1.9);
    expect(view.pending_clicks() == 0, "the count restarts after Tactical_Overview_Click_Time");
    for (int click = 0; click < 10; ++click) view.wheel(1.0F, max, max, 4.0 + click * 0.05);
    expect(view.level() == camera::OverviewLevel::overview, "ten clicks at the maximum enter the overview");
    expect(!view.wheel(1.0F, max, max, 5.0), "the tactical camera no longer zooms");
    for (int click = 0; click < 9; ++click) view.wheel(1.0F, max, max, 5.1);
    expect(view.level() == camera::OverviewLevel::map, "ten more enter the map overview");
    view.wheel(-1.0F, max, max, 6.0);
    expect(view.level() == camera::OverviewLevel::overview, "one click in steps back to the overview");
    view.wheel(-1.0F, max, max, 6.1);
    expect(view.level() == camera::OverviewLevel::off, "and one more leaves it");
    view.toggle();
    view.toggle();
    expect(view.level() == camera::OverviewLevel::map, "the overview key cycles overview and map");
    view.toggle();
    expect(view.level() == camera::OverviewLevel::off && view.transitions() == 7, "and back off");

    camera::TacticalFrame tactical;
    tactical.width = 1280;
    tactical.height = 720;
    tactical.vertical_fov_degrees = 50.0F;
    tactical.near_plane = 10.0F;
    tactical.far_plane = 3000.0F;
    tactical.target = {100.0F, 0.0F, -200.0F};
    tactical.eye = {100.0F, 900.0F, 800.0F};
    const auto off = view.frame(tactical, 30.0F, 6100.0F);
    expect(off && off.value() == tactical, "off: the tactical frame is drawn");
    view.toggle();
    const auto first = view.frame(tactical, 30.0F, 6100.0F);
    const auto length = [](const camera::TacticalFrame& frame) {
        const float x = frame.eye[0] - frame.target[0], y = frame.eye[1] - frame.target[1], z = frame.eye[2] - frame.target[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    expect(first && close_to(length(first.value()), 2200.0F, 0.5F) && close_to(first.value().vertical_fov_degrees,
               2.0F * std::atan(0.75F * std::tan(30.0F * 3.14159265F / 180.0F)) * 180.0F / 3.14159265F)
               && first.value().target == tactical.target && first.value().far_plane >= 4400.0F,
           "the overview frame: Tactical_Overview_Distance and _FOV around the same target");
    expect(first && close_to(first.value().eye[1] - tactical.target[1], 2200.0F * std::sin(62.0F * 3.14159265F / 180.0F), 0.5F),
           "at Tactical_Overview_Pitch");
    view.toggle();
    const auto map = view.frame(tactical, 30.0F, 6100.0F);
    expect(map && close_to(length(map.value()), 2900.0F, 0.5F), "the map overview: Tactical_Overview_Distance2");
    const auto capped = view.frame(tactical, 30.0F, 1000.0F);
    // #515: Tactical_Overview_FOV2 70 as its 4:3 vertical angle (tangent 3/4 of the XML one's).
    expect(capped && close_to(length(capped.value()), 1000.0F / (0.75F * std::tan(35.0F * 3.14159265F / 180.0F)), 0.5F),
           "capped so a small map's half extent fits Tactical_Overview_FOV2");
    expect(map && close_to(map.value().eye[0], tactical.target[0], 0.5F), "at yaw 0");
}

// #350: FoC's Enter_Tactical_Overview_Map_Mode sets the camera's own yaw to 0; the overview entered
// from the map keeps it; leaving the overview restores the saved tactical yaw.
void overview_yaw() {
    const camera::OverviewConstants c{2200.0F, 2900.0F, 62.0F, 80.0F, 60.0F, 70.0F, 10U, 1.8F};
    camera::TacticalOverview view(c);
    expect(!view.view_yaw(30.0F), "off: no overview yaw");
    view.toggle();
    expect(view.view_yaw(30.0F) == 30.0F, "the overview keeps the tactical yaw");
    view.toggle();
    expect(view.view_yaw(30.0F) == 0.0F, "the map overview draws yaw 0");
    constexpr float max = 1900.0F;
    view.wheel(-1.0F, max, max, 0.0);
    expect(view.level() == camera::OverviewLevel::overview && view.view_yaw(30.0F) == 0.0F,
           "one click in: the overview keeps yaw 0");
    view.wheel(-1.0F, max, max, 0.1);
    expect(view.level() == camera::OverviewLevel::off && !view.view_yaw(30.0F), "leaving restores the tactical yaw");
    view.toggle();
    expect(view.view_yaw(30.0F) == 30.0F, "a new overview takes the tactical yaw again");
}

// #350: in every overview level and at any tactical yaw, a pan moves the target along the drawn
// view's screen axes: right along the screen's right, up along its ground-projected forward.
void overview_pan_follows_the_screen() {
    camera::Constants k;
    k.distance_min = 100.0F;
    k.distance_max = 1900.0F;
    k.distance_default = 1900.0F;
    k.distance_per_mouse_unit = 50.0F;
    k.pitch_min = -10.0F;
    k.pitch_max = 85.0F;
    k.pitch_default = 45.0F;
    k.pitch_when_zoomed_in = 45.0F;
    k.yaw_min = -180.0F;
    k.yaw_max = 180.0F;
    k.yaw_per_mouse_unit = 1.5F;
    k.fov_min = 50.0F;
    k.fov_max = 50.0F;
    k.fov_default = 50.0F;
    k.near_clip = 10.0F;
    k.far_clip = 30000.0F;
    k.tactical_min_scroll_speed = 10.0F;
    k.tactical_max_scroll_speed = 20.0F;
    const camera::OverviewConstants c{2200.0F, 2900.0F, 62.0F, 80.0F, 60.0F, 70.0F, 10U, 1.8F};
    const camera::SourceTargetBounds bounds{-5000.0F, 5000.0F, -5000.0F, 5000.0F, "synthetic.json", "synthetic", "test"};
    const auto check = [](const bool condition, const std::string& message) { expect(condition, message.c_str()); };
    const auto unit = [](std::array<float, 3> v) {
        const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::array<float, 3>{v[0] / n, v[1] / n, v[2] / n};
    };
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    for (const float yaw : {0.0F, 30.0F, 135.0F, -90.0F}) {
        // Tactical -> overview -> map -> (one click in) overview again.
        for (const int presses : {1, 2, 3}) {
            const std::string what = "yaw " + std::to_string(static_cast<int>(yaw)) + ", step " + std::to_string(presses);
            auto made = camera::BoundedTacticalController::create(k, bounds, {0.0F, 0.0F, 0.0F}, 1.0F, yaw, 1280, 720);
            check(made.has_value(), what + ": controller");
            if (!made) continue;
            auto controller = made.value();
            camera::TacticalOverview view(c);
            view.toggle();
            if (presses >= 2) view.toggle();
            if (presses == 3) view.wheel(-1.0F, 1900.0F, 1900.0F, 0.0);
            const auto drawn = view.frame(controller.tactical_frame(), controller.yaw_degrees(), 6100.0F);
            check(drawn.has_value(), what + ": drawn frame");
            if (!drawn) continue;
            const auto& f = drawn.value();
            const auto forward = unit({f.target[0] - f.eye[0], f.target[1] - f.eye[1], f.target[2] - f.eye[2]});
            // Screen right = forward x up; screen up on the plane = forward without its height.
            const auto right = unit({forward[1] * f.up[2] - forward[2] * f.up[1], forward[2] * f.up[0] - forward[0] * f.up[2],
                                     forward[0] * f.up[1] - forward[1] * f.up[0]});
            const auto ahead = unit({forward[0], 0.0F, forward[2]});
            for (const auto& [units, axis, name] :
                 {std::tuple{camera::PanDisplacement{10.0F, 0.0F}, right, "right"},
                  std::tuple{camera::PanDisplacement{0.0F, 10.0F}, ahead, "up"}}) {
                auto moved = controller;
                camera::TacticalStep step{};
                step.translate_units = units;
                step.view_yaw_degrees = view.view_yaw(controller.yaw_degrees());
                check(moved.advance(step).has_value(), what + ": " + name + " pan");
                const auto& from = controller.tactical_frame().target;
                const auto& to = moved.tactical_frame().target;
                const auto went = unit({to[0] - from[0], 0.0F, to[2] - from[2]});
                check(dot(went, axis) > 0.9999F, what + ": a " + name + " pan follows the screen");
                check(moved.yaw_degrees() == yaw, what + ": the tactical yaw is kept");
            }
        }
    }
}

// Selection gestures never reach the simulation: a run that selects, boxes and recalls groups
// with no order has the command stream and hashes of an idle run; only the issued orders differ.
// O-1, O-2, O-3 and PR #342's review: a release needs its press, and attack mode is disarmed by any
// right click that is not on an enemy, including one on a selected own unit or an ally.
void right_clicks() {
    expect(ui::right_release(std::nullopt, {300.0F, 200.0F}) == ui::RightRelease::ignored,
           "a right release without a stored press (cancelled or synthetic) is no click");
    expect(ui::right_release(std::array<float, 2>{300.0F, 200.0F}, {340.0F, 260.0F}) == ui::RightRelease::click,
           "a short right drag is a click");
    expect(ui::right_release(std::array<float, 2>{300.0F, 200.0F}, {300.0F, 301.0F}) == ui::RightRelease::compass,
           "a right drag past MinimumDragSelectDistance is the compass");

    const auto own = unit(1, 7, true, {0.0F, 0.0F, 0.0F}, std::nullopt);
    const auto ally = unit(20, 7, false, {0.0F, 0.0F, 0.0F}, std::nullopt);
    const auto enemy = unit(30, 7, false, {0.0F, 0.0F, 0.0F}, std::nullopt, true);
    using ui::OrderMode;
    using ui::RightClick;
    expect(ui::right_click(OrderMode::none, &own, true) == RightClick::nothing, "no mode: a selected own unit takes no order");
    expect(ui::right_click(OrderMode::none, &ally, false) == RightClick::nothing, "no mode: an ally takes no order");
    expect(ui::right_click(OrderMode::none, &own, false) == RightClick::order, "no mode: an unselected own unit is a move");
    expect(ui::right_click(OrderMode::none, &enemy, false) == RightClick::order, "no mode: an enemy is an attack");
    expect(ui::right_click(OrderMode::none, nullptr, false) == RightClick::order, "no mode: empty space is a move");
    expect(ui::right_click(OrderMode::attack, &enemy, false) == RightClick::order, "attack mode attacks an enemy");
    for (const auto& [over, selected, what] : std::vector<std::tuple<const ui::BattleUnit*, bool, std::string_view>>{
             {&own, true, "a selected own unit"}, {&own, false, "an own unit"}, {&ally, false, "an ally"}, {nullptr, false, "empty space"}}) {
        expect(ui::right_click(OrderMode::attack, over, selected) == RightClick::disarm,
               (std::string("attack mode is disarmed by a right click on ") + std::string(what)).c_str());
        expect(ui::right_click(OrderMode::move, over, selected) == RightClick::order,
               (std::string("move mode moves on ") + std::string(what)).c_str());
    }
    expect(ui::right_click(OrderMode::move, &enemy, false) == RightClick::order, "move mode moves onto an enemy");

    // The review's repro: A, right click the selected unit, right click empty space: the second click moves.
    ui::CommandScheduler scheduler(1);
    ui::OrderInput input(scheduler);
    input.set_selection(std::vector<sim::EntityId>{1});
    input.arm(OrderMode::attack);
    if (ui::right_click(input.mode(), &own, true) == RightClick::disarm) input.cancel_mode();
    expect(input.mode() == OrderMode::none, "a right click on the selected unit disarms attack mode");
    expect(ui::right_click(input.mode(), nullptr, false) == RightClick::order, "the next right click on empty space orders");
    auto moved = input.world_command({{sim::math::Fixed::from_integer(50).value(), {}, {}}, sim::invalid_entity_id, false, false, false});
    expect(moved && moved.value(), "and the order is a move");
}

// #452 (docs/behaviour/space-orders.md OR-01): Ctrl+right click attack-moves, Ctrl+Alt+right click
// guards an own unit outside the selection or a point, an enemy is still attacked; the T and G
// modes do the same and disarm once used.
void modifier_orders() {
    using ui::OrderMode;
    using ui::RightClick;
    const auto own = unit(2, 7, true, {0.0F, 0.0F, 0.0F}, std::nullopt);
    expect(ui::right_click(OrderMode::attack_move, &own, true) == RightClick::order, "attack-move mode orders on any pick");
    expect(ui::right_click(OrderMode::guard, nullptr, false) == RightClick::order, "guard mode orders on any pick");
    ui::CommandScheduler scheduler(1);
    ui::OrderInput input(scheduler);
    input.set_selection(std::vector<sim::EntityId>{1});
    const sim::math::Vec3 ground{sim::math::Fixed::from_integer(50).value(), sim::math::Fixed::from_integer(60).value(), {}};
    const ui::WorldPick empty{ground, sim::invalid_entity_id, false, false, false};
    const ui::WorldPick friendly{ground, 2, false, true, false};
    const ui::WorldPick selected{ground, 1, false, true, true};
    const ui::WorldPick enemy{ground, 30, true, false, false};
    const ui::OrderModifiers ctrl{true, false};
    const ui::OrderModifiers ctrl_alt{true, true};
    const auto issue = [&](const ui::WorldPick& pick, const ui::OrderModifiers modifiers) {
        auto issued = input.world_command(pick, ui::CommandOrigin::world_click, modifiers);
        expect(issued && issued.value(), "the modified click issues an order");
        auto taken = scheduler.take(scheduler.open_tick());
        return taken.empty() ? tactical::CommandPayload{} : taken.back().payload;
    };
    const auto attack_moved = issue(empty, ctrl);
    const auto* attack_move = std::get_if<tactical::AttackMovePayload>(&attack_moved);
    expect(attack_move != nullptr && attack_move->destination == ground && attack_move->target == 0,
           "Ctrl on empty space: an attack-move to the point");
    expect(std::holds_alternative<tactical::AttackPayload>(issue(enemy, ctrl)), "Ctrl on an enemy: an attack");
    const auto guarded = issue(friendly, ctrl_alt);
    const auto* guard = std::get_if<tactical::GuardPayload>(&guarded);
    expect(guard != nullptr && guard->target == 2 && guard->destination == sim::math::Vec3{},
           "Ctrl+Alt on an own unit outside the selection: guard it, with no point");
    const auto held = issue(selected, ctrl_alt);
    const auto* point = std::get_if<tactical::GuardPayload>(&held);
    expect(point != nullptr && point->target == 0 && point->destination == ground,
           "Ctrl+Alt on a selected unit: guard the point");
    expect(std::holds_alternative<tactical::AttackPayload>(issue(enemy, ctrl_alt)), "Ctrl+Alt on an enemy: an attack");
    input.arm(OrderMode::guard);
    const auto moded = issue(friendly, {});
    expect(std::holds_alternative<tactical::GuardPayload>(moded) && input.mode() == OrderMode::none,
           "guard mode guards and then disarms");
    input.arm(OrderMode::attack_move);
    const auto moded_move = issue(empty, {});
    expect(std::holds_alternative<tactical::AttackMovePayload>(moded_move) && input.mode() == OrderMode::none,
           "attack-move mode attack-moves and then disarms");
    expect(std::holds_alternative<tactical::MovePayload>(issue(empty, {})), "no modifier, no mode: a plain move");
}

void replay_invariance() {
    tactical::TacticalSetup setup;
    setup.seed = 82;
    setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
    for (const auto& [id, owner] : std::vector<std::pair<sim::EntityId, tactical::PlayerId>>{{1, 1}, {2, 1}, {10, 2}}) {
        tactical::UnitState state;
        state.entity_id = id;
        state.type_id = 7;
        state.owner = owner;
        state.position = {sim::math::Fixed::from_integer(static_cast<std::int64_t>(id) * 10).value(), {}, {}};
        setup.units.push_back(state);
    }
    const auto units = fleet();
    const auto run = [&](const bool orders) {
        std::vector<std::string> hashes;
        auto created = tactical::TacticalSession::create(setup);
        expect(static_cast<bool>(created), "the invariance session is valid");
        if (!created) return hashes;
        auto session = std::move(created).value();
        ui::CommandScheduler scheduler(1);
        ui::OrderInput input(scheduler);
        ui::Selection selection;
        const sim::InlineExecutor executor;
        for (std::uint64_t tick = 0; tick < 6; ++tick) {
            const std::string before = session.state_sha256();
            selection.click(sim::EntityId{1}, {}, units, viewport);
            selection.box({0.0F, 0.0F, 250.0F, 150.0F}, true, units);
            selection.assign_group(1);
            selection.recall_group(1, false, static_cast<double>(tick) * 0.5, units);
            input.set_selection(selection.units());
            if (orders && tick == 2) {
                auto issued = input.world_command({{sim::math::Fixed::from_integer(50).value(), {}, {}}, sim::invalid_entity_id, false, false, false});
                expect(issued && issued.value(), "the order is issued");
            }
            expect(session.state_sha256() == before && session.pending_command_count() == 0,
                   "selecting never touches the session");
            for (auto& command : scheduler.take(session.completed_tick())) {
                expect(static_cast<bool>(session.submit(command)), "the order is accepted");
            }
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the tick runs");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        return hashes;
    };
    const auto idle_run = [&] {
        std::vector<std::string> hashes;
        auto session = tactical::TacticalSession::create(setup).value();
        const sim::InlineExecutor executor;
        for (int tick = 0; tick < 6; ++tick) hashes.push_back(session.step(executor).value().state_sha256);
        return hashes;
    }();
    const auto selecting = run(false);
    const auto ordering = run(true);
    expect(selecting == idle_run, "selection alone leaves every tick hash as an idle run");
    expect(ordering.size() == idle_run.size() && ordering[1] == idle_run[1] && ordering.back() != idle_run.back(),
           "an order changes the hashes from its tick on, through its recorded command");
}

} // namespace

// #424: a squadron's craft are pick volumes of one unit, its team container.
void squadrons() {
    std::vector<ui::BattleUnit> units{unit(30, 300, true, {0.0F, 0.0F, 0.0F}, std::array{100.0F, 100.0F}),
                                      unit(30, 300, true, {40.0F, 0.0F, 0.0F}, std::array{130.0F, 100.0F}),
                                      unit(31, 300, true, {400.0F, 0.0F, 0.0F}, std::array{600.0F, 100.0F}),
                                      unit(1, 7, true, {800.0F, 0.0F, 0.0F}, std::array{900.0F, 100.0F})};
    units[0].part = 11;
    units[1].part = 12;
    units[2].part = 21;
    const auto picked = ui::pick_index(down_at(40.0F, 0.0F), units);
    expect(picked == std::size_t{1} && units[*picked].entity == 30 && units[*picked].part == 12,
           "a craft picks its squadron and tells the craft");
    expect(ui::pick_unit(down_at(40.0F, 0.0F), units) == sim::EntityId{30}, "the pick is the squadron's container");
    ui::Selection selection;
    expect(selection.click(sim::EntityId{30}, {}, units, viewport) && selection.units() == std::vector<sim::EntityId>{30},
           "a click on a craft selects its squadron");
    expect(selection.box({0.0F, 0.0F, 700.0F, 200.0F}, false, units)
               && selection.units() == std::vector<sim::EntityId>({30, 31}),
           "a box over two craft of a squadron selects the squadron once");
    selection.clear();
    expect(selection.double_click(sim::EntityId{30}, units, viewport)
               && selection.units() == std::vector<sim::EntityId>({30, 31}),
           "a double click selects the squadrons of that type on screen");
    // WSU-38 (#550): an icon's double click goes by the leader's craft type and the craft on
    // screen: squadron 32 (another squadron type, the same craft type) comes in; squadron 33's
    // craft are off screen, and squadron 34's craft are of another type.
    units[0].part_type = 900;
    units[1].part_type = 900;
    units[2].part_type = 900;
    units.push_back(unit(32, 301, true, {0.0F, 40.0F, 0.0F}, std::array{100.0F, 150.0F}));
    units.back().part = 41;
    units.back().part_type = 900;
    units.push_back(unit(33, 300, true, {0.0F, 900.0F, 0.0F}, std::array{100.0F, 5000.0F}));
    units.back().part = 51;
    units.back().part_type = 900;
    units.push_back(unit(34, 300, true, {0.0F, 80.0F, 0.0F}, std::array{100.0F, 180.0F}));
    units.back().part = 61;
    units.back().part_type = 901;
    selection.clear();
    selection.click(sim::EntityId{30}, {}, units, viewport);
    expect(selection.craft_type_on_screen(900, units, viewport)
               && selection.units() == std::vector<sim::EntityId>({30, 31, 32}),
           "WSU-38: every own squadron with a craft of that type on screen, once each");
    expect(!selection.craft_type_on_screen(900, units, viewport), "WSU-38: nothing new is no change");
}

// A flat rectangle at height z in model space, as two triangles.
ui::PickMesh deck(const float x0, const float x1, const float y0, const float y1, const float z) {
    return ui::make_pick_mesh({{ui::Vec3f{x0, y0, z}, ui::Vec3f{x1, y0, z}, ui::Vec3f{x1, y1, z}},
                               {ui::Vec3f{x0, y0, z}, ui::Vec3f{x1, y1, z}, ui::Vec3f{x0, y1, z}}});
}

// #665 (WSU-10 to WSU-12): a ship is picked by its collision mesh, a craft by its mesh or else by
// its Mouse_Collide_Override_Sphere_Radius sphere; the highest contact wins.
void pick_volumes() {
    // A capital ship: its box spans y -75..75, its hull mesh only y -20..20, at z 30.
    const ui::PickMesh hull = deck(-150.0F, 150.0F, -20.0F, 20.0F, 30.0F);
    ui::BattleUnit ship = unit(1, 7, true, {0.0F, 0.0F, 0.0F}, std::array{300.0F, 300.0F});
    ship.box = box_at({0.0F, 0.0F, 0.0F}, 150.0F);
    ship.mesh = &hull;
    expect(ui::pick_contact(down_at(0.0F, 10.0F), ship) == ui::Vec3f{0.0F, 10.0F, 30.0F},
           "a ship is hit on its collision mesh (WSU-11)");
    expect(!ui::pick_contact(down_at(0.0F, 50.0F), ship), "a ray inside the ship's box but off its mesh misses");
    ui::BattleUnit scaled = ship;
    scaled.box = box_at({0.0F, 0.0F, 0.0F}, 150.0F, 0.0F, 2.0F);
    expect(ui::pick_contact(down_at(0.0F, 30.0F), scaled) == ui::Vec3f{0.0F, 30.0F, 60.0F},
           "the mesh is scaled and placed by the unit's frame");

    // Two X-wings over the hull: small meshes 1 unit above their centres, 50-unit spheres.
    const ui::PickMesh wing = deck(-5.0F, 5.0F, -3.0F, 3.0F, 1.0F);
    ui::BattleUnit craft = unit(2, 8, true, {20.0F, 0.0F, 30.0F}, std::array{320.0F, 300.0F});
    craft.mesh = &wing;
    craft.sphere_radius = 50.0F;
    ui::BattleUnit other = unit(3, 8, true, {-140.0F, 0.0F, 30.0F}, std::array{160.0F, 300.0F});
    other.mesh = &wing;
    other.sphere_radius = 50.0F;
    expect(ui::pick_contact(down_at(20.0F, 0.0F), craft) == ui::Vec3f{20.0F, 0.0F, 31.0F},
           "a ray on the craft's mesh hits the mesh, not the sphere above it (WSU-10)");
    const auto sphere = ui::pick_contact(down_at(20.0F, 15.0F), craft);
    expect(sphere && std::abs((*sphere)[2] - (30.0F + std::sqrt(2500.0F - 225.0F))) < 1.0e-3F,
           "off the mesh, the ray meets the override sphere (WSU-12)");
    const std::vector<ui::BattleUnit> units{ship, craft, other};
    expect(ui::pick_unit(down_at(20.0F, 15.0F), units) == sim::EntityId{2},
           "a fighter in front of a capital ship wins the pick by its sphere (#665)");
    expect(ui::pick_unit(down_at(-60.0F, 0.0F), units) == sim::EntityId{1},
           "a click on the hull between the fighters picks the ship");
    ui::BattleUnit no_sphere = craft;
    no_sphere.sphere_radius = 0.0F;
    expect(ui::pick_unit(down_at(20.0F, 15.0F), std::vector<ui::BattleUnit>{ship, no_sphere}) == sim::EntityId{1},
           "without the sphere the ship under the ray wins");

    // The owner's case: a double click on the fighter adds its type on screen, never the ship.
    ui::Selection selection;
    const auto picked = ui::pick_unit(down_at(20.0F, 15.0F), units);
    selection.click(picked, {}, units, viewport);
    selection.double_click(ui::pick_unit(down_at(20.0F, 15.0F), units), units, viewport);
    expect(selection.units() == std::vector<sim::EntityId>({2, 3}), "a double click on the fighter adds no ship (#665)");
}

int main() {
    picking();
    pick_volumes();
    squadrons();
    clicks_and_boxes();
    control_groups();
    overview();
    overview_yaw();
    overview_pan_follows_the_screen();
    right_clicks();
    modifier_orders();
    replay_invariance();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " battle input contract(s) failed\n";
        return 1;
    }
    std::cout << "battle input contracts passed\n";
    return 0;
}

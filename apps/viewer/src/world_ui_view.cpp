#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "world_ui_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace world_ui_detail;



WorldUiView::WorldUiView(Node3D& host) : host_(&host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

WorldUiView::~WorldUiView() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (const RID& circle : circles_) {
        if (circle.is_valid()) rendering->free_rid(circle);
    }
}

void WorldUiView::draw(const Frame& frame, const RID canvas_item) {
    if (frame.units == nullptr || frame.selection == nullptr || frame.live == nullptr || !frame.project) return;
    place_circles(frame);
    // The UI reference scale (layout.hpp UI-L1): screen height over 768 at 4:3 and wider.
    const float ui_scale = frame.viewport[1] > 0.0F
        ? std::min(frame.viewport[1] / 768.0F, frame.viewport[0] / 1024.0F * (frame.viewport[0] / frame.viewport[1] >= 4.0F / 3.0F ? 1.0e9F : 1.0F))
        : 1.0F;
    ability_rows_.clear();
    group_rows_.clear();
    ability_rows_.reserve(frame.units->size() * 2);
    group_rows_.reserve(frame.units->size() * 2);
    prepare_digits(icon_group_, ui_scale);
    prepare_digits(bracket_group_, ui_scale);
    if (frame.brackets) {
        draw_bars(frame, canvas_item, ui_scale);
    } else {
        health_bars_ = 0;
        shield_bars_ = 0;
        bar_rows_.clear();
    }
    draw_reticles(frame, canvas_item);
    draw_icons(frame, canvas_item, ui_scale);
}

} // namespace eawr::presentation::godot_backend

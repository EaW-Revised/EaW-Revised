#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"
#include "settings_store.hpp"

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
    owner_layer_ = memnew(CanvasLayer);
    owner_layer_->set_layer(40); // #5130: above the battle HUD, below modal menus and the cursor.
    host.add_child(owner_layer_);
    if (auto* rendering = RenderingServer::get_singleton()) {
        owner_canvas_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(owner_canvas_, owner_layer_->get_canvas());
    }
}

WorldUiView::~WorldUiView() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering && owner_canvas_.is_valid()) rendering->free_rid(owner_canvas_);
    if (owner_layer_) memdelete(owner_layer_);
    if (rendering == nullptr) return;
    for (const RID& circle : circles_) {
        if (circle.is_valid()) rendering->free_rid(circle);
    }
}

void WorldUiView::draw(const Frame& frame, const RID canvas_item) {
    draw_owner_name(frame);
    draw_waypoints(frame, canvas_item);
    if (!frame.visible) {
        if (auto* rendering = RenderingServer::get_singleton()) {
            for (const RID& circle : circles_) rendering->instance_set_visible(circle, false);
        }
        circles_drawn_ = health_bars_ = shield_bars_ = reticles_drawn_ = grid_icons_ = 0;
        bar_rows_.clear();
        icon_rows_.clear();
        flag_rows_.clear();
        icons_.clear();
        ability_rows_.clear();
        group_rows_.clear();
        reticle_rects_.clear();
        return;
    }
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

void WorldUiView::draw_owner_name(const Frame& frame) {
    auto* rendering = RenderingServer::get_singleton();
    owner_shown_ = nullptr;
    if (!rendering || !owner_canvas_.is_valid()) return;
    rendering->canvas_item_clear(owner_canvas_);
    const auto* settings = ActiveSettings::get();
    if (!frame.visible || !frame.live || !frame.units || !frame.pointer) return;
    const bool enabled = ui::hover_owner_names_enabled(settings ? settings->gameplay : ui::GameplaySettings{},
        frame.live->observer_multiplayer());
    sim::EntityId entity{};
    if (frame.hovered && *frame.hovered < frame.units->size()) {
        const auto& unit = (*frame.units)[*frame.hovered];
        entity = unit.part != sim::invalid_entity_id ? unit.part : unit.entity;
    } else if (frame.hovered_icon) entity = *frame.hovered_icon;
    else return;
    // Exactly one indexed hovered-instance lookup. Picking already uses visible units;
    // recheck current observer visibility so stale hover state cannot leak through fog.
    const auto* instance = frame.live->snapshot_index().instance(entity);
    if (!instance) return;
    const auto& visible = frame.live->snapshot_index().visible();
    const bool seen = std::binary_search(visible.begin(), visible.end(), entity);
    const auto* owner = ui::display_owner(*instance, owner_roster_, seen, enabled, frame.live->controller_policy_enabled());
    if (!owner || tooltip_font_.is_null()) return;
    const int pixels = ui::font_pixels({tooltip_points_, false, 1.0},
        static_cast<std::uint32_t>(frame.viewport[1])).em_height;
    if (pixels != owner_pixels_) {
        owner_pixels_ = pixels;
        for (std::size_t index = 0; index < owner_roster_.size(); ++index) {
            auto& line = owner_lines_[index];
            line->clear();
            line->add_string(String::utf8(owner_roster_[index].name.c_str()), tooltip_font_, pixels);
            static_cast<void>(line->get_size()); // Shape once at this font size, before per-frame drawing.
        }
    }
    const auto index = static_cast<std::size_t>(owner - owner_roster_.data());
    const auto& line = owner_lines_[index];
    const auto size = line->get_size();
    const Vector2 origin(std::clamp((*frame.pointer)[0] + 14.0F, 0.0F, std::max(0.0F, frame.viewport[0] - size.x)),
        std::clamp((*frame.pointer)[1] - size.y - 8.0F, 0.0F, std::max(0.0F, frame.viewport[1] - size.y)));
    line->draw(owner_canvas_, origin, colour(owner->colour));
    owner_shown_ = owner;
    owner_entity_ = entity;
}

void WorldUiView::follow_waypoint_lighting(const GodotRenderer& renderer) {
    if (waypoint_renderer_ && renderer.lighting()) waypoint_renderer_->set_lighting(*renderer.lighting());
}

void WorldUiView::draw_waypoints(const Frame& frame, const RID canvas_item) {
    waypoint_paths_ = waypoint_flags_ = waypoint_lines_ = waypoint_loops_ = 0;
    waypoint_stubs_omitted_ = 0;
    std::vector<sim::RenderInstance> flags;
    const auto* snapshot = frame.live ? frame.live->battle_frame().latest.get() : nullptr;
    const bool shown = frame.visible && frame.waypoints && frame.selection && frame.project && snapshot;
    const double waypoint_phase = waypoint_clock_.sample(shown,
        frame.live ? frame.live->presented_tick() : 0.0, waypoint_velocity_, waypoint_dash_ + waypoint_gap_);
    if (shown && waypoint_renderer_ && !waypoint_assets_.empty()) {
        if (frame.camera) {
            const auto& camera = *frame.camera;
            waypoint_renderer_->set_camera({camera.width, camera.height, camera.vertical_fov_degrees,
                camera.near_plane, camera.far_plane, camera.eye, camera.target, camera.up});
        }
        std::set<sim::EntityId> selected;
        for (const auto id : frame.selection->units()) {
            const auto member = frame.live->squadron_of().find(id);
            selected.insert(member == frame.live->squadron_of().end() ? id : member->second);
        }
        const auto source = [](const sim::math::Vec3& point) {
            return ui::Vec3f{to_float(point.x), to_float(point.y), to_float(point.z)};
        };
        for (const auto& instance : snapshot->instances()) {
            if (instance.owner != frame.live->local_player() || !selected.contains(instance.entity_id)
                || instance.waypoint_points.empty()) continue;
            const auto unit = frame.live->unit_frame(instance.entity_id);
            if (!unit) continue;
            std::vector<ui::Vec3f> points;
            points.push_back({static_cast<float>(unit->position[0]), static_cast<float>(unit->position[1]),
                static_cast<float>(unit->position[2])});
            std::optional<std::size_t> repeat;
            for (const auto& point : instance.waypoint_points) {
                if (point.repeat && !repeat) repeat = points.size();
                points.push_back(source(point.position));
                // OF-05: flag authored destinations once, independently of line-only vertices.
                const auto transform = ui::waypoint_flag_transform(point.position);
                for (const auto asset : waypoint_assets_)
                    flags.push_back({static_cast<sim::EntityId>(flags.size() + 1), asset, transform});
                ++waypoint_flags_;
            }
            if (repeat) {
                points.push_back(points[*repeat]);
                ++waypoint_loops_;
                // OF-05: omit the centroid stub when it lies within four units of the closing edge.
                const auto& a = points[points.size() - 2];
                const auto& b = points.back();
                const float dx = b[0] - a[0], dy = b[1] - a[1];
                const float length = std::hypot(dx, dy);
                if (length > 0.0F && std::abs((points[0][0] - a[0]) * dy - (points[0][1] - a[1]) * dx) / length < 4.0F) {
                    points.erase(points.begin());
                    ++waypoint_stubs_omitted_;
                }
            }
            ++waypoint_paths_;
            double phase = waypoint_phase;
            // Retail walks from the tail so dash phase remains continuous across all edges.
            for (std::size_t end = points.size() - 1; end > 0; --end) {
                const auto& a = points[end];
                const auto& b = points[end - 1];
                const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
                const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                const auto line = [&](const double start, const double finish) {
                    if (length <= 0.0) return;
                    const auto project = [&](const double at) { return frame.project(ui::Vec3f{
                        a[0] + dx * static_cast<float>(at / length), a[1] + dy * static_cast<float>(at / length),
                        a[2] + dz * static_cast<float>(at / length)}); };
                    const auto from = project(start), to = project(finish);
                    if (from && to) {
                        RenderingServer::get_singleton()->canvas_item_add_line(canvas_item,
                            Vector2((*from)[0], (*from)[1]), Vector2((*to)[0], (*to)[1]), waypoint_colour_, 1.0F);
                        ++waypoint_lines_;
                    }
                };
                const double period = waypoint_dash_ + waypoint_gap_;
                if (repeat) {
                    if (waypoint_dash_ > 0.0F && waypoint_gap_ > 0.0F) {
                        const double offset = std::fmod(phase, period);
                        for (double start = -offset; start < length; start += period)
                            if (start + waypoint_dash_ > 0.0) line(std::max(0.0, start), std::min(length, start + waypoint_dash_));
                    }
                } else line(0.0, length);
                phase += length;
            }
        }
    }
    max_waypoint_paths_ = std::max(max_waypoint_paths_, waypoint_paths_);
    max_waypoint_flags_ = std::max(max_waypoint_flags_, waypoint_flags_);
    if (waypoint_renderer_)
        waypoint_renderer_->submit(std::make_shared<const sim::RenderSnapshot>(snapshot ? snapshot->completed_tick() : 0, std::move(flags)));
}

} // namespace eawr::presentation::godot_backend

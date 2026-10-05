#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {
using Vec3 = std::array<float, 3>;

[[nodiscard]] Vec3 subtract(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] std::optional<Vec3> normalized(const Vec3& value) {
    const float length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length < 1.0e-6F) return std::nullopt;
    return Vec3{value[0] / length, value[1] / length, value[2] / length};
}

// The render basis is source (x, z, -y).
[[nodiscard]] Vec3 render_from_source(const Vec3& source) { return {source[0], source[2], -source[1]}; }
[[nodiscard]] Vec3 source_from_render(const Vec3& render) { return {render[0], -render[2], render[1]}; }

// The camera's view basis: forward, right and up, render basis.
struct ViewBasis final {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float tan_half{};
};

[[nodiscard]] std::optional<ViewBasis> basis(const camera::TacticalFrame& frame) {
    const auto forward = normalized(subtract(frame.target, frame.eye));
    if (!forward) return std::nullopt;
    const auto right = normalized(cross(*forward, frame.up));
    if (!right) return std::nullopt;
    constexpr float degrees = 3.14159265358979323846F / 180.0F;
    const float tan_half = std::tan(frame.vertical_fov_degrees * 0.5F * degrees);
    if (!std::isfinite(tan_half) || tan_half <= 0.0F) return std::nullopt;
    return ViewBasis{*forward, *right, cross(*right, *forward), tan_half};
}

} // namespace

std::optional<ui::PickRay> BattleInput::ray(const float x, const float y) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const auto view = basis(*frame_);
    if (!view) return std::nullopt;
    const float aspect = viewport_[0] / viewport_[1];
    const float ndc_x = 2.0F * x / viewport_[0] - 1.0F;
    const float ndc_y = 1.0F - 2.0F * y / viewport_[1];
    Vec3 direction{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        direction[axis] = view->forward[axis] + view->right[axis] * ndc_x * view->tan_half * aspect
            + view->up[axis] * ndc_y * view->tan_half;
    }
    return ui::PickRay{source_from_render(frame_->eye), source_from_render(direction)};
}

std::optional<std::array<float, 2>> BattleInput::project(const ui::Vec3f& source) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const auto view = basis(*frame_);
    if (!view) return std::nullopt;
    const Vec3 offset = subtract(render_from_source(source), frame_->eye);
    const float depth = dot(offset, view->forward);
    if (!(depth > frame_->near_plane)) return std::nullopt;
    const float aspect = viewport_[0] / viewport_[1];
    const float x = dot(offset, view->right) / (depth * view->tan_half * aspect);
    const float y = dot(offset, view->up) / (depth * view->tan_half);
    return std::array<float, 2>{(x + 1.0F) * 0.5F * viewport_[0], (1.0F - y) * 0.5F * viewport_[1]};
}

void BattleInput::record_pick_candidates(const ui::PickRay& pick_ray) {
    std::vector<PickCandidate> candidates;
    for (const ui::BattleUnit& unit : units_) {
        const auto contact = ui::pick_contact(pick_ray, unit);
        const auto box = ui::ray_box_contact(pick_ray, unit.box);
        if (!contact && !box) continue;
        PickCandidate candidate{unit.entity, unit.part};
        if (contact) {
            ui::BattleUnit geometry = unit;
            geometry.sphere_radius = 0.0F;
            const bool mesh = unit.mesh != nullptr && !unit.mesh->triangles.empty();
            candidate.contact_z = (*contact)[2];
            candidate.volume = ui::pick_contact(pick_ray, geometry) ? (mesh ? "mesh" : "box") : "sphere";
        }
        if (box) candidate.box_z = (*box)[2];
        candidates.push_back(candidate);
    }
    double_click_candidates_.push_back(std::move(candidates));
}

std::optional<std::array<std::array<double, 2>, 4>> BattleInput::ground_corners(const double height) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const std::array<std::array<float, 2>, 4> corners{
        {{0.0F, 0.0F}, {viewport_[0], 0.0F}, {viewport_[0], viewport_[1]}, {0.0F, viewport_[1]}}};
    std::array<std::array<double, 2>, 4> out{};
    std::array<std::array<double, 3>, 4> near_points{}, far_points{};
    std::array<double, 4> depths{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto corner_ray = ray(corners[index][0], corners[index][1]);
        if (!corner_ray) return std::nullopt;
        const auto& origin = corner_ray->origin;
        const auto& direction = corner_ray->direction;
        // ray() has forward component one, so this parameter is camera depth.
        depths[index] = std::abs(direction[2]) > 1.0e-9F
            ? (height - origin[2]) / direction[2] : -1.0;
        out[index] = {origin[0] + depths[index] * direction[0], origin[1] + depths[index] * direction[1]};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            near_points[index][axis] = origin[axis] + frame_->near_plane * direction[axis];
            far_points[index][axis] = origin[axis] + frame_->far_plane * direction[axis];
        }
    }
    const auto plane_cut = [height](const auto& lower, const auto& upper) {
        const double dz = upper[2] - lower[2];
        if (std::abs(dz) < 1.0e-9) return std::array<double, 2>{};
        const double fraction = (height - lower[2]) / dz;
        return std::array<double, 2>{lower[0] + fraction * (upper[0] - lower[0]),
                                     lower[1] + fraction * (upper[1] - lower[1])};
    };
    // MM-09: an upper ray outside the frustum meets the far face's vertical
    // edge instead. A lower ray behind the near face replaces both edges.
    if (depths[0] < frame_->near_plane || depths[1] < frame_->near_plane
        || depths[0] > frame_->far_plane || depths[1] > frame_->far_plane) {
        out[0] = plane_cut(far_points[3], far_points[0]);
        out[1] = plane_cut(far_points[2], far_points[1]);
    }
    if (depths[2] < frame_->near_plane || depths[3] < frame_->near_plane) {
        out[0] = plane_cut(far_points[3], far_points[0]);
        out[1] = plane_cut(far_points[2], far_points[1]);
        out[2] = plane_cut(near_points[2], near_points[1]);
        out[3] = plane_cut(near_points[3], near_points[0]);
    }
    return out;
}

void BattleInput::update_hover() {
    hovered_.reset();
    hovered_icon_.reset();
    hovered_hostile_ = false;
    hovered_selectable_ = false;
    cursor_hover_ = "empty";
    hover_point_.reset();
    if (!pointer_) return;
    const auto pick_ray = ray((*pointer_)[0], (*pointer_)[1]);
    if (pick_ray) hover_point_ = ui::battle_plane_point(*pick_ray);
    // WU-41: a reticle under the pointer keeps its unit hovered, ahead of an icon and the pick.
    if (const auto reticle = world_ui_->reticle_at(*pointer_)) {
        const auto unit = std::find_if(units_.begin(), units_.end(), [&](const ui::BattleUnit& candidate) {
            return candidate.entity == reticle->entity && candidate.part == sim::invalid_entity_id;
        });
        if (unit != units_.end()) {
            hovered_ = static_cast<std::size_t>(unit - units_.begin());
            hovered_hostile_ = unit->hostile;
            hovered_selectable_ = unit->own;
            cursor_hover_ = "hardpoint";
            return;
        }
    }
    if (const auto icon = world_ui_->icon_hit(*pointer_)) {
        hovered_icon_ = icon->entity;
        hovered_hostile_ = icon->hostile;
        hovered_selectable_ = icon->own;
        cursor_hover_ = "squadron_icon";
        return;
    }
    if (pick_ray) hovered_ = ui::pick_index(*pick_ray, units_);
    if (hovered_) {
        hovered_hostile_ = units_[*hovered_].hostile;
        hovered_selectable_ = units_[*hovered_].own;
        cursor_hover_ = "unit";
    }
}

} // namespace eawr::presentation::godot_backend

#include "land_deploy_zone_view.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace eawr::presentation::godot_backend {
namespace {

using godot::Color;
using godot::RenderingServer;
using godot::Vector3;

// Unshaded vertex colour that ignores depth, like the movement overlay's lines, so the ring reads through models.
constexpr const char* zone_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled;
void fragment() {
    ALBEDO = COLOR.rgb;
}
)GODOT";

constexpr float lift = 1.5F;          // above the ground, world units
constexpr int segments_per_ring = 96; // a smooth loop at the largest stock radius
constexpr double radius_limit = 1.0e6;

Vector3 ground_point(const sim::tactical::LandTerrain& terrain, const double x, const double y) {
    double z = 0.0;
    if (const auto height = terrain.height_at(
            sim::math::Fixed::from_raw(static_cast<std::int64_t>(x * sim::math::Fixed::scale)),
            sim::math::Fixed::from_raw(static_cast<std::int64_t>(y * sim::math::Fixed::scale))))
        z = static_cast<double>(height->raw()) / static_cast<double>(sim::math::Fixed::scale);
    // Render basis (x, z, -y).
    return Vector3(static_cast<float>(x), static_cast<float>(z) + lift, static_cast<float>(-y));
}

} // namespace

LandDeployZoneView::LandDeployZoneView(godot::Node3D& host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

LandDeployZoneView::~LandDeployZoneView() { release(); }

void LandDeployZoneView::clear_mesh() {
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (godot::RID* rid : {&instance_, &mesh_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = godot::RID();
    }
}

void LandDeployZoneView::frame(const std::span<const Ring> rings, const sim::tactical::LandTerrain& terrain) {
    if (rings.size() == rings_.size() && std::equal(rings.begin(), rings.end(), rings_.begin())) return;
    clear_mesh();
    auto* rendering = RenderingServer::get_singleton();
    // A list that cannot be drawn yet is not remembered, so the next frame tries again.
    if (rendering == nullptr || !scenario_.is_valid()) { rings_.clear(); return; }
    rings_.assign(rings.begin(), rings.end());
    if (rings_.empty()) return;
    std::vector<Vector3> positions;
    std::vector<Color> colours;
    const Color colour(0.2F, 1.0F, 0.3F);
    for (const Ring& ring : rings_) {
        if (!(ring.radius > 0.0) || ring.radius > radius_limit) continue;
        for (int step = 0; step < segments_per_ring; ++step) {
            const double from = 2.0 * std::numbers::pi * step / segments_per_ring;
            const double to = 2.0 * std::numbers::pi * (step + 1) / segments_per_ring;
            positions.push_back(ground_point(terrain, ring.x + ring.radius * std::cos(from), ring.y + ring.radius * std::sin(from)));
            positions.push_back(ground_point(terrain, ring.x + ring.radius * std::cos(to), ring.y + ring.radius * std::sin(to)));
            colours.push_back(colour);
            colours.push_back(colour);
        }
    }
    if (positions.empty()) return;
    if (!shader_.is_valid()) {
        shader_ = rendering->shader_create();
        rendering->shader_set_code(shader_, godot::String(zone_shader));
        material_ = rendering->material_create();
        rendering->material_set_shader(material_, shader_);
    }
    godot::PackedVector3Array vertex;
    godot::PackedColorArray colour_array;
    vertex.resize(static_cast<int64_t>(positions.size()));
    colour_array.resize(static_cast<int64_t>(colours.size()));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        vertex.set(static_cast<int64_t>(i), positions[i]);
        colour_array.set(static_cast<int64_t>(i), colours[i]);
    }
    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = vertex;
    arrays[godot::Mesh::ARRAY_COLOR] = colour_array;
    mesh_ = rendering->mesh_create();
    rendering->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_LINES, arrays);
    rendering->mesh_surface_set_material(mesh_, 0, material_);
    instance_ = rendering->instance_create2(mesh_, scenario_);
}

void LandDeployZoneView::release() {
    clear_mesh();
    rings_.clear();
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (godot::RID* rid : {&material_, &shader_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = godot::RID();
    }
}

} // namespace eawr::presentation::godot_backend

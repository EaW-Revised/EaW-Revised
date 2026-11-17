#include "path_line_view.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace eawr::presentation::godot_backend {
namespace {

using godot::RenderingServer;

// PL-04: unshaded, alpha blended, no depth test and no culling (the original draws the line over
// ships and scenery). The plan frame rides in UV.x; a vertex with a negative frame is unused and is sent
// far outside the view (writing POSITION would replace the whole default transform), so the surface keeps
// its fixed capacity. `presented` is the plan frame the ships have
// reached: the part of a line before it is gone (PL-06).
constexpr const char* path_line_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, blend_mix, cull_disabled, depth_test_disabled, depth_draw_never;
uniform vec4 line_colour : source_color = vec4(0.5, 1.0, 0.5, 0.5);
uniform float presented = 0.0;
varying float plan_frame;
void vertex() {
    plan_frame = UV.x;
    if (UV.x < -0.5) {
        VERTEX = vec3(0.0, 1.0e9, 0.0);
    }
}
void fragment() {
    if (plan_frame < presented) {
        discard;
    }
    ALBEDO = line_colour.rgb;
    ALPHA = line_colour.a;
}
)GODOT";

double wall_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

PathLineView::PathLineView(godot::Node3D& host) : builder_() {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
    capacity_ = builder_.caps().max_segments * 2;
}

PathLineView::~PathLineView() { release(); }

void PathLineView::release() {
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (godot::RID* rid : {&instance_, &mesh_, &material_, &shader_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = godot::RID();
    }
    created_ = false;
}

void PathLineView::set_colour(const godot::Color colour) {
    colour_ = colour;
    colour_dirty_ = true;
}

bool PathLineView::create_surface() {
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario_.is_valid()) return false;
    shader_ = rendering->shader_create();
    rendering->shader_set_code(shader_, godot::String(path_line_shader));
    material_ = rendering->material_create();
    rendering->material_set_shader(material_, shader_);
    // The whole capacity exists from the start, every vertex unused (frame -1).
    godot::PackedVector3Array positions;
    godot::PackedVector2Array frames;
    positions.resize(capacity_);
    frames.resize(capacity_);
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        positions.set(index, godot::Vector3());
        frames.set(index, godot::Vector2(-1.0F, 0.0F));
    }
    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_TEX_UV] = frames;
    mesh_ = rendering->mesh_create();
    rendering->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_LINES, arrays, godot::Array(),
        godot::Dictionary(), RenderingServer::ARRAY_FLAG_USE_DYNAMIC_UPDATE);
    rendering->mesh_surface_set_material(mesh_, 0, material_);
    // Lines move with the world, far outside the initial all-zero bounds.
    rendering->mesh_set_custom_aabb(mesh_, godot::AABB(godot::Vector3(-1.0e5F, -1.0e5F, -1.0e5F), godot::Vector3(2.0e5F, 2.0e5F, 2.0e5F)));
    const std::uint32_t format = static_cast<std::uint32_t>(static_cast<int64_t>(rendering->mesh_get_surface(mesh_, 0)["format"]));
    vertex_stride_ = rendering->mesh_surface_get_format_vertex_stride(format, static_cast<int32_t>(capacity_));
    attribute_stride_ = rendering->mesh_surface_get_format_attribute_stride(format, static_cast<int32_t>(capacity_));
    attribute_offset_ = rendering->mesh_surface_get_format_offset(format, static_cast<int32_t>(capacity_), godot::Mesh::ARRAY_TEX_UV);
    // The writer below knows the plain float layout; any other layout leaves the overlay off.
    if (vertex_stride_ != 12 || attribute_stride_ != 8 || attribute_offset_ != 0) {
        usable_ = false;
        release();
        return false;
    }
    instance_ = rendering->instance_create2(mesh_, scenario_);
    rendering->instance_geometry_set_cast_shadows_setting(instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
    rendering->instance_set_visible(instance_, false);
    created_ = true;
    colour_dirty_ = true;
    return true;
}

void PathLineView::upload() {
    auto* rendering = RenderingServer::get_singleton();
    const auto vertices = builder_.vertices();
    const auto used = static_cast<std::uint32_t>(vertices.size());
    // The written range: the used vertices, then the unused tail of the last upload, hidden again.
    const std::uint32_t count = std::max(used, written_);
    if (count > 0) {
        godot::PackedByteArray positions, frames;
        positions.resize(static_cast<int64_t>(count) * 12);
        frames.resize(static_cast<int64_t>(count) * 8);
        auto* position_bytes = positions.ptrw();
        auto* frame_bytes = frames.ptrw();
        for (std::uint32_t index = 0; index < count; ++index) {
            float position[3]{};
            float frame[2]{-1.0F, 0.0F};
            if (index < used) {
                // Render basis (x, z, -y).
                position[0] = vertices[index].x;
                position[1] = vertices[index].z;
                position[2] = -vertices[index].y;
                frame[0] = vertices[index].frame;
            }
            std::memcpy(position_bytes + static_cast<std::size_t>(index) * 12, position, 12);
            std::memcpy(frame_bytes + static_cast<std::size_t>(index) * 8, frame, 8);
        }
        rendering->mesh_surface_update_vertex_region(mesh_, 0, 0, positions);
        rendering->mesh_surface_update_attribute_region(mesh_, 0, 0, frames);
        ++uploads_;
    }
    written_ = used;
    drawn_segments_ = used / 2;
    uploaded_generation_ = builder_.generation();
    presented_set_ = -1.0e9F; // the base frame may have moved
}

void PathLineView::frame(const bool enabled, const std::span<const ui::PathLineShip> ships, const double presented_tick) {
    if (!usable_) return;
    const bool wanted = enabled && !ships.empty();
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    if (!created_) {
        if (!wanted || !create_surface()) return;
    }
    builder_.update(enabled, ships, wall_seconds());
    if (builder_.generation() != uploaded_generation_) upload();
    const bool shown = written_ > 0;
    rendering->instance_set_visible(instance_, shown);
    if (!shown) return;
    if (colour_dirty_) {
        rendering->material_set_param(material_, "line_colour", colour_);
        colour_dirty_ = false;
    }
    const auto presented = static_cast<float>(presented_tick - builder_.base_frame());
    if (presented != presented_set_) {
        rendering->material_set_param(material_, "presented", presented);
        presented_set_ = presented;
    }
}

} // namespace eawr::presentation::godot_backend

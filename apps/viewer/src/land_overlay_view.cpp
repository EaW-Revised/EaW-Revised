#include "land_overlay_view.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <vector>

namespace eawr::presentation::godot_backend {
namespace {

using godot::Color;
using godot::RenderingServer;
using godot::Vector3;
using sim::tactical::LandOverlayPoint;
using sim::tactical::LandOverlaySquadState;

// Unshaded vertex colour that ignores depth, so the lines read through terrain and models.
constexpr const char* overlay_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled;
void fragment() {
    ALBEDO = COLOR.rgb;
}
)GODOT";

constexpr float lift = 1.5F; // above the ground, world units

struct Lines {
    std::vector<Vector3> positions;
    std::vector<Color> colours;
    const sim::tactical::LandTerrain* terrain{};

    Vector3 at(const LandOverlayPoint& p) const {
        double z = 0.0;
        if (const auto height = terrain->height_at(
                sim::math::Fixed::from_raw(static_cast<std::int64_t>(p.x * sim::math::Fixed::scale)),
                sim::math::Fixed::from_raw(static_cast<std::int64_t>(p.y * sim::math::Fixed::scale))))
            z = static_cast<double>(height->raw()) / static_cast<double>(sim::math::Fixed::scale);
        // Render basis (x, z, -y).
        return Vector3(static_cast<float>(p.x), static_cast<float>(z) + lift, static_cast<float>(-p.y));
    }
    void segment(const LandOverlayPoint& a, const LandOverlayPoint& b, const Color& colour) {
        positions.push_back(at(a)); positions.push_back(at(b));
        colours.push_back(colour); colours.push_back(colour);
    }
    void cross(const LandOverlayPoint& c, double arm, const Color& colour) {
        segment({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, colour);
        segment({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, colour);
    }
    void dot(const LandOverlayPoint& c, double r, const Color& colour) {
        segment({c.x - r, c.y}, {c.x + r, c.y}, colour);
        segment({c.x, c.y - r}, {c.x, c.y + r}, colour);
    }
};

Color colour_of(LandOverlaySquadState state) {
    switch (state) {
    case LandOverlaySquadState::turning: return Color(1.0F, 0.9F, 0.1F);  // yellow
    case LandOverlaySquadState::yielded: return Color(1.0F, 0.5F, 0.0F);  // orange
    case LandOverlaySquadState::lagging: return Color(1.0F, 0.1F, 0.8F);  // magenta: parent slowed for a lagging member
    case LandOverlaySquadState::moving: return Color(0.2F, 1.0F, 0.3F);   // green
    case LandOverlaySquadState::idle: break;
    }
    return Color(0.7F, 0.7F, 0.7F);
}

} // namespace

LandOverlayView::LandOverlayView(godot::Node3D& host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

LandOverlayView::~LandOverlayView() { release(); }

void LandOverlayView::set_shown(bool shown) {
    shown_ = shown;
    drawn_tick_ = ~std::uint64_t{0};
    if (!shown_) release();
}

void LandOverlayView::frame(const sim::tactical::LandOverlayData* data, const sim::tactical::LandTerrain& terrain) {
    if (!shown_ || data == nullptr || data->tick == drawn_tick_ || !scenario_.is_valid()) return;
    drawn_tick_ = data->tick;
    Lines lines;
    lines.terrain = &terrain;
    for (const auto& squad : data->squads) {
        const Color colour = colour_of(squad.state);
        for (std::size_t n = 1; n < squad.path.size(); ++n) {
            // Walked segments dim, the leg being walked and the rest of the path at full colour.
            const bool ahead = n >= squad.next_node;
            lines.segment(squad.path[n - 1], squad.path[n], ahead ? colour : Color(colour.r * 0.35F, colour.g * 0.35F, colour.b * 0.35F));
        }
        if (squad.has_order) {
            lines.cross(squad.order_target, 4.0, Color(1.0F, 1.0F, 1.0F));
            if (squad.next_node < squad.path.size()) {
                lines.dot(squad.path[squad.next_node], 2.0, colour);                      // the current next node
                lines.segment(squad.position, squad.path[squad.next_node], colour);       // parent to next node
            }
            for (std::size_t n = squad.next_node + 1; n < squad.path.size(); ++n) lines.dot(squad.path[n], 0.8, colour);
        }
        lines.cross(squad.position, 1.5, colour);
        for (const auto& member : squad.members) {
            lines.dot(member.slot_target, 1.0, colour);
            lines.segment(member.position, member.slot_target, Color(colour.r * 0.6F, colour.g * 0.6F, colour.b * 0.6F));
        }
    }
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    if (!shader_.is_valid()) {
        shader_ = rendering->shader_create();
        rendering->shader_set_code(shader_, godot::String(overlay_shader));
        material_ = rendering->material_create();
        rendering->material_set_shader(material_, shader_);
    }
    if (instance_.is_valid()) { rendering->free_rid(instance_); instance_ = godot::RID(); }
    if (mesh_.is_valid()) { rendering->free_rid(mesh_); mesh_ = godot::RID(); }
    if (lines.positions.empty()) return;
    godot::PackedVector3Array positions;
    godot::PackedColorArray colours;
    positions.resize(static_cast<int64_t>(lines.positions.size()));
    colours.resize(static_cast<int64_t>(lines.colours.size()));
    for (std::size_t i = 0; i < lines.positions.size(); ++i) {
        positions.set(static_cast<int64_t>(i), lines.positions[i]);
        colours.set(static_cast<int64_t>(i), lines.colours[i]);
    }
    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_COLOR] = colours;
    mesh_ = rendering->mesh_create();
    rendering->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_LINES, arrays);
    rendering->mesh_surface_set_material(mesh_, 0, material_);
    instance_ = rendering->instance_create2(mesh_, scenario_);
}

void LandOverlayView::release() {
    auto* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (godot::RID* rid : {&instance_, &mesh_, &material_, &shader_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = godot::RID();
    }
}

} // namespace eawr::presentation::godot_backend

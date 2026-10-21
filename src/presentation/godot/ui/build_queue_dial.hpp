#pragma once

#include "gpu_debug_labels.hpp"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <array>

namespace eawr::presentation::godot_backend {

// PU-64: neutral additive unfinished-sector veil, clockwise from noon.
// Both queue adapters retain this immutable geometry and change only material state.
class BuildQueueDialRenderer final {
public:
    // Tactical callers supply counted buffer types for the warmed-allocation probe.
    template <typename Vertices = godot::PackedVector3Array,
              typename Uvs = godot::PackedVector2Array,
              typename Indices = godot::PackedInt32Array>
    void prepare() {
        Vertices vertices;
        Uvs uvs;
        Indices indices;
        vertices.resize(10); uvs.resize(10); indices.resize(24);
        vertices.set(0, godot::Vector3(0.5F, 0.5F, 0));
        uvs.set(0, godot::Vector2(-1, 0));
        const std::array<godot::Vector2, 9> rim{{{0.5F, 0}, {1, 0}, {1, 0.5F}, {1, 1}, {0.5F, 1},
            {0, 1}, {0, 0.5F}, {0, 0}, {0.5F, 0}}};
        for (int index = 0; index < 9; ++index) {
            const auto uv = rim[static_cast<std::size_t>(index)];
            vertices.set(index + 1, godot::Vector3(uv.x, uv.y, 0));
            uvs.set(index + 1, godot::Vector2(static_cast<float>(index), 0));
            if (index < 8) {
                indices.set(index * 3, 0);
                indices.set(index * 3 + 1, index + 1);
                indices.set(index * 3 + 2, index + 2);
            }
        }
        godot::Array arrays;
        arrays.resize(godot::Mesh::ARRAY_MAX);
        arrays[godot::Mesh::ARRAY_VERTEX] = vertices;
        arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
        arrays[godot::Mesh::ARRAY_INDEX] = indices;
        mesh_.instantiate();
        mesh_->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
        shader_.instantiate();
        name_canvas_shader(*shader_.ptr(), "EAWR/UI/BuildQueueDial");
        shader_->set_code(R"(shader_type canvas_item;
render_mode blend_add;
uniform float completion_percent = 100.0;
varying vec3 veil_modulate;
void vertex() {
    veil_modulate = COLOR.rgb;
    vec2 rim[9] = vec2[9](vec2(0.5,0.0),vec2(1.0,0.0),vec2(1.0,0.5),vec2(1.0,1.0),
        vec2(0.5,1.0),vec2(0.0,1.0),vec2(0.0,0.5),vec2(0.0,0.0),vec2(0.5,0.0));
    float step = max(UV.x, completion_percent * 0.08);
    int segment = int(floor(max(step, 0.0)));
    UV = UV.x < 0.0 ? vec2(0.5) : mix(rim[segment], rim[min(segment+1,8)], fract(step));
    VERTEX = UV;
}
void fragment() {
    // PU-64 fixed-function prim path: ONE/ONE, texture-stage alpha disabled.
    // The alternate FX prim-additive branch remains an unchecked seam.
    COLOR = vec4(texture(TEXTURE, UV).rgb * veil_modulate, 1.0);
}
)");
    }
    [[nodiscard]] godot::Ref<godot::ShaderMaterial> material() const {
        godot::Ref<godot::ShaderMaterial> result;
        result.instantiate();
        result->set_shader(shader_);
        return result;
    }
    static void set_percent(const godot::Ref<godot::ShaderMaterial>& material, const int percent) {
        static const godot::StringName parameter{"completion_percent"};
        material->set_shader_parameter(parameter, std::clamp(percent, 0, 100));
    }
    void submit(godot::RenderingServer& rendering, const godot::RID& item,
                const godot::Ref<godot::Texture2D>& texture) const {
        rendering.canvas_item_add_mesh(item, mesh_->get_rid(), godot::Transform2D(),
            godot::Color(1, 1, 1, 1), texture->get_rid());
    }
    void draw(godot::Control& control, const godot::Ref<godot::Texture2D>& texture) const {
        const auto size = control.get_size();
        control.draw_mesh(mesh_, texture, godot::Transform2D(godot::Vector2(size.x, 0),
            godot::Vector2(0, size.y), godot::Vector2()), godot::Color(1, 1, 1, 1));
    }
private:
    godot::Ref<godot::ArrayMesh> mesh_;
    godot::Ref<godot::Shader> shader_;
};

class EawrBuildQueueDial final : public godot::Control {
    GDCLASS(EawrBuildQueueDial, godot::Control)
public:
    EawrBuildQueueDial() {
        renderer_.prepare();
        material_ = renderer_.material();
        set_material(material_);
        set_mouse_filter(MOUSE_FILTER_IGNORE);
    }
    void set_texture(const godot::Ref<godot::Texture2D>& texture) {
        if (texture_ == texture) return;
        texture_ = texture;
        queue_redraw();
    }
    void set_percent(int completion) {
        const int percent = std::clamp(completion, 0, 100);
        if (percent == percent_) return;
        percent_ = percent;
        BuildQueueDialRenderer::set_percent(material_, percent);
        queue_redraw();
    }
    [[nodiscard]] int percent() const { return percent_; }
    [[nodiscard]] bool veiled() const noexcept { return texture_.is_valid() && percent_ < 100; }
    void _draw() override {
        if (!veiled()) return;
        renderer_.draw(*this, texture_);
    }
protected:
    static void _bind_methods() {
        godot::ClassDB::bind_method(godot::D_METHOD("set_percent", "completion"), &EawrBuildQueueDial::set_percent);
        godot::ClassDB::bind_method(godot::D_METHOD("get_percent"), &EawrBuildQueueDial::percent);
    }
private:
    BuildQueueDialRenderer renderer_;
    godot::Ref<godot::ShaderMaterial> material_;
    godot::Ref<godot::Texture2D> texture_;
    int percent_{100};
};

} // namespace eawr::presentation::godot_backend

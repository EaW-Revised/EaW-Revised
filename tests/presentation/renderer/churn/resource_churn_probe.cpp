// Isolated synthetic Godot exercise for the production GodotRenderer's
// resource lifecycle under live churn (#22). It instantiates the production
// renderer on a SubViewport host, uploads invented meshes/textures/shaders,
// submits invented RenderSnapshots and observes the result from Godot itself:
//  - RenderingServer TOTAL_OBJECTS_IN_FRAME for the frames drawn after each
//    step; Forward+ (like the Compatibility renderer) counts one object per
//    drawn mesh surface, so stale or missing instances change it;
//  - RenderingServer TEXTURE_MEM_USED / BUFFER_MEM_USED against a baseline
//    taken with no renderer alive (textures, mesh buffers and skeleton
//    transform textures that outlive their owner stay counted);
//  - the renderer's instance_evidence(), which reads each live instance's
//    mesh surface count and skeleton bones back from RenderingServer.
// The Python runner additionally rejects any engine "leaked at exit" line and
// runs a leak control that proves such a line is printed for an unfreed RID.
// Nothing here is a game asset, viewer mode or graphical baseline.

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
// Not for console output: godot-cpp links libstdc++ statically on Linux, and
// from GCC 13 only <iostream> references the library's stream initialisation
// (ios_base_library_init). Without it, g++-14 on Linux crashed with SIGSEGV at
// the report's first ostringstream << integer in finish().
#include <iostream> // IWYU pragma: keep
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource_churn_probe.hpp"

namespace eawr_resource_churn_probe {

[[nodiscard]] sim::math::Mat3x4 lane(const std::int64_t x) {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed unit = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {unit, zero, zero, Fixed::from_raw(x * Fixed::scale)};
    result.rows[1] = {zero, unit, zero, zero};
    result.rows[2] = {zero, zero, unit, zero};
    return result;
}

// One triangle per surface in the source XZ plane, which faces the fixed
// camera after the renderer's source-to-render basis change.
[[nodiscard]] assets::Submesh triangle(const float half, const float lift) {
    assets::Submesh submesh;
    submesh.vertices = {
        {{-half, 0.0F, lift - half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{half, 0.0F, lift - half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{0.0F, 0.0F, lift + half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
    };
    submesh.indices = {0, 1, 2};
    return submesh;
}

[[nodiscard]] assets::Model plate(const float half, const std::size_t surfaces, const bool rigid_skin) {
    assets::Mesh mesh;
    mesh.name = "churn-plate";
    for (std::size_t index = 0; index < surfaces; ++index) {
        mesh.submeshes.push_back(triangle(half, static_cast<float>(index) * half * 0.25F));
    }
    assets::Model model;
    if (rigid_skin) {
        model.bones.resize(1);
        mesh.bone = 0;
    }
    model.meshes.push_back(std::move(mesh));
    return model;
}

// The first surface reaches RenderingServer; the second names a palette bone
// outside the model, so the upload fails after RIDs and a surface exist.
[[nodiscard]] assets::Model partially_invalid_plate() {
    assets::Model model = plate(40.0F);
    model.bones.resize(1);
    assets::Submesh bad = triangle(40.0F, 20.0F);
    bad.skin_bones = {5};
    for (assets::Vertex& vertex : bad.vertices) {
        vertex.bone_indices = {0, 0, 0, 0};
        vertex.bone_weights = {1.0F, 0.0F, 0.0F, 0.0F};
    }
    model.meshes.front().submeshes.push_back(std::move(bad));
    return model;
}

[[nodiscard]] assets::Texture solid(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b,
    const std::uint32_t edge) {
    assets::MipLevel mip;
    mip.width = edge;
    mip.height = edge;
    mip.row_pitch = edge * 4;
    for (std::uint32_t texel = 0; texel < edge * edge; ++texel) {
        for (const std::uint8_t channel : {r, g, b, std::uint8_t{255}}) mip.bytes.push_back(std::byte{channel});
    }
    assets::Texture texture;
    texture.width = edge;
    texture.height = edge;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] presentation::MaterialDescription unshaded(const std::string_view color) {
    return {
        .schema_version = presentation::MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::modern_spatial,
        .pass = presentation::RenderPass::opaque,
        .program = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n"
                   "void fragment() { ALBEDO = vec3(" + std::string(color) + "); }\n",
        .technique = {},
        .pass_name = {},
        .bindings = {},
    };
}

[[nodiscard]] std::shared_ptr<const sim::RenderSnapshot> scene(
    const std::uint64_t tick, const std::vector<Member>& members) {
    std::vector<sim::RenderInstance> instances;
    for (const Member& member : members) instances.push_back({member.entity, member.asset, lane(member.x)});
    return std::make_shared<const sim::RenderSnapshot>(tick, std::move(instances));
}

[[nodiscard]] std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}
} // namespace eawr_resource_churn_probe

void EawrResourceChurnProbe::check(const bool condition, const std::string& message) {
    if (condition) return;
    const std::string where = index_ < steps_.size() ? steps_[index_].name : std::string("setup");
    failures_.push_back(where + ": " + message);
}

EawrResourceChurnProbe::Sample EawrResourceChurnProbe::sample() const {
    RenderingServer* rendering = RenderingServer::get_singleton();
    return {
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME)),
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED)),
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED)),
    };
}

void EawrResourceChurnProbe::record(const std::string& step, const Sample& value) {
    samples_.push_back("{\"step\":\"" + escape(step) + "\",\"objects\":" + std::to_string(value.objects)
        + ",\"texture\":" + std::to_string(value.texture) + ",\"buffer\":" + std::to_string(value.buffer) + "}");
}

void EawrResourceChurnProbe::upload(const sim::AssetId asset, const assets::Model& model,
    const assets::Texture& texture, const presentation::MaterialDescription& material) {
    const auto uploaded = renderer_->upload(asset, model, texture, material);
    check(uploaded.has_value(), "upload of asset " + std::to_string(asset) + " failed: "
        + (uploaded ? std::string() : uploaded.error().message));
}

void EawrResourceChurnProbe::submit(std::shared_ptr<const sim::RenderSnapshot> snapshot, const std::size_t expected,
    const std::size_t drawn) {
    renderer_->submit(std::move(snapshot));
    expected_instances_ = expected;
    expected_drawn_ = drawn == static_cast<std::size_t>(-1) ? expected : drawn;
    check(renderer_->instance_count() == expected, "registry holds " + std::to_string(renderer_->instance_count())
        + " instances, expected " + std::to_string(expected));
}

void EawrResourceChurnProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i < arguments.size(); ++i) {
        if (i + 1 < arguments.size() && arguments[i] == String("--eawr-churn-report")) {
            report_path_ = arguments[i + 1].utf8().get_data();
        }
        if (arguments[i] == String("--eawr-churn-leak-control")) leak_control_ = true;
    }
    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(view_width, view_height));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);
    build_steps();
    set_process(true);
}

void EawrResourceChurnProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (finishing_) {
        finish();
        return;
    }
    if (++frames_ > frame_budget) {
        check(false, "frame budget exhausted");
        renderer_.reset();
        finishing_ = true;
        return;
    }
    if (wait_ > 0) {
        if (--wait_ > 0) return;
        if (steps_[index_].verify) steps_[index_].verify();
        ++index_;
    }
    if (index_ < steps_.size()) {
        steps_[index_].act();
        wait_ = steps_[index_].hold;
        return;
    }
    finishing_ = true;
}

namespace {

void initialize_resource_churn_probe(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrResourceChurnProbe);
}

void uninitialize_resource_churn_probe(const ModuleInitializationLevel) {}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_resource_churn_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_resource_churn_probe);
    init.register_terminator(uninitialize_resource_churn_probe);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

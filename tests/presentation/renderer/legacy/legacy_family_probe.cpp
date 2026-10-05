// Synthetic WP-43 legacy/ family exercise through the production GodotRenderer.
// Every quad, colour and texture below is invented; this is not a retail
// comparison. The probe records sampled pixels and upload diagnostics; the
// runner (tests/presentation/renderer/test_legacy_family_runtime.py) holds
// them to the families' declared arithmetic.
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include "shader_adapter.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace godot;
namespace {
namespace sim = eawr::sim;
namespace presentation = eawr::presentation;
namespace assets = eawr::assets;
using eawr::presentation::godot_backend::GodotRenderer;

constexpr int viewport_size = 256;
constexpr float eye_height = 20.0F;
constexpr float fov = 45.0F;
constexpr float tan_half = 0.41421356237F;
constexpr std::int64_t fixed_one = std::int64_t{1} << 24;
using Rgba = std::array<std::uint8_t, 4>;

[[nodiscard]] sim::math::Mat3x4 identity() {
    sim::math::Mat3x4 result;
    for (int i = 0; i < 3; ++i) result.rows[i][i] = sim::math::Fixed::from_raw(fixed_one);
    return result;
}

// A 16x16 texture of four 8x8 quadrants, in order top-left, top-right,
// bottom-left, bottom-right as seen from above.
[[nodiscard]] assets::Texture quadrant_texture(const std::array<Rgba, 4>& quadrants) {
    assets::Texture result;
    result.width = 16;
    result.height = 16;
    result.format = assets::PixelFormat::rgba8;
    result.has_alpha = true;
    assets::MipLevel mip{16, 16, 64, {}};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const Rgba& rgba = quadrants[static_cast<std::size_t>((y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0))];
            for (const std::uint8_t channel : rgba) mip.bytes.push_back(static_cast<std::byte>(channel));
        }
    }
    result.mips.push_back(std::move(mip));
    return result;
}

[[nodiscard]] assets::Texture solid_texture(const Rgba& rgba) {
    return quadrant_texture({rgba, rgba, rgba, rgba});
}

// Source X right and Y up in plan view, Z toward the camera. UV (0, 0) is the
// top-left corner seen from above. The front winding is ALO's outward order,
// counter-clockwise seen from above in this plan view; the upload reverses it
// into Godot's front-face convention, so every legacy adapter keeps it under
// its renderer-baseline cull_back. `reversed` is the culled winding.
void add_quad(assets::Submesh& surface, const float x0, const float x1, const float y0, const float y1,
    const float z, const bool reversed) {
    const auto base = static_cast<std::uint16_t>(surface.vertices.size());
    const std::array<std::array<float, 4>, 4> corners{{
        {x0, y0, 0.0F, 1.0F}, {x1, y0, 1.0F, 1.0F}, {x1, y1, 1.0F, 0.0F}, {x0, y1, 0.0F, 0.0F}}};
    for (const auto& corner : corners) {
        assets::Vertex vertex;
        vertex.position = {corner[0], corner[1], z};
        vertex.normal = {0.0F, 0.0F, 1.0F};
        vertex.tangent = {1.0F, 0.0F, 0.0F};
        vertex.binormal = {0.0F, 1.0F, 0.0F};
        vertex.texcoord[0] = {corner[2], corner[3]};
        vertex.color = {1.0F, 1.0F, 1.0F, 1.0F};
        surface.vertices.push_back(vertex);
    }
    const std::array<std::uint16_t, 6> front{0, 1, 2, 0, 2, 3};
    const std::array<std::uint16_t, 6> back{0, 2, 1, 0, 3, 2};
    for (const std::uint16_t index : reversed ? back : front) {
        surface.indices.push_back(static_cast<std::uint16_t>(base + index));
    }
}

[[nodiscard]] assets::Model model_of(assets::Submesh surface, const std::string& name) {
    assets::Mesh mesh;
    mesh.name = name;
    mesh.bounds_min = {-9.0F, -9.0F, 0.0F};
    mesh.bounds_max = {9.0F, 9.0F, 3.0F};
    mesh.submeshes.push_back(std::move(surface));
    assets::Model model;
    model.meshes.push_back(std::move(mesh));
    return model;
}

[[nodiscard]] assets::Model quad(const float x0, const float x1, const float y0, const float y1, const float z,
    const bool reversed = false) {
    assets::Submesh surface;
    surface.shader = "synthetic";
    add_quad(surface, x0, x1, y0, y1, z, reversed);
    return model_of(std::move(surface), "synthetic_quad");
}

[[nodiscard]] presentation::MaterialDescription legacy(const std::string& program, const std::string& technique,
    const std::string& pass_name, const presentation::RenderPass pass,
    std::vector<presentation::MaterialBinding> bindings = {}) {
    return presentation::MaterialDescription{
        .schema_version = presentation::MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = pass,
        .program = program,
        .technique = technique,
        .pass_name = pass_name,
        .bindings = std::move(bindings),
    };
}

[[nodiscard]] presentation::MaterialDescription additive(std::vector<presentation::MaterialBinding> bindings) {
    return legacy("MeshAdditive.fx", "t0", "t0_p0", presentation::RenderPass::transparent, std::move(bindings));
}

[[nodiscard]] presentation::MaterialDescription offset(std::vector<presentation::MaterialBinding> bindings) {
    return legacy("MeshAdditiveOffset.fx", "t0", "t0_p0", presentation::RenderPass::transparent, std::move(bindings));
}

[[nodiscard]] presentation::MaterialDescription solid(const presentation::MaterialBinding& color) {
    return legacy("MeshSolidColor.fx", "t0", "t0_p0", presentation::RenderPass::opaque, {color});
}

[[nodiscard]] presentation::MaterialDescription colorize(const assets::Vec4f& colorization, const float specular) {
    return legacy("MeshGlossColorize.fx", "sph_t0", "sph_t0_p0", presentation::RenderPass::opaque, {
        {"Emissive", assets::Vec4f{0.0F, 0.0F, 0.0F, 1.0F}},
        {"Diffuse", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}},
        {"Specular", assets::Vec4f{specular, specular, specular, 1.0F}},
        {"Shininess", 32.0F},
        {"Colorization", colorization},
        {"BaseTexture", std::string("synthetic_ship.tga")},
        {"GlossTexture", std::string("SYNTHETIC_SHIP.TGA")},
    });
}

[[nodiscard]] presentation::MaterialDescription meshgloss_reference() {
    return legacy("MeshGloss.fx", "sph_t0", "sph_t0_p0", presentation::RenderPass::opaque, {
        {"Emissive", assets::Vec4f{0.0F, 0.0F, 0.0F, 1.0F}},
        {"Diffuse", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}},
        {"Specular", assets::Vec4f{0.0F, 0.0F, 0.0F, 1.0F}},
        {"BaseTexture", std::string("synthetic_ship.tga")},
    });
}

// The backdrop destination for additive draws: an unlit, opaque quad.
constexpr std::array<float, 4> backdrop_color{40.0F / 255.0F, 60.0F / 255.0F, 80.0F / 255.0F, 1.0F};
const std::array<Rgba, 4> additive_quadrants{{
    {200, 100, 50, 255}, {100, 180, 30, 0}, {20, 60, 160, 128}, {150, 40, 120, 32}}};
const std::array<Rgba, 4> ship_quadrants{{
    {180, 120, 60, 0}, {180, 120, 60, 255}, {0, 160, 90, 255}, {255, 100, 50, 255}}};

// Named sample points (source X, Y) on the z = 1 plane.
const std::vector<std::pair<std::string, std::array<float, 2>>> sample_points{
    {"tl", {-2.5F, 2.5F}}, {"tr", {2.5F, 2.5F}}, {"bl", {-2.5F, -2.5F}}, {"br", {2.5F, -2.5F}},
    {"outside", {-7.0F, -7.0F}}};

[[nodiscard]] std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        if (c == '\n') { result += "\\n"; continue; }
        if (static_cast<unsigned char>(c) >= 32) result.push_back(c);
    }
    return result + '"';
}

class EawrLegacyFamilyProbe final : public Node {
    GDCLASS(EawrLegacyFamilyProbe, Node)
protected:
    static void _bind_methods() {}
public:
    void _ready() override;
    void _process(double) override;
private:
    struct Capture { std::string name; std::vector<std::pair<std::string, std::array<double, 3>>> points; };
    struct Step { std::string name; std::function<void()> action; };
    void check(bool condition, const std::string& reason);
    void fresh(bool with_lighting = false, bool shadows = false);
    void upload(sim::AssetId asset, const assets::Model& model, const assets::Texture& texture,
        const presentation::MaterialDescription& material, const std::string& label);
    void submit(const std::vector<sim::AssetId>& assets);
    void backdrop();
    void scene(const std::string& name, std::function<void()> setup);
    void capture(const std::string& name);
    void fail_closed();
    void finish();

    SubViewport* viewport_{};
    Node3D* host_{};
    std::unique_ptr<GodotRenderer> renderer_;
    presentation::FixedCamera camera_{};
    std::vector<Step> steps_;
    std::vector<Capture> captures_;
    std::vector<std::pair<std::string, std::string>> rejections_;
    std::vector<std::pair<std::string, std::string>> submitted_passes_;
    std::vector<std::string> failures_;
    std::string report_path_;
    std::string output_dir_;
    std::size_t step_{};
    std::size_t shadow_receiving_{};
    std::size_t shadow_variant_failures_{};
    std::size_t resources_after_rejections_{};
    int wait_frames_{};
    int frames_{};
    std::uint64_t snapshot_{};
    bool finished_{};
};

void EawrLegacyFamilyProbe::check(const bool condition, const std::string& reason) {
    if (!condition) failures_.push_back(reason);
}

void EawrLegacyFamilyProbe::fresh(const bool with_lighting, const bool shadows) {
    renderer_.reset();
    renderer_ = std::make_unique<GodotRenderer>(*host_);
    renderer_->set_camera(camera_);
    if (!with_lighting) return;
    // A synthetic light chosen so the half vector of the renderer's fixed
    // legacy eye position and the light is the +Y render normal of the quads:
    // specular is then at its maximum where the gloss mask is 1.
    GodotRenderer::LightingState lighting;
    const std::array<float, 3> toward{0.0F, 0.37268F, -0.92796F};
    const auto channel = [&toward](const float ambient, const float directional) {
        const auto matrix = eawr::presentation::godot_backend::meshgloss_hemisphere_matrix(ambient, directional, toward);
        std::array<float, 16> flat{};
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t row = 0; row < 4; ++row) flat[column * 4 + row] = matrix.columns[column][row];
        }
        return flat;
    };
    lighting.sph = {channel(0.1F, 0.5F), channel(0.1F, 0.5F), channel(0.1F, 0.5F)};
    lighting.toward_light = toward;
    lighting.specular = {2.0F, 1.88F, 1.72F};
    lighting.shadows = shadows;
    lighting.shadow_max_distance = 60.0F;
    lighting.shadow_atlas_size = 1024;
    renderer_->set_lighting(lighting);
}

void EawrLegacyFamilyProbe::upload(const sim::AssetId asset, const assets::Model& model,
    const assets::Texture& texture, const presentation::MaterialDescription& material, const std::string& label) {
    // MeshGlossColorize's GlossTexture is its own upload binding since #80; the probe binds the
    // colour texture to it, as its GlossTexture names the BaseTexture.
    std::vector<GodotRenderer::BindingTexture> sampled;
    if (material.program == "MeshGlossColorize.fx") sampled.push_back({"GlossTexture", texture});
    const auto result = renderer_->upload(asset, model, texture, material, sampled);
    check(result.has_value(), label + " upload: "
        + (result ? std::string() : result.error().code + " " + result.error().message));
}

void EawrLegacyFamilyProbe::submit(const std::vector<sim::AssetId>& assets) {
    std::vector<sim::RenderInstance> instances;
    sim::EntityId entity = 1;
    for (const sim::AssetId asset : assets) instances.push_back({entity++, asset, identity()});
    renderer_->submit(std::make_shared<const sim::RenderSnapshot>(++snapshot_, std::move(instances)));
    check(renderer_->instance_count() == assets.size(), "instance count after submit");
}

void EawrLegacyFamilyProbe::backdrop() {
    upload(100, quad(-9.0F, 9.0F, -9.0F, 9.0F, 0.0F), solid_texture({0, 0, 0, 255}),
        solid({"Color", assets::Vec4f{backdrop_color[0], backdrop_color[1], backdrop_color[2], 1.0F}}),
        "backdrop");
}

void EawrLegacyFamilyProbe::scene(const std::string& name, std::function<void()> setup) {
    steps_.push_back({name + "-setup", std::move(setup)});
    steps_.push_back({name, [this, name] { capture(name); }});
}

void EawrLegacyFamilyProbe::capture(const std::string& name) {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid() && image->get_width() == viewport_size && image->get_height() == viewport_size,
        name + " viewport dimensions");
    if (!image.is_valid()) return;
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    if (!output_dir_.empty()) {
        check(image->save_png(String((output_dir_ + "/" + name + ".png").c_str())) == OK, name + " PNG save");
    }
    Capture result{name, {}};
    for (const auto& [label, point] : sample_points) {
        // Perspective projection of the z = 1 plane; screen up is source +Y.
        const float depth = eye_height - 1.0F;
        const float ndc_x = point[0] / (depth * tan_half);
        const float ndc_y = point[1] / (depth * tan_half);
        const int cx = static_cast<int>(std::lround((ndc_x + 1.0F) * 0.5F * viewport_size));
        const int cy = static_cast<int>(std::lround((1.0F - ndc_y) * 0.5F * viewport_size));
        std::array<double, 3> sum{};
        int count = 0;
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                const std::int64_t at = (static_cast<std::int64_t>(y) * viewport_size + x) * 4;
                for (std::size_t c = 0; c < 3; ++c) sum[c] += pixels[at + static_cast<std::int64_t>(c)];
                ++count;
            }
        }
        for (double& value : sum) value /= count;
        result.points.push_back({label, sum});
    }
    captures_.push_back(std::move(result));
}

// Every rejected selector fails at upload with one EAWR-RENDER-0001 and
// leaves no resource behind.
void EawrLegacyFamilyProbe::fail_closed() {
    fresh();
    const auto model = quad(-5.0F, 5.0F, -5.0F, 5.0F, 1.0F);
    const auto texture = quadrant_texture(additive_quadrants);
    const std::vector<std::pair<std::string, presentation::MaterialDescription>> cases{
        {"additive_fixed_function", legacy("MeshAdditive.fx", "t1", "t1_p0", presentation::RenderPass::transparent)},
        {"additive_opaque_pass", legacy("MeshAdditive.fx", "t0", "t0_p0", presentation::RenderPass::opaque)},
        {"additive_scalar_color", additive({{"Color", 0.5F}})},
        {"vertex_additive_scalar_rate", legacy("MeshAdditiveVColor.fx", "t0", "t0_p0",
            presentation::RenderPass::transparent, {{"UVScrollRate", 0.5F}})},
        {"offset_texture_offset", offset({{"UVOffset", std::string("offset.tga")}})},
        {"solid_transparent_pass", legacy("MeshSolidColor.fx", "t0", "t0_p0", presentation::RenderPass::transparent)},
        {"colorize_distinct_gloss", legacy("MeshGlossColorize.fx", "sph_t0", "sph_t0_p0",
            presentation::RenderPass::opaque, {{"BaseTexture", std::string("a.tga")},
                {"GlossTexture", std::string("a_gloss.tga")}})},
        {"colorize_fixed_function", legacy("MeshGlossColorize.fx", "sph_t1", "sph_t1_p0",
            presentation::RenderPass::opaque)},
        {"aldefault", legacy("alDefault.fx", "t0", "t0_p0", presentation::RenderPass::opaque)},
    };
    sim::AssetId asset = 200;
    for (const auto& [label, material] : cases) {
        const auto result = renderer_->upload(asset++, model, texture, material);
        check(!result.has_value(), label + " must be rejected");
        rejections_.push_back({label, result ? std::string("accepted")
                                             : result.error().code + " " + result.error().message});
    }
    resources_after_rejections_ = renderer_->resources().size();
}

void EawrLegacyFamilyProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == String("--eawr-legacy-report")) report_path_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-legacy-output")) output_dir_ = args[i + 1].utf8().get_data();
    }
    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(viewport_size, viewport_size));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);
    camera_.width = viewport_size;
    camera_.height = viewport_size;
    camera_.vertical_fov_degrees = fov;
    camera_.near_plane = 0.5F;
    camera_.far_plane = 100.0F;
    camera_.eye = {0.0F, eye_height, 0.0F};
    camera_.target = {0.0F, 0.0F, 0.0F};
    camera_.up = {0.0F, 0.0F, -1.0F};

    const auto quadrants = quadrant_texture(additive_quadrants);
    const auto center = [] { return quad(-5.0F, 5.0F, -5.0F, 5.0F, 1.0F); };
    const std::vector<presentation::MaterialBinding> authored{
        {"BaseTexture", std::string("synthetic_glow.tga")},
        {"UVScrollRate", assets::Vec4f{0.25F, -0.5F, 7.0F, 9.0F}},
        {"Color", assets::Vec4f{0.5F, 0.75F, 1.0F, 0.2F}}};

    steps_.push_back({"fail-closed", [this] { fail_closed(); }});
    scene("empty", [this] { fresh(); submit({}); });
    scene("backdrop", [this] { fresh(); backdrop(); submit({100}); });
    scene("solid", [this] {
        fresh();
        upload(1, quad(-5.0F, 0.0F, -5.0F, 5.0F, 1.0F), solid_texture({0, 0, 0, 255}),
            solid({"Color", assets::Vec4f{1.5F, -0.5F, 0.5F, 0.482F}}), "solid over-unit float4");
        upload(2, quad(0.0F, 5.0F, -5.0F, 5.0F, 1.0F), solid_texture({0, 0, 0, 255}),
            solid({"Color", assets::Vec3f{0.25F, 0.5F, 0.75F}}), "solid float3");
        submit({1, 2});
    });
    scene("additive", [this, quadrants, center, authored] {
        fresh(); backdrop();
        upload(1, center(), quadrants, additive(authored), "additive");
        submit({100, 1});
    });
    scene("additive_zero_rate", [this, quadrants, center] {
        fresh(); backdrop();
        upload(1, center(), quadrants, additive({{"UVScrollRate", assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
            {"Color", assets::Vec4f{0.5F, 0.75F, 1.0F, 0.2F}}}), "additive zero rate");
        submit({100, 1});
    });
    scene("additive_defaults", [this, quadrants, center] {
        fresh(); backdrop();
        upload(1, center(), quadrants, additive({}), "additive defaults");
        submit({100, 1});
    });
    // AVC-02..04: use actual uploaded vertex colour, ignore both alpha and
    // the ordinary additive family's Color, and advance the authored UV clock.
    const auto vertex_model = [center](const float alpha) {
        auto result = center();
        for (auto& vertex : result.meshes.front().submeshes.front().vertices) {
            vertex.color = {0.5F, 0.75F, 1.0F, alpha};
        }
        return result;
    };
    const auto vertex_material = legacy("MeshAdditiveVColor.fx", "t0", "t0_p0",
        presentation::RenderPass::transparent, {
            {"UVScrollRate", assets::Vec4f{1.0F, 0.0F, 7.0F, 9.0F}},
            {"eawr_effect_time", 0.0F},
            {"Color", assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}}});
    scene("vertex_additive", [this, quadrants, vertex_model, vertex_material] {
        fresh(); backdrop();
        upload(1, vertex_model(0.0F), quadrants, vertex_material, "vertex additive alpha zero");
        submit({100, 1});
    });
    scene("vertex_additive_alpha_one", [this, quadrants, vertex_model, vertex_material] {
        fresh(); backdrop();
        upload(1, vertex_model(1.0F), quadrants, vertex_material, "vertex additive alpha one");
        submit({100, 1});
    });
    scene("vertex_additive_scrolled", [this, quadrants, vertex_model, vertex_material] {
        fresh(); backdrop();
        upload(1, vertex_model(0.0F), quadrants, vertex_material, "vertex additive clock");
        check(renderer_->set_material_scalar(1, "eawr_effect_time", 0.5F).has_value(), "set vertex additive clock");
        submit({100, 1});
    });
    scene("additive_culled", [this, quadrants, authored] {
        fresh(); backdrop();
        upload(1, quad(-5.0F, 5.0F, -5.0F, 5.0F, 1.0F, true), quadrants, additive(authored), "additive reversed");
        submit({100, 1});
    });
    scene("additive_layers", [this] {
        fresh(); backdrop();
        // One draw, nearer layer first in the index buffer: without depth
        // writes the farther layer still adds.
        assets::Submesh surface;
        surface.shader = "synthetic";
        add_quad(surface, -5.0F, 5.0F, -5.0F, 5.0F, 1.5F, false);
        add_quad(surface, -5.0F, 5.0F, -5.0F, 5.0F, 1.0F, false);
        upload(1, model_of(std::move(surface), "synthetic_layers"), solid_texture({100, 100, 100, 255}),
            additive({{"Color", assets::Vec3f{0.5F, 0.5F, 0.5F}}}), "additive layers");
        submit({100, 1});
    });
    scene("occluder_only", [this] {
        fresh(); backdrop();
        upload(3, quad(-6.0F, 0.0F, -6.0F, 6.0F, 2.0F), solid_texture({30, 30, 220, 255}),
            meshgloss_reference(), "depth-writing MeshGloss occluder");
        submit({100, 3});
    });
    scene("additive_occluded", [this, quadrants, center, authored] {
        fresh(); backdrop();
        upload(3, quad(-6.0F, 0.0F, -6.0F, 6.0F, 2.0F), solid_texture({30, 30, 220, 255}),
            meshgloss_reference(), "depth-writing MeshGloss occluder");
        upload(1, center(), quadrants, additive(authored), "occluded additive");
        submit({100, 3, 1});
    });
    scene("solid_over_additive", [this, quadrants, authored] {
        fresh();
        upload(2, quad(-5.0F, 5.0F, -5.0F, 5.0F, 2.0F), solid_texture({0, 0, 0, 255}),
            solid({"Color", assets::Vec4f{0.2F, 0.2F, 0.2F, 1.0F}}), "front solid");
        upload(1, quad(-5.0F, 5.0F, -5.0F, 5.0F, 1.0F), quadrants, additive(authored), "additive behind solid");
        submit({2, 1});
        for (const auto& item : renderer_->submission_evidence()) {
            submitted_passes_.push_back({item.asset_id == 2 ? "MeshSolidColor.fx" : "MeshAdditive.fx",
                std::string(presentation::to_string(item.pass))});
        }
    });
    scene("offset", [this, quadrants, center] {
        fresh(); backdrop();
        upload(1, center(), quadrants, offset({{"UVOffset", assets::Vec4f{0.5F, 0.0F, 3.0F, 4.0F}},
            {"Color", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}}}), "offset");
        submit({100, 1});
    });
    scene("offset_zero", [this, quadrants, center] {
        fresh(); backdrop();
        upload(1, center(), quadrants, offset({{"UVOffset", assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
            {"Color", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}}}), "offset zero");
        submit({100, 1});
    });
    const auto ship = quadrant_texture(ship_quadrants);
    scene("gloss_reference", [this, ship, center] {
        fresh(true);
        upload(4, center(), ship, meshgloss_reference(), "MeshGloss reference");
        submit({4});
    });
    scene("colorize_white", [this, ship, center] {
        fresh(true);
        upload(5, center(), ship, colorize({1.0F, 1.0F, 1.0F, 1.0F}, 0.0F), "colorize white");
        submit({5});
    });
    scene("colorize_black", [this, ship, center] {
        fresh(true);
        upload(5, center(), ship, colorize({0.0F, 0.0F, 0.0F, 1.0F}, 0.0F), "colorize black");
        submit({5});
    });
    scene("colorize_specular", [this, ship, center] {
        fresh(true);
        upload(5, center(), ship, colorize({1.0F, 1.0F, 1.0F, 1.0F}, 1.0F), "colorize specular");
        submit({5});
    });
    scene("additive_shadows_on", [this, quadrants, center, authored, ship] {
        fresh(true, true);
        backdrop();
        upload(1, center(), quadrants, additive(authored), "additive under shadows");
        upload(6, center(), quadrants,
            offset({{"Color", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}}}), "offset under shadows");
        upload(7, center(), ship, colorize({1.0F, 1.0F, 1.0F, 1.0F}, 0.0F), "colorize under shadows");
        // Only the lit MeshGlossColorize compiles as a receiving variant; the
        // unlit families stay unshaded, which is not a variant failure. The
        // offset and colorize uploads prove compilation and are not drawn.
        shadow_receiving_ = renderer_->shadow_receiving_materials();
        shadow_variant_failures_ = renderer_->shadow_variant_failures();
        submit({100, 1});
    });
    set_process(true);
}

void EawrLegacyFamilyProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (++frames_ > 600) { check(false, "frame budget exhausted"); finish(); return; }
    if (wait_frames_ > 0 && --wait_frames_ > 0) return;
    if (step_ < steps_.size()) {
        steps_[step_++].action();
        wait_frames_ = 5;
        return;
    }
    finish();
}

void EawrLegacyFamilyProbe::finish() {
    finished_ = true;
    const auto backend = renderer_ ? renderer_->backend_info() : presentation::BackendInfo{};
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "{\"schema\":\"eawr-legacy-family-probe-v1\",\"status\":"
        << quote(failures_.empty() ? "legacy_family_probe_passed" : "legacy_family_probe_failed")
        << ",\"rendering_method\":" << quote(backend.rendering_method)
        << ",\"adapter\":" << quote(backend.adapter_vendor + " " + backend.adapter_name)
        << ",\"driver_api\":" << quote(backend.driver_api)
        << ",\"steps_completed\":" << step_ << ",\"steps_total\":" << steps_.size()
        << ",\"shadow_receiving_materials\":" << shadow_receiving_
        << ",\"shadow_variant_failures\":" << shadow_variant_failures_
        << ",\"resources_after_rejections\":" << resources_after_rejections_
        << ",\"rejections\":{";
    for (std::size_t i = 0; i < rejections_.size(); ++i) {
        out << (i ? "," : "") << quote(rejections_[i].first) << ":" << quote(rejections_[i].second);
    }
    out << "},\"submitted_passes\":{";
    for (std::size_t i = 0; i < submitted_passes_.size(); ++i) {
        out << (i ? "," : "") << quote(submitted_passes_[i].first) << ":" << quote(submitted_passes_[i].second);
    }
    out << "},\"captures\":{";
    for (std::size_t i = 0; i < captures_.size(); ++i) {
        out << (i ? "," : "") << quote(captures_[i].name) << ":{";
        for (std::size_t p = 0; p < captures_[i].points.size(); ++p) {
            const auto& [label, rgb] = captures_[i].points[p];
            out << (p ? "," : "") << quote(label) << ":[" << rgb[0] << "," << rgb[1] << "," << rgb[2] << "]";
        }
        out << "}";
    }
    out << "},\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << quote(failures_[i]);
    out << "]}\n";
    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(report_path_, std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    renderer_.reset();
    UtilityFunctions::print(failures_.empty() ? "EAWR legacy family probe passed" : "EAWR legacy family probe failed");
    get_tree()->quit(failures_.empty() && written ? 0 : 1);
}

void initialize(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrLegacyFamilyProbe);
}
void uninitialize(const ModuleInitializationLevel) {}
} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_legacy_family_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

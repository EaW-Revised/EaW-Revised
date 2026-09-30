#include "fog_renderer_probe.hpp"

namespace eawr_fog_renderer_probe {

// The terrain shader with a nonstandard definition: bound by default, a white
// default texture and a default mapping covering the whole terrain plate, so a
// consumer left at its shader defaults would draw the unattenuated surface
// everywhere instead of dark.
[[nodiscard]] std::string bound_default_program() {
    std::string source(terrain_program);
    for (const auto& [from, to] : {
             std::pair<std::string_view, std::string_view>{"uniform bool eawr_fog_bound = false;",
                 "uniform bool eawr_fog_bound = true;"},
             {"filter_nearest, repeat_disable;", "filter_nearest, repeat_disable, hint_default_white;"},
             {"uniform vec2 eawr_fog_origin = vec2(0.0);", "uniform vec2 eawr_fog_origin = vec2(-5.0, -2.0);"},
             {"uniform vec2 eawr_fog_extent = vec2(1.0);", "uniform vec2 eawr_fog_extent = vec2(12.0, 6.0);"}}) {
        const std::size_t at = source.find(from);
        if (at == std::string::npos) std::abort();
        source.replace(at, from.size(), to);
    }
    return source;
}

[[nodiscard]] std::string sampler_case_program(const SamplerCase& sampler) {
    return "shader_type spatial;\nrender_mode unshaded;\n" + std::string(sampler.declaration) + R"GODOT(
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
void fragment() {
    vec2 uv = eawr_fog_origin + eawr_fog_extent + eawr_fog_size;
    float bound = eawr_fog_bound ? 1.0 : 0.0;
    ALBEDO = vec3(bound + )GODOT" + std::string(sampler.sample) + ");\n}\n";
}

// Pre-existing fog overrides carried by the asset's own material bindings: a
// lit window over its white texture while fog is disabled, at source
// [-4, -1) x [-1, 3), clear of the unit plate. Material bindings have no vec2
// kind, so the mapping values are Vec3 (Godot stores the Variant as given;
// restoration is checked on that stored value and on pixels).
[[nodiscard]] std::vector<presentation::MaterialBinding> pre_existing_overrides() {
    return {
        {"eawr_fog_bound", std::int32_t{1}},
        {"eawr_fog_texture", std::string("BaseTexture")},
        {"eawr_fog_origin", eawr::assets::Vec3f{-4.0F, -1.0F, 0.0F}},
        {"eawr_fog_extent", eawr::assets::Vec3f{3.0F, 4.0F, 0.0F}},
        {"eawr_fog_size", eawr::assets::Vec3f{2.0F, 2.0F, 0.0F}},
    };
}

[[nodiscard]] std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] sim::math::Fixed fixed(const double value) {
    return sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * one)));
}

// Row-major fixed affine: rows[r] = (basis row r, translation r).
[[nodiscard]] sim::math::Mat3x4 affine(const std::array<std::array<double, 4>, 3>& rows) {
    sim::math::Mat3x4 result;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) result.rows[r][c] = fixed(rows[r][c]);
    }
    return result;
}

[[nodiscard]] Transform3D godot_transform(const sim::math::Mat3x4& matrix) {
    // Exactly the renderer's fixed-to-float bridge and matrix construction.
    const sim::RenderSnapshot snapshot(0, {sim::RenderInstance{1, 1, matrix}});
    const auto m = presentation::adapt_snapshot(snapshot).front().column_major;
    return Transform3D(Basis(Vector3(m[0], m[1], m[2]), Vector3(m[4], m[5], m[6]), Vector3(m[8], m[9], m[10])),
        Vector3(m[12], m[13], m[14]));
}

// A flat plate in asset XY (render XZ after the adapter's axis conversion),
// subdivided into nx * ny quads with both windings (the BatchMeshGloss
// adapter culls back faces).
[[nodiscard]] eawr::assets::Model plate(const double half_x, const double half_y, const int nx, const int ny) {
    eawr::assets::Submesh submesh;
    submesh.shader = "synthetic";
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            eawr::assets::Vertex vertex;
            vertex.position = {static_cast<float>(-half_x + 2.0 * half_x * i / nx),
                static_cast<float>(-half_y + 2.0 * half_y * j / ny), 0.0F};
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.tangent = {1.0F, 0.0F, 0.0F};
            vertex.binormal = {0.0F, 1.0F, 0.0F};
            vertex.texcoord[0] = {static_cast<float>(i) / nx, static_cast<float>(j) / ny};
            vertex.color = {1.0F, 1.0F, 1.0F, 1.0F};
            submesh.vertices.push_back(vertex);
        }
    }
    const auto index = [nx](const int i, const int j) { return static_cast<std::uint16_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::array<std::uint16_t, 4> q{index(i, j), index(i + 1, j), index(i + 1, j + 1), index(i, j + 1)};
            for (const std::uint16_t v : {q[0], q[1], q[2], q[0], q[2], q[3], q[0], q[2], q[1], q[0], q[3], q[2]}) {
                submesh.indices.push_back(v);
            }
        }
    }
    eawr::assets::Mesh mesh;
    mesh.name = "synthetic_plate";
    mesh.submeshes.push_back(std::move(submesh));
    eawr::assets::Model model;
    model.meshes.push_back(std::move(mesh));
    return model;
}

[[nodiscard]] eawr::assets::Texture solid_texture(const std::array<std::uint8_t, 4> rgba) {
    eawr::assets::Texture texture;
    texture.width = 4;
    texture.height = 4;
    texture.format = eawr::assets::PixelFormat::rgba8;
    eawr::assets::MipLevel mip{4, 4, 16, {}};
    for (int i = 0; i < 16; ++i) {
        for (const std::uint8_t channel : rgba) mip.bytes.push_back(static_cast<std::byte>(channel));
    }
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] presentation::MaterialDescription batch_mesh_gloss(std::vector<presentation::MaterialBinding> bindings) {
    presentation::MaterialDescription material;
    material.route = presentation::MaterialRoute::legacy_effect;
    material.pass = presentation::RenderPass::opaque;
    material.program = "BatchMeshGloss.fx";
    material.technique = "sph_t1";
    material.pass_name = "sph_t1_p0";
    material.bindings = std::move(bindings);
    return material;
}

[[nodiscard]] presentation::MaterialDescription batch_mesh_alpha(const float rgb_scale) {
    presentation::MaterialDescription material;
    material.route = presentation::MaterialRoute::legacy_effect;
    material.pass = presentation::RenderPass::transparent;
    material.program = "BatchMeshAlpha.fx";
    material.technique = "sph_t1";
    material.pass_name = "sph_t1_p0";
    material.bindings = {{"Diffuse", eawr::assets::Vec4f{rgb_scale, rgb_scale, rgb_scale, 0.6F}}};
    return material;
}

[[nodiscard]] presentation::MaterialDescription modern(const std::string_view program,
    std::vector<presentation::MaterialBinding> bindings) {
    presentation::MaterialDescription material;
    material.route = presentation::MaterialRoute::modern_spatial;
    material.pass = presentation::RenderPass::opaque;
    material.program = std::string(program);
    material.bindings = std::move(bindings);
    return material;
}

sim_fog::FogGridDesc base_desc(const std::uint64_t revision, const std::uint32_t team) {
    return sim_fog::FogGridDesc{
        .team_id = team, .width = 3, .height = 2,
        .origin_x_raw = -5 * one / 2, .origin_y_raw = one / 4,
        .cell_x_raw = 2 * one, .cell_y_raw = 3 * one / 4,
        .encoding = sim_fog::encoding_linear_u8_attenuation, .revision = revision,
    };
}

[[nodiscard]] std::string readiness_name(const GodotRenderer::FogReadiness readiness) {
    switch (readiness) {
    case GodotRenderer::FogReadiness::disabled: return "disabled";
    case GodotRenderer::FogReadiness::awaiting_grid: return "awaiting_grid";
    case GodotRenderer::FogReadiness::ready: return "ready";
    case GodotRenderer::FogReadiness::rejected: return "rejected";
    }
    return "unknown";
}

[[nodiscard]] bool same_resources(
    const std::vector<presentation::ResourceReference>& left,
    const std::vector<presentation::ResourceReference>& right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const auto& a, const auto& b) {
        return a.asset_id == b.asset_id && a.references == b.references;
    });
}

} // namespace eawr_fog_renderer_probe

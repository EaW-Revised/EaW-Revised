#include "legacy_family_support.hpp"

namespace legacy_family_test_support {

void binding_contracts() {
    MaterialDescription vertex_additive = selector(legacy::mesh_additive_vcolor::family, RenderPass::transparent);
    check(value_of<std::array<float, 2>>(legacy::uniforms(vertex_additive), "eawr_uv_scroll_rate")
            == std::array<float, 2>{0.0F, 0.0F}, "AVC-02: absent scroll rate is zero");
    vertex_additive.bindings = {{"UVScrollRate", eawr::assets::Vec4f{0.0F, 0.04F, 7.0F, 9.0F}},
        {"Color", std::string("ignored material colour")}};
    check(rejection(vertex_additive).empty()
            && value_of<std::array<float, 2>>(legacy::uniforms(vertex_additive), "eawr_uv_scroll_rate")
                == std::array<float, 2>{0.0F, 0.04F}
            && value_of<float>(legacy::uniforms(vertex_additive), "eawr_effect_time") == 0.0F,
        "AVC-02/03: authored rate uses xy, held clock starts at zero, material Color is unread");
    vertex_additive.bindings.push_back({"eawr_effect_time", 0.5F});
    check(value_of<float>(legacy::uniforms(vertex_additive), "eawr_effect_time") == 0.5F,
        "AVC-02: a retained scalar clock survives material reconfiguration");
    vertex_additive.bindings[0].value = 1.0F;
    check(!rejection(vertex_additive).empty(), "AVC-02: scalar scroll rate is rejected");
    vertex_additive.bindings[0].value = eawr::assets::Vec4f{0.0F, 0.04F, 0.0F, std::numeric_limits<float>::quiet_NaN()};
    check(!rejection(vertex_additive).empty(), "AVC-02: nonfinite scroll rate is rejected");
    vertex_additive.bindings = {{"UVScrollRate", eawr::assets::Vec3f{0.25F, -0.5F, 0.0F}}};
    vertex_additive.bindings.push_back(vertex_additive.bindings.front());
    check(!rejection(vertex_additive).empty(), "AVC-02: duplicate scroll rate is rejected");
    const auto vertex = legacy::mesh_additive_vcolor::reference_vertex({0.25F, 0.5F, 0.75F, 0.0F}, {2.0F, 1.0F, 4.0F, 0.5F});
    check(vertex == std::array<float, 3>{0.25F, 0.25F, 1.0F}
            && vertex == legacy::mesh_additive_vcolor::reference_vertex({0.25F, 0.5F, 0.75F, 1.0F}, {2.0F, 1.0F, 4.0F, 0.5F}),
        "AVC-03: light-scale alpha dims RGB, vertex alpha is ignored, overbright output saturates");
    check(legacy::mesh_additive_vcolor::shader_source.find("COLOR.rgb *") != std::string_view::npos
            && legacy::mesh_additive_vcolor::shader_source.find("COLOR.a") == std::string_view::npos,
        "AVC-03: the GPU source reads vertex RGB and ignores vertex alpha");
    const legacy::Family& additive = legacy::mesh_additive::family;
    MaterialDescription material = selector(additive, RenderPass::transparent);

    const auto defaults = legacy::uniforms(material);
    check(value_of<std::array<float, 3>>(defaults, "eawr_color") == std::array<float, 3>{1.0F, 1.0F, 1.0F}
            && value_of<std::array<float, 2>>(defaults, "eawr_uv_scroll_rate") == std::array<float, 2>{0.0F, 0.0F}
            && value_of<float>(defaults, "eawr_time") == 0.0F,
        "unauthored MeshAdditive parameters keep the effect initializers and TIME is fixed at 0");

    material.bindings = {
        {"BaseTexture", std::string("w_glow.tga")},
        {"UVScrollRate", eawr::assets::Vec4f{0.25F, -0.5F, 7.0F, 9.0F}},
        {"Color", eawr::assets::Vec4f{0.5F, 0.75F, 1.5F, 0.2F}},
        {"Emissive", eawr::assets::Vec4f{9.0F, 9.0F, 9.0F, 9.0F}},
    };
    check(rejection(material).empty(), "authored float4 MeshAdditive parameters validate; unread ones are ignored");
    const auto authored = legacy::uniforms(material);
    check(value_of<std::array<float, 3>>(authored, "eawr_color") == std::array<float, 3>{0.5F, 0.75F, 1.5F}
            && value_of<std::array<float, 2>>(authored, "eawr_uv_scroll_rate") == std::array<float, 2>{0.25F, -0.5F},
        "MeshAdditive binds Color.rgb unclamped and UVScrollRate.xy exactly");
    material.bindings[2].value = eawr::assets::Vec3f{0.25F, 0.375F, 0.5F};
    check(rejection(material).empty()
            && value_of<std::array<float, 3>>(legacy::uniforms(material), "eawr_color")
                == std::array<float, 3>{0.25F, 0.375F, 0.5F},
        "a float3 Color binds its three components");

    const auto rejected = [&](const MaterialDescription& changed, const std::string_view fragment, const std::string_view why) {
        const std::string message = rejection(changed);
        check(message.starts_with("EAWR-RENDER-0001 legacy material family 'MeshAdditive.fx' ")
                && message.find(fragment) != std::string::npos,
            why);
        check(!legacy::shader_source(changed), std::string(why) + " (upload selection)");
    };
    MaterialDescription changed = material;
    changed.bindings[2].value = 1.0F;
    rejected(changed, "binds parameter 'Color' as a scalar, not a float3 or float4", "a scalar Color fails closed");
    changed.bindings[2].value = std::int32_t{1};
    rejected(changed, "as an integer", "an integer Color fails closed");
    changed.bindings[2].value = std::string("red.tga");
    rejected(changed, "as a texture", "a texture Color fails closed");
    changed = material;
    changed.bindings[1].value = eawr::assets::Vec4f{0.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()};
    rejected(changed, "binds a non-finite component in parameter 'UVScrollRate'",
        "a non-finite unread UVScrollRate component fails closed");
    changed = material;
    changed.bindings[2].value = eawr::assets::Vec4f{std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 1.0F};
    rejected(changed, "non-finite component in parameter 'Color'", "an infinite Color fails closed");
    changed = material;
    changed.bindings.push_back({"Color", eawr::assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}});
    rejected(changed, "binds parameter 'Color' 2 times", "a duplicated Color fails closed");
    changed = material;
    changed.bindings[1].value = eawr::assets::Vec3f{0.5F, 0.25F, 0.0F};
    check(rejection(changed).empty()
            && value_of<std::array<float, 2>>(legacy::uniforms(changed), "eawr_uv_scroll_rate")
                == std::array<float, 2>{0.5F, 0.25F},
        "a float3 UVScrollRate binds its first two components");

    MaterialDescription offset = selector(legacy::mesh_additive_offset::family, RenderPass::transparent);
    offset.bindings = {{"UVOffset", eawr::assets::Vec4f{0.5F, -0.25F, 3.0F, 4.0F}},
        {"Color", eawr::assets::Vec4f{0.2F, 0.4F, 0.6F, 1.0F}}};
    const auto offset_uniforms = legacy::uniforms(offset);
    check(rejection(offset).empty()
            && value_of<std::array<float, 2>>(offset_uniforms, "eawr_uv_offset") == std::array<float, 2>{0.5F, -0.25F}
            && value_of<std::array<float, 3>>(offset_uniforms, "eawr_color") == std::array<float, 3>{0.2F, 0.4F, 0.6F}
            && uniform(offset_uniforms, "eawr_time") == nullptr,
        "MeshAdditiveOffset binds UVOffset.xy and Color.rgb and no time");
    offset.bindings[0].value = 2.0F;
    check(rejection(offset).find("binds parameter 'UVOffset' as a scalar") != std::string::npos,
        "a scalar UVOffset fails closed");

    MaterialDescription solid = selector(legacy::mesh_solid_color::family, RenderPass::opaque);
    check(value_of<std::array<float, 3>>(legacy::uniforms(solid), "eawr_color") == std::array<float, 3>{0.0F, 0.0F, 1.0F},
        "an unauthored MeshSolidColor Color keeps the effect initializer");
    solid.bindings = {{"Color", eawr::assets::Vec4f{0.098F, 0.992F, 0.024F, 0.482F}}};
    check(rejection(solid).empty()
            && value_of<std::array<float, 3>>(legacy::uniforms(solid), "eawr_color")
                == std::array<float, 3>{0.098F, 0.992F, 0.024F},
        "MeshSolidColor binds Color.rgb");
    solid.bindings[0].value = std::string("marker.tga");
    check(rejection(solid).find("'MeshSolidColor.fx' binds parameter 'Color' as a texture") != std::string::npos,
        "a texture MeshSolidColor Color fails closed");
}

void texture_placeholder_contracts() {
    MaterialDescription material = admissible(legacy::bump_colorize::mesh_family, RenderPass::opaque);
    const auto missing = [](std::string_view) -> std::optional<eawr::assets::Texture> { return std::nullopt; };
    const auto normal = godot_backend::family_binding_textures(material, missing);
    check(normal.size() == 1 && normal[0].texture.mips[0].bytes == std::vector<std::byte>{
        std::byte{128}, std::byte{128}, std::byte{255}, std::byte{0}},
        "unresolved NormalTexture is flat with zero gloss alpha");
    // Exercise a gloss binding without admitting the separate #200 family here.
    constexpr legacy::TextureBinding gloss_bindings[]{{"GlossTexture", legacy::TexturePlaceholder::black}};
    material.bindings.push_back({"GlossTexture", std::string("missing-gloss.dds")});
    const auto gloss = godot_backend::family_binding_textures(material, gloss_bindings, missing);
    check(gloss.size() == 1 && gloss[0].texture.mips[0].bytes == std::vector<std::byte>(4, std::byte{0}),
        "unresolved GlossTexture has zero red gloss, not the flat normal's half gloss");
    const auto loaded = godot_backend::family_binding_textures(material, gloss_bindings,
        [](std::string_view) -> std::optional<eawr::assets::Texture> {
            auto texture = godot_backend::family_placeholder_texture(legacy::TexturePlaceholder::black);
            texture.mips[0].bytes[0] = std::byte{231};
            return texture;
        });
    check(loaded[0].texture.mips[0].bytes[0] == std::byte{231}, "resolved gloss is preserved");
}

} // namespace legacy_family_test_support

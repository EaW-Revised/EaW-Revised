#include "legacy_family_support.hpp"
#include "resource_lease_ledger.hpp"
#include "../../../apps/viewer/src/effect_clock_assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
            && value_of<float>(defaults, "eawr_effect_time") == 0.0F,
        "unauthored MeshAdditive parameters keep the effect initializers and the clock starts at 0");

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
    check(legacy::reads_effect_time(material) && legacy::reads_effect_time(vertex_additive),
        "A-12/AVC-05: both additive passes subscribe to the scene effect clock");
    for (const float time : {0.0F, 2.1F}) {
        MaterialDescription timed = material;
        timed.bindings.push_back({"eawr_effect_time", time});
        const auto mapped = legacy::uniforms(timed);
        check(rejection(timed).empty() && value_of<float>(mapped, "eawr_effect_time") == time,
            "A-12: the supplied scene clock survives upload and material reconfiguration");
        const auto uv = legacy::mesh_additive::reference_uv({0.125F, 0.5F},
            value_of<std::array<float, 2>>(mapped, "eawr_uv_scroll_rate"), value_of<float>(mapped, "eawr_effect_time"));
        check(near(uv[0], 0.125F + time * 0.25F) && near(uv[1], 0.5F - time * 0.5F),
            "A-03/A-12: time in seconds scales the authored xy rate without a frame multiplier");
        check(legacy::mesh_additive::reference_uv({0.125F, 0.5F}, {0.0F, 0.0F}, time)
                == std::array<float, 2>{0.125F, 0.5F}, "A-03: zero-rate materials are unchanged at either time");
    }
    MaterialDescription stationary = selector(additive, RenderPass::transparent);
    check(!legacy::reads_effect_time(stationary), "A-12: an unauthored zero rate needs no per-tick writes");
    stationary.bindings = {{"UVScrollRate", eawr::assets::Vec4f{0.0F, 0.0F, 7.0F, 9.0F}}};
    check(!legacy::reads_effect_time(stationary), "A-05: unused rate components do not subscribe to the clock");
    godot_backend::detail::ResourceLeaseLedger leases;
    check(leases.upload(91) && leases.upload(92) && leases.retain(91),
        "the live model and its fog memory share one clocked material lease");
    std::vector<eawr::sim::AssetId> clocks{91, 92};
    check(leases.release(91) == godot_backend::detail::ResourceLeaseLedger::Release::retained,
        "retiring the live model keeps its fog memory's material resident");
    godot_backend::prune_effect_clock_assets(clocks, leases.resources());
    std::vector<eawr::sim::AssetId> writes;
    for (const auto asset : clocks) writes.push_back(asset);
    check(writes == std::vector<eawr::sim::AssetId>{91, 92},
        "the clock keeps writing to the fog-held material after live retirement");
    check(leases.release(91) == godot_backend::detail::ResourceLeaseLedger::Release::free,
        "discarding the fog memory releases its final material lease");
    godot_backend::prune_effect_clock_assets(clocks, leases.resources());
    writes.clear();
    for (const auto asset : clocks) writes.push_back(asset);
    check(writes == std::vector<eawr::sim::AssetId>{92},
        "freeing the fog material stops its clock writes and keeps unrelated live clocks");
    check(leases.release(92) == godot_backend::detail::ResourceLeaseLedger::Release::free,
        "the unrelated model releases its final lease");
    godot_backend::prune_effect_clock_assets(clocks, leases.resources());
    check(clocks.empty(), "final release leaves no stale material clock subscriptions");
    for (const MaterialBinding& binding : std::array<MaterialBinding, 3>{
             MaterialBinding{"eawr_effect_time", eawr::assets::Vec3f{1.0F, 2.0F, 3.0F}},
             MaterialBinding{"eawr_effect_time", std::numeric_limits<float>::infinity()},
             MaterialBinding{"eawr_effect_time", std::numeric_limits<float>::quiet_NaN()}}) {
        MaterialDescription invalid = material;
        invalid.bindings.push_back(binding);
        check(!rejection(invalid).empty(), "A-12: malformed or nonfinite clocks fail closed");
    }
    MaterialDescription duplicate_clock = material;
    duplicate_clock.bindings.insert(duplicate_clock.bindings.end(), {{"eawr_effect_time", 1.0F}, {"eawr_effect_time", 2.0F}});
    check(!rejection(duplicate_clock).empty(), "A-12: duplicate clocks fail closed");
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
    check(!legacy::reads_effect_time(offset)
            && !legacy::reads_effect_time(selector(legacy::mesh_gloss_colorize::family, RenderPass::opaque)),
        "A-12: offset-only additive materials and hull materials do not subscribe to time");
    offset.bindings = {{"UVOffset", eawr::assets::Vec4f{0.5F, -0.25F, 3.0F, 4.0F}},
        {"Color", eawr::assets::Vec4f{0.2F, 0.4F, 0.6F, 1.0F}}};
    const auto offset_uniforms = legacy::uniforms(offset);
    check(rejection(offset).empty()
            && value_of<std::array<float, 2>>(offset_uniforms, "eawr_uv_offset") == std::array<float, 2>{0.5F, -0.25F}
            && value_of<std::array<float, 3>>(offset_uniforms, "eawr_color") == std::array<float, 3>{0.2F, 0.4F, 0.6F}
            && uniform(offset_uniforms, "eawr_effect_time") == nullptr,
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
